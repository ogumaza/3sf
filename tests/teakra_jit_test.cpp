// SPDX-License-Identifier: MIT

// Checks that Teakra's JIT (externals/teakra/src/jit.cpp) runs DSP code as its interpreter does: random programs run
// from random states on two DSPs, one with the JIT (where the host has one) and one with TEAKRA_JIT=0, and their
// registers and memory must end up the same.
//
// usage: threesf_teakra_jit_test [trials [first seed [cycles]]]

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

#include <teakra/disassembler.h>
#include <teakra/impl/register.h>
#include <teakra/teakra.h>

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

// Runs two DSPs from the same state, one with the JIT and one without, and compares what they end up with. Returns
// false, after a report under `label`, if they differ.
bool RunBoth(const std::string& label, Teakra::Teakra& jit, Teakra::Teakra& reference)
{
    const std::string jit_result = Run(jit);
    const std::string reference_result = Run(reference);
    translated += jit.JitInstructions();
    translated_without += reference.JitInstructions();
    if (std::getenv("JIT_TEST_VERBOSE"))
    {
        std::fprintf(stderr, "%s: %s / %s; lp %u bcn %u / lp %u bcn %u\n", label.c_str(), jit_result.c_str(),
                     reference_result.c_str(), jit.GetRegisterState().lp, jit.GetRegisterState().bcn,
                     reference.GetRegisterState().lp, reference.GetRegisterState().bcn);
    }

    bool same = true;
    if (jit_result != reference_result)
    {
        std::fprintf(stderr, "%s: with the JIT \"%s\", without \"%s\"\n", label.c_str(), jit_result.c_str(),
                     reference_result.c_str());
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
                std::fprintf(stderr, "%s: registers differ at %s: %02x with the JIT, %02x without\n", label.c_str(),
                             FieldAt(i).c_str(), a[i], b[i]);
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
                std::fprintf(stderr, "%s: memory word %05zx differs: %04x with the JIT, %04x without\n", label.c_str(),
                             i / 2, ma[i] | (ma[i + 1] << 8), mb[i] | (mb[i + 1] << 8));
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
                std::fprintf(stderr, "%s: MMIO register %03x differs: %04x with the JIT, %04x without\n", label.c_str(),
                             address, x, y);
            }
            same = false;
        }
    }

    return same;
}

// Copies the memory and registers of `from` to `to`, byte for byte, padding included. RunBoth compares the registers
// byte for byte as well.
void CopyState(const Teakra::Teakra& from, Teakra::Teakra& to)
{
    std::memcpy(to.GetDspMemory(), from.GetDspMemory(), kMemoryBytes);
    std::memcpy(static_cast<void*>(&to.GetRegisterState()), &from.GetRegisterState(), sizeof(RegisterState));
}

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
    Randomize(jit.GetRegisterState(), rng);
    CopyState(jit, reference);

    const bool same = RunBoth("seed " + std::to_string(seed), jit, reference);

    if (keep_jit)
    {
        *keep_jit = std::move(jit_dsp);
        *keep_reference = std::move(reference_dsp);
    }

    return same;
}

// A block repeat whose body ends in push lc. The interpreter steps the loop before that instruction runs. The push then
// stores the next pass's count, or, on the inner loop's last pass, the outer loop's count. An inner loop of three
// passes ends at the push. The inner loop runs inside an outer loop of two passes. Returns false, after a report, if
// the DSPs differ.
bool LoopEndTrial()
{
    constexpr uint16_t kPushLc = 0x5E5E;
    constexpr uint32_t kStart = 0x100;
    constexpr uint32_t kInnerEnd = 0x103;
    constexpr uint32_t kOuterEnd = 0x110;
    std::unique_ptr<Teakra::Teakra> jit = MakeDsp(true);
    std::unique_ptr<Teakra::Teakra> reference = MakeDsp(false);

    uint8_t* memory = jit->GetDspMemory();
    std::memset(memory, 0, kMemoryBytes); // nops
    memory[kInnerEnd * 2] = static_cast<uint8_t>(kPushLc);
    memory[kInnerEnd * 2 + 1] = static_cast<uint8_t>(kPushLc >> 8);

    RegisterState& r = jit->GetRegisterState();
    r.pc = kStart;
    r.sp = 0x1000;
    r.bkrep_stack[0] = {kStart, kOuterEnd, 1};
    r.bkrep_stack[1] = {kStart, kInnerEnd, 2};
    r.bcn = 2;
    r.lp = 1;
    CopyState(*jit, *reference);

    return RunBoth("push lc at a loop end", *jit, *reference);
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

    if (!LoopEndTrial())
    {
        failed++;
    }

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
