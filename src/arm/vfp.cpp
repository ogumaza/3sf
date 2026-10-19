// SPDX-License-Identifier: MIT

// ARM11 interpreter: VFPv2 (the ARM11 MPCore's VFP11 coprocessor), see cpu.h.
//
// Arithmetic is done with host IEEE single/double operations, which are correctly rounded, so results match the VFP bit
// for bit once the host rounding mode follows FPSCR.RMode. The multiply-accumulate forms aren't fused on VFPv2 and are
// computed as two rounded steps. Flush-to-zero (FPSCR.FZ) and default NaN (FPSCR.DN) are applied in software. NaN
// results while FPSCR.DN is clear aren't modelled. The host computes them, and an x86 host computes some differently
// from the VFP. Floating-point exceptions aren't modelled, and neither are short vectors: while FPSCR.LEN or STRIDE
// isn't 0, every VFP data-processing instruction is undefined, even one that's always scalar.

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <type_traits>

#include "arm/cpu.h"

namespace threesf::arm
{
namespace
{

constexpr uint32_t kFpscrDn = 1u << 25;
constexpr uint32_t kFpscrFz = 1u << 24;
constexpr uint32_t kFpscrVector = 0x00370000; // LEN (18:16) and STRIDE (21:20)
constexpr uint32_t kFpsid = 0x410120b4;       // VFP11 (VFPv2)
constexpr uint32_t kFpexcEn = 0x40000000;

// NZCV, DN, FZ, RMode, STRIDE, LEN, the trap enables and the cumulative flags.
constexpr uint32_t kFpscrWritable = 0xf3f79f9f;

// The register fields of an instruction. A single-precision register's number is Vx:X, and a double-precision one's
// is X:Vx. VFPv2 has D0 to D15, so a double-precision number of 16 or more is undefined.
constexpr uint32_t Sd(uint32_t op)
{
    return ((op >> 11) & 0x1e) | ((op >> 22) & 1);
}

constexpr uint32_t Sn(uint32_t op)
{
    return ((op >> 15) & 0x1e) | ((op >> 7) & 1);
}

constexpr uint32_t Sm(uint32_t op)
{
    return ((op << 1) & 0x1e) | ((op >> 5) & 1);
}

constexpr uint32_t Dd(uint32_t op)
{
    return ((op >> 18) & 0x10) | ((op >> 12) & 0xf);
}

constexpr uint32_t Dn(uint32_t op)
{
    return ((op >> 3) & 0x10) | ((op >> 16) & 0xf);
}

constexpr uint32_t Dm(uint32_t op)
{
    return ((op >> 1) & 0x10) | (op & 0xf);
}

uint32_t ToInt(double x, bool is_signed, bool round_to_zero)
{
    if (std::isnan(x))
    {
        return 0;
    }

    const double r = round_to_zero ? std::trunc(x) : std::nearbyint(x);
    if (is_signed)
    {
        if (r >= 2147483648.0)
        {
            return 0x7fffffff;
        }
        if (r < -2147483648.0)
        {
            return 0x80000000;
        }

        return static_cast<uint32_t>(static_cast<int32_t>(r));
    }

    if (r >= 4294967296.0)
    {
        return 0xffffffff;
    }
    if (r < 0.0)
    {
        return 0;
    }

    return static_cast<uint32_t>(r);
}

} // namespace

HostFpuMode::HostFpuMode()
{
    std::fegetenv(&saved_);
}

HostFpuMode::~HostFpuMode()
{
    std::fesetenv(&saved_);
}

void HostFpuMode::Apply(uint32_t fpscr)
{
    static constexpr int kModes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    std::fesetround(kModes[(fpscr >> 22) & 3]);
}

void Cpu::UpdateHostFpu()
{
    const uint32_t rmode = state_.fpscr & 0x00c00000;
    if (rmode != host_rmode_)
    {
        HostFpuMode::Apply(state_.fpscr);
        host_rmode_ = rmode;
    }
}

float Cpu::FlushIn(float f) const
{
    if ((state_.fpscr & kFpscrFz) && (std::bit_cast<uint32_t>(f) & 0x7f800000) == 0)
    {
        return std::bit_cast<float>(std::bit_cast<uint32_t>(f) & 0x80000000);
    }

    return f;
}

double Cpu::FlushIn(double d) const
{
    if ((state_.fpscr & kFpscrFz) && (std::bit_cast<uint64_t>(d) & 0x7ff0000000000000ull) == 0)
    {
        return std::bit_cast<double>(std::bit_cast<uint64_t>(d) & 0x8000000000000000ull);
    }

    return d;
}

void Cpu::SetS(uint32_t i, float f)
{
    uint32_t bits = std::bit_cast<uint32_t>(f);
    if ((bits & 0x7f800000) == 0x7f800000 && (bits & 0x007fffff) != 0)
    {
        if (state_.fpscr & kFpscrDn)
        {
            bits = 0x7fc00000;
        }
    }
    else if ((state_.fpscr & kFpscrFz) && (bits & 0x7f800000) == 0)
    {
        bits &= 0x80000000;
    }

    state_.vfp[i] = bits;
}

void Cpu::SetD(uint32_t i, double d)
{
    uint64_t bits = std::bit_cast<uint64_t>(d);
    if ((bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (bits & 0x000fffffffffffffull) != 0)
    {
        if (state_.fpscr & kFpscrDn)
        {
            bits = 0x7ff8000000000000ull;
        }
    }
    else if ((state_.fpscr & kFpscrFz) && (bits & 0x7ff0000000000000ull) == 0)
    {
        bits &= 0x8000000000000000ull;
    }

    SetDBits(i, bits);
}

template <>
float Cpu::Fp<float>(uint32_t i) const
{
    return S(i);
}

template <>
double Cpu::Fp<double>(uint32_t i) const
{
    return D(i);
}

template <>
void Cpu::SetFp<float>(uint32_t i, float value)
{
    SetS(i, value);
}

template <>
void Cpu::SetFp<double>(uint32_t i, double value)
{
    SetD(i, value);
}

void Cpu::FpCompare(double a, double b)
{
    uint32_t nzcv;
    if (std::isnan(a) || std::isnan(b))
    {
        nzcv = 0x3;
    }
    else if (a == b)
    {
        nzcv = 0x6;
    }
    else if (a < b)
    {
        nzcv = 0x8;
    }
    else
    {
        nzcv = 0x2;
    }

    state_.fpscr = (state_.fpscr & 0x0fffffff) | (nzcv << 28);
}

// opc is bits 23, 21, 20 and 6 of the instruction.
template <typename T>
bool Cpu::VfpArithmetic(uint32_t op, uint32_t opc, uint32_t d, uint32_t n, uint32_t m)
{
    const T a = FlushIn(Fp<T>(n));
    const T b = FlushIn(Fp<T>(m));
    T product;
    switch (opc)
    {
    case 0x0: // FMAC
        product = a * b;
        SetFp(d, FlushIn(Fp<T>(d)) + FlushIn(product));
        break;

    case 0x1: // FNMAC
        product = a * b;
        SetFp(d, FlushIn(Fp<T>(d)) - FlushIn(product));
        break;

    case 0x2: // FMSC
        product = a * b;
        SetFp(d, -FlushIn(Fp<T>(d)) + FlushIn(product));
        break;

    case 0x3: // FNMSC
        product = a * b;
        SetFp(d, -FlushIn(Fp<T>(d)) - FlushIn(product));
        break;

    case 0x4: // FMUL
        SetFp(d, a * b);
        break;

    case 0x5: // FNMUL: the negation follows default-NaN substitution
        SetFp(d, a * b);
        if constexpr (std::is_same_v<T, float>)
        {
            state_.vfp[d] ^= 0x80000000;
        }
        else
        {
            SetDBits(d, DBits(d) ^ 0x8000000000000000ull);
        }
        break;

    case 0x6: // FADD
        SetFp(d, a + b);
        break;

    case 0x7: // FSUB
        SetFp(d, a - b);
        break;

    case 0x8: // FDIV
        SetFp(d, a / b);
        break;

    default:
        return Undefined(op);
    }

    return true;
}

bool Cpu::Vfp(uint32_t op)
{
    if (state_.fpscr & kFpscrVector)
    {
        return Undefined(op); // short vectors aren't supported
    }

    const bool dp = (op >> 8) & 1;
    const uint32_t opc = ((op >> 20) & 8) | ((op >> 19) & 4) | ((op >> 19) & 2) | ((op >> 6) & 1);

    const uint32_t sd = Sd(op);
    const uint32_t sn = Sn(op);
    const uint32_t sm = Sm(op);
    const uint32_t dd = Dd(op);
    const uint32_t dn = Dn(op);
    const uint32_t dm = Dm(op);
    if (opc != 15)
    {
        if (!dp)
        {
            return VfpArithmetic<float>(op, opc, sd, sn, sm);
        }

        if (dd >= 16 || dn >= 16 || dm >= 16)
        {
            return Undefined(op);
        }

        return VfpArithmetic<double>(op, opc, dd, dn, dm);
    }

    // Extension instructions, selected by Vn:N.
    switch (sn)
    {
    case 0x00: // FCPY
        if (dp)
        {
            if (dd >= 16 || dm >= 16)
            {
                return Undefined(op);
            }

            SetDBits(dd, DBits(dm));
        }
        else
        {
            state_.vfp[sd] = state_.vfp[sm];
        }

        return true;

    case 0x01: // FABS
        if (dp)
        {
            if (dd >= 16 || dm >= 16)
            {
                return Undefined(op);
            }

            SetDBits(dd, DBits(dm) & ~0x8000000000000000ull);
        }
        else
        {
            state_.vfp[sd] = state_.vfp[sm] & 0x7fffffff;
        }

        return true;

    case 0x02: // FNEG
        if (dp)
        {
            if (dd >= 16 || dm >= 16)
            {
                return Undefined(op);
            }

            SetDBits(dd, DBits(dm) ^ 0x8000000000000000ull);
        }
        else
        {
            state_.vfp[sd] = state_.vfp[sm] ^ 0x80000000;
        }

        return true;

    case 0x03: // FSQRT
        if (dp)
        {
            if (dd >= 16 || dm >= 16)
            {
                return Undefined(op);
            }

            SetD(dd, std::sqrt(FlushIn(D(dm))));
        }
        else
        {
            SetS(sd, std::sqrt(FlushIn(S(sm))));
        }

        return true;

    case 0x08: // FCMP
    case 0x09: // FCMPE
        if (dp)
        {
            if (dd >= 16 || dm >= 16)
            {
                return Undefined(op);
            }

            FpCompare(FlushIn(D(dd)), FlushIn(D(dm)));
        }
        else
        {
            FpCompare(FlushIn(S(sd)), FlushIn(S(sm)));
        }

        return true;

    case 0x0a: // FCMPZ
    case 0x0b: // FCMPEZ
        if (dp)
        {
            if (dd >= 16)
            {
                return Undefined(op);
            }

            FpCompare(FlushIn(D(dd)), 0.0);
        }
        else
        {
            FpCompare(FlushIn(S(sd)), 0.0);
        }

        return true;

    case 0x0f:
        if (dp) // FCVTSD: single <- double
        {
            if (dm >= 16)
            {
                return Undefined(op);
            }

            SetS(sd, static_cast<float>(FlushIn(D(dm))));
        }
        else // FCVTDS: double <- single
        {
            if (dd >= 16)
            {
                return Undefined(op);
            }

            SetD(dd, static_cast<double>(FlushIn(S(sm))));
        }

        return true;

    case 0x10: // FUITO
    case 0x11: // FSITO
        {
            const uint32_t value = state_.vfp[sm];
            if (dp)
            {
                if (dd >= 16)
                {
                    return Undefined(op);
                }

                SetD(dd, (sn & 1) ? static_cast<double>(static_cast<int32_t>(value)) : static_cast<double>(value));
            }
            else
            {
                SetS(sd, (sn & 1) ? static_cast<float>(static_cast<int32_t>(value)) : static_cast<float>(value));
            }

            return true;
        }

    case 0x18: // FTOUI
    case 0x19: // FTOUIZ
    case 0x1a: // FTOSI
    case 0x1b: // FTOSIZ
        {
            double x;
            if (dp)
            {
                if (dm >= 16)
                {
                    return Undefined(op);
                }

                x = FlushIn(D(dm));
            }
            else
            {
                x = FlushIn(S(sm));
            }

            state_.vfp[sd] = ToInt(x, (sn & 2) != 0, (sn & 1) != 0);
            return true;
        }

    default:
        return Undefined(op);
    }
}

bool Cpu::VfpLoadStore(uint32_t op)
{
    const bool p = (op >> 24) & 1;
    const bool u = (op >> 23) & 1;
    const bool w = (op >> 21) & 1;
    const bool l = (op >> 20) & 1;
    const bool dp = (op >> 8) & 1;
    const uint32_t rn = (op >> 16) & 0xf;
    const uint32_t imm8 = op & 0xff;
    const uint32_t d = dp ? Dd(op) : Sd(op);
    if (p && !w) // FLDS/FLDD/FSTS/FSTD
    {
        const uint32_t base = rn == 15 ? (Reg(15) & ~3u) : state_.r[rn];
        const uint32_t addr = u ? base + imm8 * 4 : base - imm8 * 4;
        if (dp)
        {
            if (d >= 16)
            {
                return Undefined(op);
            }

            if (l)
            {
                const uint32_t lo = mem_.Read32(addr);
                const uint32_t hi = mem_.Read32(addr + 4);
                state_.vfp[2 * d] = lo;
                state_.vfp[2 * d + 1] = hi;
            }
            else
            {
                mem_.Write32(addr, state_.vfp[2 * d]);
                mem_.Write32(addr + 4, state_.vfp[2 * d + 1]);
            }
        }
        else
        {
            if (l)
            {
                state_.vfp[d] = mem_.Read32(addr);
            }
            else
            {
                mem_.Write32(addr, state_.vfp[d]);
            }
        }

        return true;
    }

    if (p == u)
    {
        return Undefined(op);
    }

    // FLDM/FSTM, increment after or decrement before (FLDMX/FSTMX when the count is odd).
    const uint32_t base = Reg(rn);
    const uint32_t words = imm8;
    uint32_t addr = u ? base : base - words * 4;
    const uint32_t first = dp ? 2 * d : d;
    const uint32_t count = dp ? (words & ~1u) : words;
    if (count == 0 || first + count > 32)
    {
        return Undefined(op);
    }

    if (w)
    {
        state_.r[rn] = u ? base + words * 4 : base - words * 4;
    }

    for (uint32_t i = 0; i < count; i++, addr += 4)
    {
        if (l)
        {
            state_.vfp[first + i] = mem_.Read32(addr);
        }
        else
        {
            mem_.Write32(addr, state_.vfp[first + i]);
        }
    }

    return true;
}

bool Cpu::VfpTransfer(uint32_t op)
{
    const bool l = (op >> 20) & 1;
    const uint32_t rt = (op >> 12) & 0xf;
    const uint32_t cp = (op >> 8) & 0xf;
    const uint32_t opc1 = (op >> 21) & 7;

    if ((op & 0x7f) != 0x10) // bits 6:5 zero, bit 4 set, CRm zero
    {
        return Undefined(op);
    }

    if (cp == 10 && opc1 == 0) // FMSR / FMRS
    {
        const uint32_t n = Sn(op);
        if (l)
        {
            state_.r[rt] = state_.vfp[n];
        }
        else
        {
            state_.vfp[n] = state_.r[rt];
        }

        return true;
    }

    if (cp == 10 && opc1 == 7) // FMXR / FMRX / FMSTAT
    {
        const uint32_t reg = (op >> 16) & 0xf;
        if (l)
        {
            uint32_t value;
            switch (reg)
            {
            case 0:
                value = kFpsid;
                break;
            case 1:
                value = state_.fpscr;
                break;
            case 8:
                value = kFpexcEn;
                break;
            default:
                return Undefined(op);
            }

            if (rt == 15)
            {
                if (reg != 1)
                {
                    return Undefined(op);
                }

                state_.n = (value >> 31) & 1;
                state_.z = (value >> 30) & 1;
                state_.c = (value >> 29) & 1;
                state_.v = (value >> 28) & 1;
            }
            else
            {
                state_.r[rt] = value;
            }

            return true;
        }

        if (reg == 1)
        {
            state_.fpscr = state_.r[rt] & kFpscrWritable;
            UpdateHostFpu();
            return true;
        }

        if (reg == 8)
        {
            return true; // FPEXC: the VFP stays enabled
        }

        return Undefined(op);
    }

    if (cp == 11 && (opc1 & 6) == 0) // FMDLR / FMDHR / FMRDL / FMRDH
    {
        const uint32_t n = Dn(op);
        if (n >= 16)
        {
            return Undefined(op);
        }

        const uint32_t index = 2 * n + (opc1 & 1);
        if (l)
        {
            state_.r[rt] = state_.vfp[index];
        }
        else
        {
            state_.vfp[index] = state_.r[rt];
        }

        return true;
    }

    return Undefined(op);
}

bool Cpu::VfpTransfer64(uint32_t op)
{
    const bool l = (op >> 20) & 1;
    const uint32_t rt2 = (op >> 16) & 0xf;
    const uint32_t rt = (op >> 12) & 0xf;

    if ((op & 0xd0) != 0x10)
    {
        return Undefined(op);
    }

    uint32_t first;
    if ((op >> 8) & 1) // FMDRR / FMRRD
    {
        const uint32_t m = Dm(op);
        if (m >= 16)
        {
            return Undefined(op);
        }

        first = 2 * m;
    }
    else // FMSRR / FMRRS
    {
        first = Sm(op);
        if (first == 31)
        {
            return Undefined(op);
        }
    }

    if (l)
    {
        state_.r[rt] = state_.vfp[first];
        state_.r[rt2] = state_.vfp[first + 1];
    }
    else
    {
        state_.vfp[first] = state_.r[rt];
        state_.vfp[first + 1] = state_.r[rt2];
    }

    return true;
}

} // namespace threesf::arm
