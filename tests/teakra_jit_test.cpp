// SPDX-License-Identifier: MIT

// Checks that Teakra's JIT (externals/teakra/src/jit.cpp) runs DSP code as its interpreter does: random programs run
// from random states on two DSPs, one with the JIT (where the host has one) and one with TEAKRA_JIT=0, and their
// registers and memory must end up the same.
//
// usage: teakra_jit_test [trials [first seed [cycles]]]

#include <teakra/disassembler.h>
#include <teakra/impl/register.h>
#include <teakra/teakra.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "common/quiet_stdout.h"
#include "teakra_programs.h"

namespace
{

using Teakra::RegisterState;
using namespace threesf::teakra_test;

uint32_t cycles = 3000; // per program

// Runs a DSP, and returns what it threw, if anything.
std::string Run(Teakra::Teakra& dsp)
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

int threw = 0; // trials that ended in an exception, in both DSPs

// The instructions that translated code ran, on the DSPs with the JIT and without.
uint64_t translated = 0;
uint64_t translated_without = 0;

#if defined(__x86_64__) || defined(_M_X64)
constexpr bool kHostHasJit = true;
#else
constexpr bool kHostHasJit = false;
#endif

// One program from one state on both DSPs. Returns false, after a report, if they differ.
bool Trial(uint64_t seed, std::unique_ptr<Teakra::Teakra>* keep_jit = nullptr,
           std::unique_ptr<Teakra::Teakra>* keep_reference = nullptr)
{
    std::unique_ptr<Teakra::Teakra> jit_dsp = MakeDsp(true);
    std::unique_ptr<Teakra::Teakra> reference_dsp = MakeDsp(false);
    Teakra::Teakra& jit = *jit_dsp;
    Teakra::Teakra& reference = *reference_dsp;
    std::mt19937_64 rng(seed);

    const std::vector<uint8_t> memory = RandomMemory(rng);

    std::memcpy(jit.GetDspMemory(), memory.data(), kMemoryBytes);
    std::memcpy(reference.GetDspMemory(), memory.data(), kMemoryBytes);
    Randomize(jit.GetRegisterState(), rng);
    // Byte for byte, padding included, so that the states can be compared the same way.
    std::memcpy(static_cast<void*>(&reference.GetRegisterState()), &jit.GetRegisterState(), sizeof(RegisterState));

    const std::string jit_result = Run(jit);
    const std::string reference_result = Run(reference);
    translated += jit.JitInstructions();
    translated_without += reference.JitInstructions();
    if (std::getenv("JIT_TEST_VERBOSE"))
    {
        std::fprintf(stderr, "seed %llu: %s / %s; lp %u bcn %u / lp %u bcn %u\n", static_cast<unsigned long long>(seed),
                     jit_result.c_str(), reference_result.c_str(), jit.GetRegisterState().lp,
                     jit.GetRegisterState().bcn, reference.GetRegisterState().lp, reference.GetRegisterState().bcn);
    }

    bool same = true;
    if (jit_result != reference_result)
    {
        std::fprintf(stderr, "seed %llu: with the JIT \"%s\", without \"%s\"\n", static_cast<unsigned long long>(seed),
                     jit_result.c_str(), reference_result.c_str());
        same = false;
    }

    // After an exception the interpreter's pc is the one it had when RunOrdinary began, not the failing instruction's,
    // so it isn't compared then.
    if (!reference_result.empty())
    {
        ++threw;
        jit.GetRegisterState().pc = reference.GetRegisterState().pc;
    }

    const auto* a = reinterpret_cast<const uint8_t*>(&jit.GetRegisterState());
    const auto* b = reinterpret_cast<const uint8_t*>(&reference.GetRegisterState());
    int reported = 0;
    for (std::size_t i = 0; i < sizeof(RegisterState); ++i)
    {
        if (a[i] != b[i])
        {
            if (reported++ < 8)
            {
                std::fprintf(stderr, "seed %llu: registers differ at %s: %02x with the JIT, %02x without\n",
                             static_cast<unsigned long long>(seed), FieldAt(i).c_str(), a[i], b[i]);
            }
            same = false;
        }
    }

    const uint8_t* ma = jit.GetDspMemory();
    const uint8_t* mb = reference.GetDspMemory();
    reported = 0;
    for (std::size_t i = 0; i < kMemoryBytes; i += 2)
    {
        if (ma[i] != mb[i] || ma[i + 1] != mb[i + 1])
        {
            if (reported++ < 8)
            {
                std::fprintf(stderr, "seed %llu: memory word %05zx differs: %04x with the JIT, %04x without\n",
                             static_cast<unsigned long long>(seed), i / 2, ma[i] | (ma[i + 1] << 8),
                             mb[i] | (mb[i + 1] << 8));
            }
            same = false;
        }
    }

    // The components, through their registers (reading some changes them, which no longer matters).
    reported = 0;
    for (uint16_t address = 0; address < 0x800; address += 2)
    {
        const uint16_t x = jit.MMIORead(address);
        const uint16_t y = reference.MMIORead(address);
        if (x != y)
        {
            if (reported++ < 8)
            {
                std::fprintf(stderr, "seed %llu: MMIO register %03x differs: %04x with the JIT, %04x without\n",
                             static_cast<unsigned long long>(seed), address, x, y);
            }
            same = false;
        }
    }

    if (keep_jit)
    {
        *keep_jit = std::move(jit_dsp);
        *keep_reference = std::move(reference_dsp);
    }

    return same;
}

// For a program that runs differently: the fewest cycles after which it does, and the instructions around the
// reference's program counter before them.
void Bisect(uint64_t seed)
{
    const uint32_t all = cycles;
    uint32_t low = 0, high = all; // same after low cycles, different after high
    while (high - low > 1)
    {
        cycles = (low + high) / 2;
        if (Trial(seed))
        {
            low = cycles;
        }
        else
        {
            high = cycles;
        }
    }

    cycles = low;
    std::unique_ptr<Teakra::Teakra> jit_dsp, reference_dsp;
    Trial(seed, &jit_dsp, &reference_dsp);

    const Teakra::Teakra& reference = *reference_dsp;
    const uint32_t pc = reference.GetRegisterState().pc;
    const RegisterState& r = reference.GetRegisterState();
    std::fprintf(stderr, "seed %llu: the same after %u cycles, at pc %05x (page %02x, lp %u, bcn %u),\n",
                 static_cast<unsigned long long>(seed), low, pc, r.page, r.lp, r.bcn);
    std::fprintf(stderr, "different after %u:\n", high);
    for (uint32_t address = pc >= 8 ? pc - 8 : 0; address < pc + 8; ++address)
    {
        const uint8_t* memory = reference.GetDspMemory();
        const auto word = [&](uint32_t a)
        {
            return static_cast<uint16_t>(memory[a * 2] | (memory[a * 2 + 1] << 8));
        };
        std::fprintf(stderr, "  %c %05x  %04x  %s\n", address == pc ? '>' : ' ', address, word(address),
                     Teakra::Disassembler::Do(word(address), word(address + 1)).c_str());
    }

    cycles = high;
    Trial(seed);
    cycles = all;
}

} // namespace

