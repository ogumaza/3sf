// SPDX-License-Identifier: MIT

// ARM11 interpreter: the ARMv6 media instructions (parallel add/subtract, pack, saturate, extend, reverse, select, dual
// 16-bit and most-significant-word multiplies, sum of absolute differences). See cpu.h.

#include <array>
#include <bit>
#include <cstdint>

#include "xsflib/bytes.h"

#include "arm/cpu.h"

namespace threesf::arm
{
namespace
{

// The low byte or halfword of `value`, extended with zeros or with its sign.
uint32_t ExtendByte(uint32_t value, bool is_unsigned)
{
    return is_unsigned ? value & 0xff : static_cast<uint32_t>(static_cast<int8_t>(value));
}

uint32_t ExtendHalf(uint32_t value, bool is_unsigned)
{
    return is_unsigned ? value & 0xffff : static_cast<uint32_t>(static_cast<int16_t>(value));
}

} // namespace

bool Cpu::Media(uint32_t op)
{
    // Operands are read from a copy in which PC reads as the instruction address + 8.
    std::array<uint32_t, 16> r = state_.r;
    r[15] = cur_pc_ + 8;
    switch ((op >> 23) & 3)
    {
    case 0:
        return MediaParallelAddSubtract(op, r);
    case 1:
        return MediaPackSaturateReverse(op, r);
    case 2:
        return MediaSignedMultiply(op, r);
    default:
        return MediaSumOfAbsoluteDifferences(op, r);
    }
}

bool Cpu::MediaParallelAddSubtract(uint32_t op, const std::array<uint32_t, 16>& r)
{
    const uint32_t op1 = (op >> 20) & 7;
    const uint32_t op2 = (op >> 5) & 7;
    auto& out = state_.r;

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
            lane =
                (is_signed ? static_cast<uint32_t>(SatSigned(sum, width, unused)) : SatUnsigned(sum, width, unused)) &
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

// Packing, unpacking, saturation and reversal. op1 is bits 22 to 20, and op2 is bits 7 to 5. A pair that none of the
// tests matches is undefined.
bool Cpu::MediaPackSaturateReverse(uint32_t op, const std::array<uint32_t, 16>& r)
{
    const uint32_t op1 = (op >> 20) & 7;
    const uint32_t op2 = (op >> 5) & 7;
    const uint32_t rn = (op >> 16) & 0xf;
    const uint32_t rd = (op >> 12) & 0xf;
    const uint32_t rm = op & 0xf;
    if (op2 == 3 && op1 != 1 && op1 != 5) // SXTAB16, SXTAB, SXTAH, UXTAB16, UXTAB, UXTAH
    {
        MediaExtend(op, r);
        return true;
    }

    if (op1 == 0 && (op2 & 1) == 0) // PKHBT, PKHTB
    {
        MediaPack(op, r);
        return true;
    }

    if ((op1 & 2) != 0 && (op2 & 1) == 0) // SSAT, USAT
    {
        MediaSaturate(op, r);
        return true;
    }

    if ((op1 == 2 || op1 == 6) && op2 == 1) // SSAT16, USAT16
    {
        MediaSaturate16(op, r);
        return true;
    }

    if (op1 == 0 && op2 == 5) // SEL: each byte from Rn where its GE flag is set and from Rm where it's clear
    {
        uint32_t result = 0;
        for (uint32_t i = 0; i < 4; i++)
        {
            result |= (((state_.ge >> i) & 1) ? r[rn] : r[rm]) & (0xffu << (8 * i));
        }

        state_.r[rd] = result;
        return true;
    }

    if (op1 == 3 && op2 == 1) // REV
    {
        state_.r[rd] = xsflib::ByteSwap32(r[rm]);
        return true;
    }

    if (op1 == 3 && op2 == 5) // REV16
    {
        state_.r[rd] = Rev16(r[rm]);
        return true;
    }

    if (op1 == 7 && op2 == 5) // REVSH
    {
        state_.r[rd] = Revsh(r[rm]);
        return true;
    }

    return Undefined(op);
}

// PKHBT takes the bottom half of Rn and the top half of Rm shifted left. PKHTB (bit 6) takes the top half of Rn and the
// bottom half of Rm shifted right arithmetically. A shift right by 0 encodes a shift by 32.
void Cpu::MediaPack(uint32_t op, const std::array<uint32_t, 16>& r)
{
    const uint32_t rn = (op >> 16) & 0xf;
    const uint32_t rd = (op >> 12) & 0xf;
    const uint32_t rm = op & 0xf;
    const uint32_t imm = (op >> 7) & 0x1f;
    if ((op & 0x40) == 0)
    {
        state_.r[rd] = (r[rn] & 0xffff) | ((r[rm] << imm) & 0xffff0000);
        return;
    }

    const uint32_t shifted = static_cast<uint32_t>(static_cast<int32_t>(r[rm]) >> (imm == 0 ? 31 : imm));
    state_.r[rd] = (r[rn] & 0xffff0000) | (shifted & 0xffff);
}

// SSAT and USAT saturate Rm, shifted left or arithmetically right (bit 6) by an immediate. A shift right by 0 encodes a
// shift by 32. SSAT saturates to a signed width of sat + 1 bits. USAT (bit 22) saturates to an unsigned width of sat
// bits. A saturation sets Q.
void Cpu::MediaSaturate(uint32_t op, const std::array<uint32_t, 16>& r)
{
    const uint32_t sat = (op >> 16) & 0x1f;
    const uint32_t rd = (op >> 12) & 0xf;
    const uint32_t rm = op & 0xf;
    const uint32_t imm = (op >> 7) & 0x1f;
    const int64_t value =
        (op & 0x40) ? static_cast<int32_t>(r[rm]) >> (imm == 0 ? 31 : imm) : static_cast<int32_t>(r[rm] << imm);
    bool saturated = false;
    if ((op >> 22) & 1)
    {
        state_.r[rd] = SatUnsigned(value, sat, saturated);
    }
    else
    {
        state_.r[rd] = static_cast<uint32_t>(SatSigned(value, sat + 1, saturated));
    }

    if (saturated)
    {
        state_.q = true;
    }
}

// SSAT16 and USAT16 saturate each halfword of Rm. SSAT16 saturates to a signed width of sat + 1 bits. USAT16 (bit 22)
// saturates to an unsigned width of sat bits. A saturation of either halfword sets Q.
void Cpu::MediaSaturate16(uint32_t op, const std::array<uint32_t, 16>& r)
{
    const uint32_t sat = (op >> 16) & 0xf;
    const uint32_t rd = (op >> 12) & 0xf;
    const uint32_t rm = op & 0xf;
    const auto lo = static_cast<int16_t>(r[rm]);
    const auto hi = static_cast<int16_t>(r[rm] >> 16);
    bool saturated = false;
    if ((op >> 22) & 1)
    {
        state_.r[rd] = (SatUnsigned(lo, sat, saturated) & 0xffff) | (SatUnsigned(hi, sat, saturated) << 16);
    }
    else
    {
        state_.r[rd] = (static_cast<uint32_t>(SatSigned(lo, sat + 1, saturated)) & 0xffff) |
                       (static_cast<uint32_t>(SatSigned(hi, sat + 1, saturated)) << 16);
    }

    if (saturated)
    {
        state_.q = true;
    }
}

// SXTAB16, SXTAB, SXTAH, UXTAB16, UXTAB and UXTAH. Rm is rotated right by 0, 8, 16 or 24 bits. Bit 22 selects zero
// extension over sign extension. Bits 21 and 20 select two bytes (0), a byte (2) or a halfword (3). A byte or a
// halfword is extended and added to Rn. Two bytes, at bits 0 and 16, are each extended to a halfword and added to Rn's
// halves. With Rn = PC, nothing is added: SXTB16, SXTB, SXTH, UXTB16, UXTB and UXTH.
void Cpu::MediaExtend(uint32_t op, const std::array<uint32_t, 16>& r)
{
    const uint32_t rn = (op >> 16) & 0xf;
    const uint32_t rd = (op >> 12) & 0xf;
    const uint32_t size = (op >> 20) & 3;
    const bool is_unsigned = (op >> 22) & 1;
    const uint32_t rotated = std::rotr(r[op & 0xf], static_cast<int>(((op >> 10) & 3) * 8));
    if (size == 0)
    {
        uint32_t lo = ExtendByte(rotated, is_unsigned);
        uint32_t hi = ExtendByte(rotated >> 16, is_unsigned);
        if (rn != 15)
        {
            lo += r[rn];
            hi += r[rn] >> 16;
        }

        state_.r[rd] = (lo & 0xffff) | (hi << 16);
        return;
    }

    const uint32_t value = size == 2 ? ExtendByte(rotated, is_unsigned) : ExtendHalf(rotated, is_unsigned);
    state_.r[rd] = rn == 15 ? value : r[rn] + value;
}

bool Cpu::MediaSignedMultiply(uint32_t op, const std::array<uint32_t, 16>& r)
{
    const uint32_t op1 = (op >> 20) & 7;
    const uint32_t op2 = (op >> 5) & 7;
    const uint32_t rd = (op >> 16) & 0xf; // Rd or RdHi
    const uint32_t ra = (op >> 12) & 0xf; // Ra or RdLo
    const uint32_t rm = (op >> 8) & 0xf;
    const uint32_t rn = op & 0xf;
    auto& out = state_.r;

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
            const uint64_t product =
                static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(r[rn])) * static_cast<int32_t>(r[rm]));
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

bool Cpu::MediaSumOfAbsoluteDifferences(uint32_t op, const std::array<uint32_t, 16>& r)
{
    const uint32_t op1 = (op >> 20) & 7;
    const uint32_t op2 = (op >> 5) & 7;
    const uint32_t rd = (op >> 16) & 0xf;
    const uint32_t ra = (op >> 12) & 0xf;
    const uint32_t rm = (op >> 8) & 0xf;
    const uint32_t rn = op & 0xf;
    auto& out = state_.r;

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
