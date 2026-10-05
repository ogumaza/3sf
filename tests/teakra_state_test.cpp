// SPDX-License-Identifier: MIT

// Checks Teakra's SaveState and LoadState. Save a DSP part-way through a program, then run to the end. Restore the
// saved state and memory to both the original DSP and a new one without the JIT, which the original uses where the host
// supports it. Both must reproduce the original run's registers, memory, component registers and audio output. Programs
// are random (see teakra_programs.h), or idle in a loop while randomly configured timers, audio ports, command
// registers and DMA raise interrupts.
//
// usage: teakra_state_test [trials [first seed]]

#include <teakra/impl/register.h>
#include <teakra/teakra.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/quiet_stdout.h"
#include "teakra_programs.h"

namespace
{

using Teakra::RegisterState;
using namespace threesf::teakra_test;

using Sample = std::array<int16_t, 2>;

// Instructions for the idle programs.
constexpr uint16_t kBranch = 0x4180; // br <expansion word> always
constexpr uint16_t kNop = 0x0000;
constexpr uint16_t kReturnFromInterrupt = 0x45c0; // reti

// Where the idle programs' vectored interrupts go.
constexpr uint16_t kVectorAddress = 0x0040;

// Where a DSP ended up: what its run threw, if anything, and its registers, memory and audio output since the save.
struct Outcome
{
    std::string threw;
    std::vector<uint8_t> registers;
    std::vector<uint8_t> memory;
    std::vector<Sample> samples;
};

// Runs `dsp` for `cycles` cycles, collecting its audio output in `samples`.
std::string Run(Teakra::Teakra& dsp, uint32_t cycles)
{
    try
    {
        dsp.Run(cycles);
    }
    catch (const std::exception& e)
    {
        return std::string("threw: ") + e.what();
    }

    return {};
}

Outcome Finish(Teakra::Teakra& dsp, uint32_t cycles, std::vector<Sample>& samples)
{
    samples.clear();
    Outcome o;
    o.threw = Run(dsp, cycles);
    const auto* regs = reinterpret_cast<const uint8_t*>(&dsp.GetRegisterState());
    o.registers.assign(regs, regs + sizeof(RegisterState));
    o.memory.assign(dsp.GetDspMemory(), dsp.GetDspMemory() + kMemoryBytes);
    o.samples = samples;

    return o;
}

// Reports how `got` differs from `expected`. Returns false if it does.
bool Compare(uint64_t seed, const char* what, const Outcome& expected, const Outcome& got)
{
    bool same = true;
    if (got.threw != expected.threw)
    {
        std::fprintf(stderr, "seed %llu, %s: \"%s\" where the first run had \"%s\"\n",
                     static_cast<unsigned long long>(seed), what, got.threw.c_str(), expected.threw.c_str());
        same = false;
    }

    // After an exception the interpreter's pc is the one it had when RunOrdinary began, which differs with the JIT, so
    // it isn't compared then (as in teakra_jit_test).
    const RegisterState layout;
    const std::size_t pc_offset = static_cast<std::size_t>(reinterpret_cast<const uint8_t*>(&layout.pc) -
                                                           reinterpret_cast<const uint8_t*>(&layout));
    int reported = 0;
    for (std::size_t i = 0; i < expected.registers.size(); ++i)
    {
        if (!expected.threw.empty() && i >= pc_offset && i < pc_offset + sizeof(RegisterState::pc))
        {
            continue;
        }

        if (got.registers[i] != expected.registers[i])
        {
            if (reported++ < 8)
            {
                std::fprintf(stderr, "seed %llu, %s: registers differ at %s: %02x where the first run had %02x\n",
                             static_cast<unsigned long long>(seed), what, FieldAt(i).c_str(), got.registers[i],
                             expected.registers[i]);
            }
            same = false;
        }
    }

    reported = 0;
    for (std::size_t i = 0; i < kMemoryBytes; i += 2)
    {
        if (got.memory[i] != expected.memory[i] || got.memory[i + 1] != expected.memory[i + 1])
        {
            if (reported++ < 8)
            {
                std::fprintf(stderr, "seed %llu, %s: memory word %05zx differs: %04x where the first run had %04x\n",
                             static_cast<unsigned long long>(seed), what, i / 2,
                             got.memory[i] | (got.memory[i + 1] << 8),
                             expected.memory[i] | (expected.memory[i + 1] << 8));
            }
            same = false;
        }
    }

    if (got.samples != expected.samples)
    {
        std::fprintf(stderr, "seed %llu, %s: %zu samples of audio where the first run had %zu, or different ones\n",
                     static_cast<unsigned long long>(seed), what, got.samples.size(), expected.samples.size());
        same = false;
    }

    return same;
}

// Compares the component registers of two DSPs, as teakra_jit_test does. Reading some of them changes them.
bool CompareRegisters(uint64_t seed, const char* what, Teakra::Teakra& expected, Teakra::Teakra& got)
{
    bool same = true;
    int reported = 0;
    for (uint16_t address = 0; address < 0x800; address += 2)
    {
        std::string x, y;
        try
        {
            x = std::to_string(expected.MMIORead(address));
        }
        catch (const std::exception& e)
        {
            x = e.what();
        }
        try
        {
            y = std::to_string(got.MMIORead(address));
        }
        catch (const std::exception& e)
        {
            y = e.what();
        }

        if (x != y)
        {
            if (reported++ < 8)
            {
                std::fprintf(stderr, "seed %llu, %s: MMIO register %03x reads \"%s\" where the first run had \"%s\"\n",
                             static_cast<unsigned long long>(seed), what, address, y.c_str(), x.c_str());
            }
            same = false;
        }
    }

    return same;
}

void Store(std::vector<uint8_t>& memory, uint32_t word, uint16_t value)
{
    memory[word * 2] = static_cast<uint8_t>(value);
    memory[word * 2 + 1] = static_cast<uint8_t>(value >> 8);
}

// A program that idles in a branch to itself, with a short handler at each interrupt vector.
std::vector<uint8_t> IdleMemory(std::mt19937_64& rng)
{
    std::vector<uint8_t> memory(kMemoryBytes);
    Store(memory, 0, kBranch);
    Store(memory, 1, 0);
    for (uint32_t vector : {0x0006u, 0x000Eu, 0x0016u, uint32_t{kVectorAddress}})
    {
        const uint32_t nops = static_cast<uint32_t>(rng() % 4);
        for (uint32_t i = 0; i < nops; ++i)
        {
            Store(memory, vector + i, kNop);
        }
        Store(memory, vector + nops, kReturnFromInterrupt);
    }

    // Random data, for DMA to move about.
    for (uint32_t word = kProgramWords; word < kMemoryBytes / 2; ++word)
    {
        Store(memory, word, static_cast<uint16_t>(rng()));
    }

    return memory;
}

// Sets up the components at random: interrupt routing and vectors, the timers, the audio ports' queues, the command
// registers and semaphore, and sometimes a DMA transfer within data memory.
void SetUpComponents(Teakra::Teakra& dsp, std::mt19937_64& rng)
{
    const auto chance = [&](unsigned percent)
    {
        return rng() % 100 < percent;
    };

    // The interrupt requests: 0xA and 0x9 the timers, 0xB the audio ports, 0xE the command registers and semaphore, 0xF
    // DMA.
    constexpr uint16_t kRequests = (1 << 0xA) | (1 << 0x9) | (1 << 0xB) | (1 << 0xE) | (1 << 0xF);
    for (uint16_t i = 0; i < 3; ++i)
    {
        dsp.MMIOWrite(static_cast<uint16_t>(0x206 + 2 * i), static_cast<uint16_t>(rng() & kRequests));
    }
    dsp.MMIOWrite(0x20C, static_cast<uint16_t>(rng() & kRequests));

    for (uint16_t irq = 0; irq < 16; ++irq)
    {
        dsp.MMIOWrite(static_cast<uint16_t>(0x212 + irq * 4),
                      static_cast<uint16_t>(chance(50) ? 0x8000 : 0)); // context switch, and the vector's high bits
        dsp.MMIOWrite(static_cast<uint16_t>(0x214 + irq * 4), kVectorAddress);
    }

    for (uint16_t t = 0; t < 2; ++t)
    {
        const uint32_t start = 50 + static_cast<uint32_t>(rng() % 20000);
        dsp.MMIOWrite(static_cast<uint16_t>(0x24 + t * 0x10), static_cast<uint16_t>(start));
        dsp.MMIOWrite(static_cast<uint16_t>(0x26 + t * 0x10), static_cast<uint16_t>(start >> 16));
        const uint16_t mode = static_cast<uint16_t>(rng() % 3); // single, auto restart or free running
        const uint16_t update = chance(50) ? 1 : 0;
        const uint16_t pause = chance(10) ? 1 : 0;
        // The scale (bits 0-1) stays 0, which Teakra requires; bit 10 loads the start value.
        dsp.MMIOWrite(static_cast<uint16_t>(0x20 + t * 0x10),
                      static_cast<uint16_t>((mode << 2) | (pause << 8) | (update << 9) | (1 << 10)));
    }

    for (uint16_t port = 0; port < 2; ++port)
    {
        if (chance(70))
        {
            const unsigned count = static_cast<unsigned>(rng() % 17);
            for (unsigned i = 0; i < count; ++i)
            {
                dsp.MMIOWrite(static_cast<uint16_t>(0x2C6 + port * 0x80), static_cast<uint16_t>(rng()));
            }
            dsp.MMIOWrite(static_cast<uint16_t>(0x2BE + port * 0x80), 1);
        }
    }

    for (uint8_t channel = 0; channel < 3; ++channel)
    {
        if (chance(30))
        {
            dsp.SendData(channel, static_cast<uint16_t>(rng()));
        }
    }
    dsp.MaskSemaphore(static_cast<uint16_t>(rng()));
    if (chance(50))
    {
        dsp.SetSemaphore(static_cast<uint16_t>(rng()));
    }

    if (chance(30))
    {
        // A transfer of a few hundred words from one part of data memory to another (space 0 is data memory).
        dsp.MMIOWrite(0x1BE, 0); // channel 0
        dsp.MMIOWrite(0x1C0, static_cast<uint16_t>(0x1000 + rng() % 0x4000));
        dsp.MMIOWrite(0x1C2, 0);
        dsp.MMIOWrite(0x1C4, static_cast<uint16_t>(0x6000 + rng() % 0x1000));
        dsp.MMIOWrite(0x1C6, 0);
        dsp.MMIOWrite(0x1C8, static_cast<uint16_t>(1 + rng() % 300));
        dsp.MMIOWrite(0x1CA, 1);
        dsp.MMIOWrite(0x1CC, 1);
        dsp.MMIOWrite(0x1CE, 1);
        dsp.MMIOWrite(0x1D0, 1);
        dsp.MMIOWrite(0x1DA, 0x0000); // source and destination in data memory
        dsp.MMIOWrite(0x184, 1);      // enable channel 0
        dsp.MMIOWrite(0x1DE, 0x40C0); // start
    }
}

int threw_before = 0; // trials whose run threw before the save, which test nothing
int threw_after = 0;  // trials whose run threw after it, which are compared up to the exception

// One trial. Returns false, after a report, if a loaded DSP ran differently.
bool Trial(uint64_t seed, bool idle)
{
    std::mt19937_64 rng(seed);
    const uint32_t cycles = idle ? 60000 : 3000;
    const std::vector<uint8_t> memory = idle ? IdleMemory(rng) : RandomMemory(rng);

    std::vector<Sample> samples;
    const auto collect = [&samples](Sample s)
    {
        samples.push_back(s);
    };

    std::unique_ptr<Teakra::Teakra> first = MakeDsp(true);
    first->SetAudioCallback(collect);
    std::memcpy(first->GetDspMemory(), memory.data(), kMemoryBytes);

    RegisterState& regs = first->GetRegisterState();
    if (idle)
    {
        regs.pc = 0;
        regs.sp = 0x4000;
        regs.ie = 1;
        for (auto& m : regs.im)
        {
            m = static_cast<uint16_t>(rng() & 1);
        }
        regs.imv = static_cast<uint16_t>(rng() & 1);
    }
    else
    {
        Randomize(regs, rng);
    }

    SetUpComponents(*first, rng);

    // An idle program and its handlers are valid code, so it mustn't throw. A random program may.
    const uint32_t before = 1 + static_cast<uint32_t>(rng() % (cycles - 1));
    const uint32_t after = cycles - before;
    if (const std::string threw = Run(*first, before); !threw.empty())
    {
        ++threw_before;
        if (idle)
        {
            std::fprintf(stderr, "seed %llu: the idle program %s\n", static_cast<unsigned long long>(seed),
                         threw.c_str());
        }

        return !idle;
    }

    const std::vector<uint8_t> state = first->SaveState();
    const std::vector<uint8_t> saved_memory(first->GetDspMemory(), first->GetDspMemory() + kMemoryBytes);
    const Outcome expected = Finish(*first, after, samples);
    bool same = true;
    if (!expected.threw.empty())
    {
        ++threw_after;
        if (idle)
        {
            std::fprintf(stderr, "seed %llu: the idle program %s after the save\n",
                         static_cast<unsigned long long>(seed), expected.threw.c_str());
            same = false;
        }
    }

    // A new DSP, without the JIT.
    std::unique_ptr<Teakra::Teakra> second = MakeDsp(false);
    second->SetAudioCallback(collect);
    second->LoadState(state);
    std::memcpy(second->GetDspMemory(), saved_memory.data(), kMemoryBytes);
    if (second->SaveState() != state)
    {
        std::fprintf(stderr, "seed %llu: a new DSP saves a different state from the one it loaded\n",
                     static_cast<unsigned long long>(seed));
        same = false;
    }
    same = Compare(seed, "a new DSP", expected, Finish(*second, after, samples)) && same;

    // Restore the original DSP.
    first->LoadState(state);
    std::memcpy(first->GetDspMemory(), saved_memory.data(), kMemoryBytes);
    same = Compare(seed, "the first DSP again", expected, Finish(*first, after, samples)) && same;

    same = CompareRegisters(seed, "a new DSP", *first, *second) && same;

    return same;
}

// LoadState refuses bytes that SaveState didn't make.
bool BadStates()
{
    std::unique_ptr<Teakra::Teakra> dsp = MakeDsp(false);
    const std::vector<uint8_t> state = dsp->SaveState();

    bool ok = true;
    const auto refused = [&](std::vector<uint8_t> bytes, const char* what)
    {
        try
        {
            dsp->LoadState(bytes);
        }
        catch (const std::invalid_argument&)
        {
            return;
        }

        std::fprintf(stderr, "LoadState took %s\n", what);
        ok = false;
    };

    std::vector<uint8_t> bad = state;
    bad[0] ^= 1;
    refused(bad, "a state with the wrong first word");
    refused(std::vector<uint8_t>(state.begin(), state.end() - 1), "a state cut short");
    bad = state;
    bad.push_back(0);
    refused(bad, "a state with a byte too many");
    refused({}, "no bytes at all");

    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    const int trials = argc > 1 ? std::atoi(argv[1]) : 600;
    const uint64_t first = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;

    threesf::SilenceStdout(); // Teakra reports MMIO it doesn't model there
    FindThrowing(*MakeDsp(false), Kinds());

    int failed = 0;
    for (int i = 0; i < trials; ++i)
    {
        // Every third program idles, with the components doing the work.
        if (!Trial(first + static_cast<uint64_t>(i), i % 3 == 2))
        {
            ++failed;
        }
    }

    const bool bad_states = BadStates();

    std::fprintf(stderr, "%d of %d programs ran differently after LoadState (%d threw before the save, %d after it)\n",
                 failed, trials, threw_before, threw_after);

    return failed == 0 && bad_states ? 0 : 1;
}
