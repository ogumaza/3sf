// SPDX-License-Identifier: MIT

// ARM11 interpreter: the ARMv6 media instructions (parallel add/subtract, pack, saturate, extend, reverse, select, dual
// 16-bit and most-significant-word multiplies, sum of absolute differences). See cpu.h.

#include <array>
#include <bit>
#include <cstdint>

#include "arm/cpu.h"

namespace threesf::arm
{
namespace
{

int32_t SatSigned(int64_t value, uint32_t bits, bool& saturated)
{
    const int64_t max = (int64_t{1} << (bits - 1)) - 1;
    const int64_t min = -(int64_t{1} << (bits - 1));
    if (value > max)
    {
        saturated = true;
        return static_cast<int32_t>(max);
    }

    if (value < min)
    {
        saturated = true;
        return static_cast<int32_t>(min);
    }

    return static_cast<int32_t>(value);
}

uint32_t SatUnsigned(int64_t value, uint32_t bits, bool& saturated)
{
    const int64_t max = (int64_t{1} << bits) - 1;
    if (value > max)
    {
        saturated = true;
        return static_cast<uint32_t>(max);
    }

    if (value < 0)
    {
        saturated = true;
        return 0;
    }

    return static_cast<uint32_t>(value);
}

} // namespace

bool Cpu::Media(uint32_t op)
{
    const uint32_t group = (op >> 23) & 3;
    const uint32_t op1 = (op >> 20) & 7;
    const uint32_t op2 = (op >> 5) & 7;

    // Operands are read from a copy in which PC reads as the instruction address + 8.
    std::array<uint32_t, 16> r = state_.r;
    r[15] = cur_pc_ + 8;
    auto& out = state_.r;
    if (group == 0)
    {
        // Parallel add/subtract: op1 selects S/Q/SH/U/UQ/UH, op2 the operation.
        if (op1 == 0 || op1 == 4 || op2 == 5 || op2 == 6)
        {
            return Undefined(op);
        }

        const uint32_t rn = (op >> 16) & 0xf, rd = (op >> 12) & 0xf, rm = op & 0xf;
        const bool is_signed = op1 < 4;
        const uint32_t kind = op1 & 3; // 1 modulo (sets GE), 2 saturating, 3 halving
        const bool bytes = op2 >= 4;
        const int lanes = bytes ? 4 : 2;
        const uint32_t width = bytes ? 8 : 16;
        const uint32_t mask = bytes ? 0xff : 0xffff;
        const uint32_t a = r[rn], b = r[rm];

        const auto get = [&](uint32_t value, int lane) -> int32_t
        {
            const uint32_t field = (value >> (lane * width)) & mask;
            if (!is_signed)
            {
                return static_cast<int32_t>(field);
            }

            return bytes ? static_cast<int8_t>(field) : static_cast<int16_t>(field);
        };

        const bool exchange = op2 == 1 || op2 == 2;
        uint32_t result = 0;
        uint8_t ge = 0;
        for (int i = 0; i < lanes; i++)
        {
            const int32_t x = get(a, i);
            const int32_t y = get(b, exchange ? 1 - i : i);
            bool add;
            switch (op2)
            {
            case 0: // ADD16
            case 4: // ADD8
                add = true;
                break;
            case 1: // ASX: lo = a.lo - b.hi, hi = a.hi + b.lo
                add = i == 1;
                break;
            case 2: // SAX: lo = a.lo + b.hi, hi = a.hi - b.lo
                add = i == 0;
                break;
            default: // SUB16, SUB8
                add = false;
                break;
            }

            const int32_t sum = add ? x + y : x - y;
            uint32_t lane;
            bool unused = false;
            switch (kind)
            {
            case 1:
                {
                    lane = static_cast<uint32_t>(sum) & mask;
                    const bool flag = (is_signed || !add) ? sum >= 0 : sum >= static_cast<int32_t>(1u << width);
                    if (flag)
                    {
                        ge |= static_cast<uint8_t>(bytes ? (1u << i) : (3u << (2 * i)));
                    }
                    break;
                }

            case 2:
                lane = (is_signed ? static_cast<uint32_t>(SatSigned(sum, width, unused))
                                  : SatUnsigned(sum, width, unused)) &
                       mask;
                break;

            default:
                lane = static_cast<uint32_t>(sum >> 1) & mask;
                break;
            }

            result |= lane << (i * width);
        }

        out[rd] = result;
        if (kind == 1)
        {
            state_.ge = ge;
        }
        return true;
    }

    if (group == 1)
    {
        const uint32_t rn = (op >> 16) & 0xf, rd = (op >> 12) & 0xf, rm = op & 0xf;
        const uint32_t rot = ((op >> 10) & 3) * 8;
        const uint32_t rotated = std::rotr(r[rm], static_cast<int>(rot));
        switch (op1)
        {
        case 0:
            if ((op2 & 1) == 0) // PKHBT / PKHTB
            {
                const uint32_t imm = (op >> 7) & 0x1f;
                if (!(op & 0x40))
                {
                    out[rd] = (r[rn] & 0xffff) | ((r[rm] << imm) & 0xffff0000);
                }
                else
                {
                    const uint32_t shifted =
                        static_cast<uint32_t>(static_cast<int32_t>(r[rm]) >> (imm == 0 ? 31 : imm));
                    out[rd] = (r[rn] & 0xffff0000) | (shifted & 0xffff);
                }
                return true;
            }

            if (op2 == 3) // SXTAB16 / SXTB16
            {
                uint32_t lo = static_cast<uint32_t>(static_cast<int8_t>(rotated & 0xff));
                uint32_t hi = static_cast<uint32_t>(static_cast<int8_t>((rotated >> 16) & 0xff));
                if (rn != 15)
                {
                    lo += r[rn];
                    hi += r[rn] >> 16;
                }
                out[rd] = (lo & 0xffff) | (hi << 16);
                return true;
            }

            if (op2 == 5) // SEL
            {
                uint32_t result = 0;
                for (uint32_t i = 0; i < 4; i++)
                {
                    result |= (((state_.ge >> i) & 1) ? r[rn] : r[rm]) & (0xffu << (8 * i));
                }
                out[rd] = result;
                return true;
            }

            return Undefined(op);

        case 2:
        case 3:
        case 6:
        case 7:
            {
                const bool is_unsigned = op1 >= 6;
                if ((op2 & 1) == 0) // SSAT / USAT
                {
                    const uint32_t sat = (op >> 16) & 0x1f;
                    const uint32_t imm = (op >> 7) & 0x1f;
                    int64_t value;
                    if (op & 0x40)
                    {
                        value = static_cast<int32_t>(r[rm]) >> (imm == 0 ? 31 : imm);
                    }
                    else
                    {
                        value = static_cast<int32_t>(r[rm] << imm);
                    }

                    bool saturated = false;
                    if (is_unsigned)
                    {
                        out[rd] = SatUnsigned(value, sat, saturated);
                    }
                    else
                    {
                        out[rd] = static_cast<uint32_t>(SatSigned(value, sat + 1, saturated));
                    }

                    if (saturated)
                    {
                        state_.q = true;
                    }
                    return true;
                }

                if (op2 == 1 && (op1 & 1) == 0) // SSAT16 / USAT16
                {
                    const uint32_t sat = (op >> 16) & 0xf;
                    bool saturated = false;
                    uint32_t lo, hi;
                    if (is_unsigned)
                    {
                        lo = SatUnsigned(static_cast<int16_t>(r[rm]), sat, saturated);
                        hi = SatUnsigned(static_cast<int16_t>(r[rm] >> 16), sat, saturated);
                    }
                    else
                    {
                        lo = static_cast<uint32_t>(SatSigned(static_cast<int16_t>(r[rm]), sat + 1, saturated));
                        hi = static_cast<uint32_t>(SatSigned(static_cast<int16_t>(r[rm] >> 16), sat + 1, saturated));
                    }

                    out[rd] = (lo & 0xffff) | (hi << 16);
                    if (saturated)
                    {
                        state_.q = true;
                    }
                    return true;
                }

                if (op2 == 3) // SXTAB, SXTAH, UXTAB, UXTAH (no add when Rn = PC)
                {
                    const bool half = op1 & 1;
                    uint32_t value;
                    if (is_unsigned)
                    {
                        value = half ? (rotated & 0xffff) : (rotated & 0xff);
                    }
                    else
                    {
                        value = half ? static_cast<uint32_t>(static_cast<int16_t>(rotated))
                                     : static_cast<uint32_t>(static_cast<int8_t>(rotated));
                    }

                    out[rd] = rn == 15 ? value : r[rn] + value;
                    return true;
                }

                const uint32_t sub = ((op1 & 1) << 3) | op2;
                if (!is_unsigned && sub == 0x9) // REV
                {
                    out[rd] = ByteSwap32(r[rm]);
                    return true;
                }

                if (!is_unsigned && sub == 0xd) // REV16
                {
                    const uint32_t v = r[rm];
                    out[rd] = ((v >> 8) & 0x00ff00ff) | ((v << 8) & 0xff00ff00);
                    return true;
                }

                if (is_unsigned && sub == 0xd) // REVSH
                {
                    const uint32_t v = r[rm];
                    out[rd] = static_cast<uint32_t>(static_cast<int16_t>(((v >> 8) & 0xff) | ((v & 0xff) << 8)));
                    return true;
                }

                return Undefined(op);
            }

        case 4:
            if (op2 == 3) // UXTAB16 / UXTB16
            {
                uint32_t lo = rotated & 0xff;
                uint32_t hi = (rotated >> 16) & 0xff;
                if (rn != 15)
                {
                    lo += r[rn];
                    hi += r[rn] >> 16;
                }
                out[rd] = (lo & 0xffff) | (hi << 16);
                return true;
            }

            return Undefined(op);

        default:
            return Undefined(op);
        }
    }

    const uint32_t rd = (op >> 16) & 0xf; // Rd or RdHi
    const uint32_t ra = (op >> 12) & 0xf; // Ra or RdLo
    const uint32_t rm = (op >> 8) & 0xf;
    const uint32_t rn = op & 0xf;
    if (group == 2)
    {
        const uint32_t m = (op2 & 1) ? std::rotr(r[rm], 16) : r[rm];
        const int64_t p1 = static_cast<int64_t>(static_cast<int16_t>(r[rn])) * static_cast<int16_t>(m);
        const int64_t p2 = static_cast<int64_t>(static_cast<int16_t>(r[rn] >> 16)) * static_cast<int16_t>(m >> 16);
        switch (op1)
        {
        case 0: // SMLAD, SMUAD, SMLSD, SMUSD
            {
                if (op2 & 4)
                {
                    return Undefined(op);
                }

                int64_t sum = (op2 & 2) ? p1 - p2 : p1 + p2;
                if (ra != 15)
                {
                    sum += static_cast<int32_t>(r[ra]);
                }

                out[rd] = static_cast<uint32_t>(sum);
                if (sum != static_cast<int32_t>(sum))
                {
                    state_.q = true;
                }
                return true;
            }

        case 4: // SMLALD, SMLSLD
            {
                if (op2 & 4)
                {
                    return Undefined(op);
                }

                const uint64_t acc = (static_cast<uint64_t>(r[rd]) << 32) | r[ra];
                const uint64_t result = acc + static_cast<uint64_t>((op2 & 2) ? p1 - p2 : p1 + p2);
                out[ra] = static_cast<uint32_t>(result);
                out[rd] = static_cast<uint32_t>(result >> 32);
                return true;
            }

        case 5: // SMMLA, SMMUL, SMMLS
            {
                const uint64_t product = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(r[rn])) *
                                                               static_cast<int32_t>(r[rm]));
                uint64_t result;
                if ((op2 & 6) == 0)
                {
                    result = (ra == 15 ? 0 : (static_cast<uint64_t>(r[ra]) << 32)) + product;
                }
                else if ((op2 & 6) == 6)
                {
                    result = (static_cast<uint64_t>(r[ra]) << 32) - product;
                }
                else
                {
                    return Undefined(op);
                }

                if (op2 & 1)
                {
                    result += 0x80000000u;
                }

                out[rd] = static_cast<uint32_t>(result >> 32);
                return true;
            }

        default:
            return Undefined(op);
        }
    }

    // Group 3: sum of absolute differences.
    if (op1 == 0 && op2 == 0) // USAD8 / USADA8
    {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < 4; i++)
        {
            const int32_t x = static_cast<int32_t>((r[rn] >> (8 * i)) & 0xff);
            const int32_t y = static_cast<int32_t>((r[rm] >> (8 * i)) & 0xff);
            sum += static_cast<uint32_t>(x > y ? x - y : y - x);
        }
        out[rd] = ra == 15 ? sum : sum + r[ra];
        return true;
    }

    return Undefined(op);
}

} // namespace threesf::arm
