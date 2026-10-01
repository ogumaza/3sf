// SPDX-License-Identifier: MIT

// Checks that Teakra's JIT (externals/teakra/src/jit.cpp) runs DSP code as its interpreter does: random programs run
// from random states on two DSPs, one with the JIT (where the host has one) and one with TEAKRA_JIT=0, and their
// registers and memory must end up the same.
//
// usage: teakra_jit_test [trials [first seed [cycles]]]

#include <teakra/disassembler.h>
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
#include <string>
#include <vector>

#include "common/quiet_stdout.h"

namespace
{

using Teakra::RegisterState;

constexpr std::size_t kMemoryBytes = 0x80000;
constexpr uint32_t kProgramWords = 0x20000;
uint32_t cycles = 3000; // per program

// Jumps, calls, returns, repeats and the like, by the disassembler's name for them: programs keep few of them (and more
// of the block repeats, whose loops the JIT runs itself), so that the runs between them, which the JIT translates, are
// long.
enum class Kind : uint8_t
{
    kOrdinary,
    kFlow,
    kBlockRepeat,
    kThrows, // undefined, or the interpreter throws for it whatever the state (not something to test)
};

Kind KindOf(uint16_t opcode);

// Marks the opcodes the interpreter throws for from a reset state, which programs leave out.
void FindThrowing(Teakra::Teakra& dsp, std::vector<Kind>& kinds)
{
    for (uint32_t op = 0; op < 0x10000; ++op)
    {
        if (kinds[op] != Kind::kOrdinary)
        {
            continue;
        }

        dsp.Reset();
        uint8_t* memory = dsp.GetDspMemory();
        memory[0] = static_cast<uint8_t>(op);
        memory[1] = static_cast<uint8_t>(op >> 8);
        memory[2] = memory[3] = 0;

        try
        {
            dsp.Run(1);
        }
        catch (const std::exception&)
        {
            kinds[op] = Kind::kThrows;
        }
    }
}

std::vector<Kind>& Kinds()
{
    static std::vector<Kind> kinds = []
    {
        static const char* const kFlow[] = {"br",   "brr",  "call",  "calla", "callr", "ret",
                                            "rets", "reti", "retd",  "retic", "retid", "retidc",
                                            "rep",  "trap", "break", "cntx",  "eint",  "dint"};

        std::vector<Kind> table(0x10000, Kind::kOrdinary);
        for (uint32_t op = 0; op < 0x10000; ++op)
        {
            const std::vector<std::string> tokens = Teakra::Disassembler::GetTokenList(static_cast<uint16_t>(op));
            if (tokens.empty() || tokens[0] == "[ERROR]")
            {
                table[op] = Kind::kThrows;
                continue;
            }

            if (tokens[0] == "bkrep")
            {
                table[op] = Kind::kBlockRepeat;
            }
            for (const char* name : kFlow)
            {
                if (tokens[0] == name)
                {
                    table[op] = Kind::kFlow;
                }
            }
        }
        return table;
    }();
    return kinds;
}

Kind KindOf(uint16_t opcode)
{
    return Kinds()[opcode];
}

// Sets TEAKRA_JIT while a DSP is made, which is when its JIT reads it.
class JitSetting
{
public:
    explicit JitSetting(const char* value)
    {
        if (const char* old = std::getenv("TEAKRA_JIT"))
        {
            old_ = old;
            had_ = true;
        }

        Set(value);
    }

    ~JitSetting()
    {
        Set(had_ ? old_.c_str() : nullptr);
    }

private:
    static void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s("TEAKRA_JIT", value ? value : "");
#else
        if (value)
        {
            setenv("TEAKRA_JIT", value, 1);
        }
        else
        {
            unsetenv("TEAKRA_JIT");
        }
#endif
    }

    std::string old_;
    bool had_ = false;
};

// A random state: data, pointers, flags and the modes that decide how the JIT translates.
void Randomize(RegisterState& r, std::mt19937_64& rng)
{
    const auto bits = [&](int n)
    {
        return static_cast<uint16_t>(rng() & ((1u << n) - 1));
    };
    const auto acc = [&]
    {
        const uint64_t v = rng() & 0xFF'FFFF'FFFFull;
        return (v & (1ull << 39)) ? v | 0xFFFF'FF00'0000'0000ull : v;
    };

    for (auto& v : r.a)
    {
        v = acc();
    }
    for (auto& v : r.b)
    {
        v = acc();
    }
    r.a1s = acc();
    r.b1s = acc();

    for (auto& v : r.x)
    {
        v = bits(16);
    }
    for (auto& v : r.y)
    {
        v = bits(16);
    }
    for (auto& v : r.p)
    {
        v = static_cast<uint32_t>(rng());
    }
    for (auto& v : r.pe)
    {
        v = bits(1);
    }
    for (auto& v : r.ps)
    {
        v = bits(2);
    }

    for (auto& v : r.r)
    {
        v = bits(16);
    }
    r.sp = bits(16);
    r.page = bits(8);
    r.mixp = bits(16);
    r.sv = (rng() & 1) ? bits(16) : static_cast<uint16_t>(static_cast<int>(rng() % 81) - 40);

    for (uint16_t* flag : {&r.fz, &r.fm, &r.fn, &r.fv, &r.fe, &r.fc0, &r.fc1, &r.flm, &r.fvl, &r.fr})
    {
        *flag = bits(1);
    }
    r.sat = bits(1);
    r.sata = bits(1);
    r.s = bits(1);
    r.hwm = bits(2);

    r.stepi = bits(7);
    r.stepj = bits(7);
    r.modi = bits(9);
    r.modj = bits(9);
    r.stepi0 = bits(16);
    r.stepj0 = bits(16);
    for (auto& v : r.m)
    {
        v = (rng() % 4) == 0;
    }
    for (auto& v : r.br)
    {
        v = (rng() % 8) == 0;
    }

    r.stp16 = bits(1);
    r.cmd = bits(1);
    r.epi = bits(1);
    r.epj = bits(1);
    for (auto* array : {&r.arstep, &r.arpstepi, &r.arpstepj})
    {
        for (auto& v : *array)
        {
            v = bits(3);
        }
    }
    for (auto* array : {&r.aroffset, &r.arpoffseti, &r.arpoffsetj})
    {
        for (auto& v : *array)
        {
            v = bits(2);
        }
    }
    for (auto& v : r.arrn)
    {
        v = bits(3);
    }

    // Two bits each: arprnj picks r4 to r7 with 0 to 3.
    for (auto* array : {&r.arprni, &r.arprnj})
    {
        for (auto& v : *array)
        {
            v = bits(2);
        }
    }

    r.cpc = bits(1);
    r.repc = bits(16);
}