int main(int argc, char** argv)
{
    const int trials = argc > 1 ? std::atoi(argv[1]) : 1000;
    const uint64_t first = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    if (argc > 3)
    {
        cycles = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
    }

    threesf::SilenceStdout(); // Teakra reports MMIO it doesn't model there
    FindThrowing(*MakeDsp(false), Kinds());

    int failed = 0;
    for (int i = 0; i < trials; ++i)
    {
        if (!Trial(first + static_cast<uint64_t>(i)))
        {
            ++failed;
            if (std::getenv("JIT_TEST_BISECT"))
            {
                Bisect(first + static_cast<uint64_t>(i));
            }
        }
    }

    std::fprintf(stderr, "%d of %d random programs ran differently with the JIT (%d ended in an exception)\n", failed,
                 trials, threw);

    // Without translated code on the first DSP, or with some on the second, the test compared a DSP with itself. A
    // program can throw before a block runs, so only a run of 100 programs or more must have run translated code.
    if ((kHostHasJit && trials >= 100 && translated == 0) || translated_without != 0)
    {
        std::fprintf(stderr, "translated code ran %llu instructions with the JIT and %llu without\n",
                     static_cast<unsigned long long>(translated), static_cast<unsigned long long>(translated_without));
        failed++;
    }

    return failed == 0 ? 0 : 1;
}