// Names the RegisterState field at byte `offset`, for a report.
std::string FieldAt(std::size_t offset)
{
    RegisterState r;
    const auto* base = reinterpret_cast<const uint8_t*>(&r);

    struct Field
    {
        const char* name;
        const void* at;
        std::size_t size;
    };
#define THREESF_FIELD(f) (Field{#f, &r.f, sizeof(r.f)})
    const Field fields[] = {
        THREESF_FIELD(pc),         THREESF_FIELD(prpage),     THREESF_FIELD(cpc),      THREESF_FIELD(repc),
        THREESF_FIELD(rep),        THREESF_FIELD(bcn),        THREESF_FIELD(lp),       THREESF_FIELD(bkrep_stack),
        THREESF_FIELD(a),          THREESF_FIELD(b),          THREESF_FIELD(a1s),      THREESF_FIELD(b1s),
        THREESF_FIELD(sat),        THREESF_FIELD(sata),       THREESF_FIELD(s),        THREESF_FIELD(sv),
        THREESF_FIELD(fz),         THREESF_FIELD(fm),         THREESF_FIELD(fn),       THREESF_FIELD(fv),
        THREESF_FIELD(fe),         THREESF_FIELD(fc0),        THREESF_FIELD(fc1),      THREESF_FIELD(flm),
        THREESF_FIELD(fvl),        THREESF_FIELD(fr),         THREESF_FIELD(x),        THREESF_FIELD(y),
        THREESF_FIELD(hwm),        THREESF_FIELD(p),          THREESF_FIELD(pe),       THREESF_FIELD(ps),
        THREESF_FIELD(r),          THREESF_FIELD(mixp),       THREESF_FIELD(sp),       THREESF_FIELD(page),
        THREESF_FIELD(stepi),      THREESF_FIELD(stepj),      THREESF_FIELD(modi),     THREESF_FIELD(modj),
        THREESF_FIELD(stepi0),     THREESF_FIELD(stepj0),     THREESF_FIELD(m),        THREESF_FIELD(br),
        THREESF_FIELD(arstep),     THREESF_FIELD(arpstepi),   THREESF_FIELD(arpstepj), THREESF_FIELD(aroffset),
        THREESF_FIELD(arpoffseti), THREESF_FIELD(arpoffsetj), THREESF_FIELD(arrn),     THREESF_FIELD(arprni),
        THREESF_FIELD(arprnj),     THREESF_FIELD(ie),
    };
#undef THREESF_FIELD

    for (const Field& f : fields)
    {
        const std::size_t start = static_cast<const uint8_t*>(f.at) - base;
        if (offset >= start && offset < start + f.size)
        {
            return std::string(f.name) + "+" + std::to_string(offset - start);
        }
    }

    return "offset " + std::to_string(offset);
}

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

// A DSP with the JIT or without. Each program gets new ones: Reset leaves some MMIO registers as they were.
std::unique_ptr<Teakra::Teakra> MakeDsp(bool with_jit)
{
    JitSetting setting(with_jit ? nullptr : "0");
    return std::make_unique<Teakra::Teakra>(Teakra::UserConfig{});
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

    // A program with few jumps (and some block repeats), and random data.
    std::vector<uint8_t> memory(kMemoryBytes);
    for (uint32_t word = 0; word < kMemoryBytes / 2; ++word)
    {
        uint16_t value = static_cast<uint16_t>(rng());
        while (word < kProgramWords)
        {
            const Kind kind = KindOf(value);
            if (kind == Kind::kOrdinary ||
                (kind != Kind::kThrows && (rng() % 100) < (kind == Kind::kBlockRepeat ? 30u : 2u)))
            {
                break;
            }
            value = static_cast<uint16_t>(rng());
        }
        memory[word * 2] = static_cast<uint8_t>(value);
        memory[word * 2 + 1] = static_cast<uint8_t>(value >> 8);
    }

    std::memcpy(jit.GetDspMemory(), memory.data(), kMemoryBytes);
    std::memcpy(reference.GetDspMemory(), memory.data(), kMemoryBytes);
    Randomize(jit.GetRegisterState(), rng);
    // Byte for byte, padding included, so that the states can be compared the same way.
    std::memcpy(static_cast<void*>(&reference.GetRegisterState()), &jit.GetRegisterState(), sizeof(RegisterState));

    const std::string jit_result = Run(jit);
    const std::string reference_result = Run(reference);
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

    return failed == 0 ? 0 : 1;
}
