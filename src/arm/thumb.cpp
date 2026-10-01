// SPDX-License-Identifier: MIT

// ARM11 interpreter: the Thumb-1 instruction set of ARMv6K (see cpu.h). There's no Thumb-2 on the ARM11, so BL and BLX
// are pairs of 16-bit instructions that are executed one at a time.

#include <bit>
#include <cstdint>

#include "arm/cpu.h"

namespace threesf::arm
{

bool Cpu::StepThumb(uint32_t op)
{
    auto& r = state_.r;
    switch (op >> 11)
    {
    case 0x00: // LSL Rd, Rm, #imm
        {
            const uint32_t rd = op & 7, rm = (op >> 3) & 7, imm = (op >> 6) & 0x1f;
            bool carry = state_.c;
            r[rd] = ShiftImm(r[rm], 0, imm, carry);
            SetNZ(r[rd]);
            state_.c = carry;
            return true;
        }

    case 0x01: // LSR Rd, Rm, #imm
    case 0x02: // ASR Rd, Rm, #imm
        {
            const uint32_t rd = op & 7, rm = (op >> 3) & 7, imm = (op >> 6) & 0x1f;
            bool carry = state_.c;
            r[rd] = ShiftImm(r[rm], op >> 11, imm, carry);
            SetNZ(r[rd]);
            state_.c = carry;
            return true;
        }

    case 0x03: // ADD/SUB Rd, Rn, Rm / #imm3
        {
            const uint32_t rd = op & 7, rn = (op >> 3) & 7, x = (op >> 6) & 7;
            const uint32_t b = (op & (1u << 10)) ? x : r[x];
            if (op & (1u << 9))
            {
                r[rd] = AddWithCarry(r[rn], ~b, true, true);
            }
            else
            {
                r[rd] = AddWithCarry(r[rn], b, false, true);
            }
            return true;
        }

    case 0x04: // MOV Rd, #imm8
        {
            const uint32_t rd = (op >> 8) & 7;
            r[rd] = op & 0xff;
            SetNZ(r[rd]);
            return true;
        }

    case 0x05: // CMP Rn, #imm8
        AddWithCarry(r[(op >> 8) & 7], ~(op & 0xff), true, true);
        return true;

    case 0x06: // ADD Rd, #imm8
        {
            const uint32_t rd = (op >> 8) & 7;
            r[rd] = AddWithCarry(r[rd], op & 0xff, false, true);
            return true;
        }

    case 0x07: // SUB Rd, #imm8
        {
            const uint32_t rd = (op >> 8) & 7;
            r[rd] = AddWithCarry(r[rd], ~(op & 0xff), true, true);
            return true;
        }

    case 0x08:
        if (!(op & (1u << 10)))
        {
            // Data processing
            const uint32_t rd = op & 7, rm = (op >> 3) & 7;
            const uint32_t a = r[rd], b = r[rm];
            switch ((op >> 6) & 0xf)
            {
            case 0x0: // AND
                r[rd] = a & b;
                SetNZ(r[rd]);
                break;

            case 0x1: // EOR
                r[rd] = a ^ b;
                SetNZ(r[rd]);
                break;

            case 0x2:
            case 0x3:
            case 0x4:
            case 0x7: // LSL, LSR, ASR, ROR by register
                {
                    static constexpr uint32_t kType[8] = {0, 0, 0, 1, 2, 0, 0, 3};
                    bool carry = state_.c;
                    r[rd] = ShiftReg(a, kType[(op >> 6) & 7], b & 0xff, carry);
                    SetNZ(r[rd]);
                    state_.c = carry;
                    break;
                }

            case 0x5: // ADC
                r[rd] = AddWithCarry(a, b, state_.c, true);
                break;

            case 0x6: // SBC
                r[rd] = AddWithCarry(a, ~b, state_.c, true);
                break;

            case 0x8: // TST
                SetNZ(a & b);
                break;

            case 0x9: // NEG
                r[rd] = AddWithCarry(0, ~b, true, true);
                break;

            case 0xa: // CMP
                AddWithCarry(a, ~b, true, true);
                break;

            case 0xb: // CMN
                AddWithCarry(a, b, false, true);
                break;

            case 0xc: // ORR
                r[rd] = a | b;
                SetNZ(r[rd]);
                break;

            case 0xd: // MUL
                r[rd] = a * b;
                SetNZ(r[rd]);
                break;

            case 0xe: // BIC
                r[rd] = a & ~b;
                SetNZ(r[rd]);
                break;

            default: // MVN
                r[rd] = ~b;
                SetNZ(r[rd]);
                break;
            }

            return true;
        }
        else
        {
            // High register operations and BX/BLX
            const uint32_t rd = (op & 7) | ((op >> 4) & 8);
            const uint32_t rm = (op >> 3) & 0xf;
            switch ((op >> 8) & 3)
            {
            case 0: // ADD
                {
                    const uint32_t result = Reg(rd) + Reg(rm);
                    if (rd == 15)
                    {
                        BranchWritePc(result);
                    }
                    else
                    {
                        r[rd] = result;
                    }
                    return true;
                }

            case 1: // CMP
                AddWithCarry(Reg(rd), ~Reg(rm), true, true);
                return true;

            case 2: // MOV
                {
                    const uint32_t value = Reg(rm);
                    if (rd == 15)
                    {
                        BranchWritePc(value);
                    }
                    else
                    {
                        r[rd] = value;
                    }
                    return true;
                }

            default: // BX / BLX
                {
                    const uint32_t target = Reg(rm);
                    if (op & 0x80)
                    {
                        r[14] = (cur_pc_ + 2) | 1;
                    }
                    BxWritePc(target);
                    return true;
                }
            }
        }

    case 0x09: // LDR Rd, [PC, #imm8 * 4]
        {
            const uint32_t addr = ((cur_pc_ + 4) & ~3u) + (op & 0xff) * 4;
            r[(op >> 8) & 7] = mem_.Read32(addr);
            return true;
        }

    case 0x0a:
    case 0x0b: // load/store with register offset
        {
            const uint32_t rd = op & 7;
            const uint32_t addr = r[(op >> 3) & 7] + r[(op >> 6) & 7];
            switch ((op >> 9) & 7)
            {
            case 0:
                mem_.Write32(addr, r[rd]);
                break;
            case 1:
                mem_.Write16(addr, static_cast<uint16_t>(r[rd]));
                break;
            case 2:
                mem_.Write8(addr, static_cast<uint8_t>(r[rd]));
                break;
            case 3:
                r[rd] = static_cast<uint32_t>(static_cast<int8_t>(mem_.Read8(addr)));
                break;
            case 4:
                r[rd] = mem_.Read32(addr);
                break;
            case 5:
                r[rd] = mem_.Read16(addr);
                break;
            case 6:
                r[rd] = mem_.Read8(addr);
                break;
            default:
                r[rd] = static_cast<uint32_t>(static_cast<int16_t>(mem_.Read16(addr)));
                break;
            }

            return true;
        }

    case 0x0c: // STR Rd, [Rn, #imm5 * 4]
        mem_.Write32(r[(op >> 3) & 7] + ((op >> 6) & 0x1f) * 4, r[op & 7]);
        return true;

    case 0x0d: // LDR
        r[op & 7] = mem_.Read32(r[(op >> 3) & 7] + ((op >> 6) & 0x1f) * 4);
        return true;

    case 0x0e: // STRB
        mem_.Write8(r[(op >> 3) & 7] + ((op >> 6) & 0x1f), static_cast<uint8_t>(r[op & 7]));
        return true;

    case 0x0f: // LDRB
        r[op & 7] = mem_.Read8(r[(op >> 3) & 7] + ((op >> 6) & 0x1f));
        return true;

    case 0x10: // STRH
        mem_.Write16(r[(op >> 3) & 7] + ((op >> 6) & 0x1f) * 2, static_cast<uint16_t>(r[op & 7]));
        return true;

    case 0x11: // LDRH
        r[op & 7] = mem_.Read16(r[(op >> 3) & 7] + ((op >> 6) & 0x1f) * 2);
        return true;

    case 0x12: // STR Rd, [SP, #imm8 * 4]
        mem_.Write32(r[13] + (op & 0xff) * 4, r[(op >> 8) & 7]);
        return true;

    case 0x13: // LDR Rd, [SP, #imm8 * 4]
        r[(op >> 8) & 7] = mem_.Read32(r[13] + (op & 0xff) * 4);
        return true;

    case 0x14: // ADD Rd, PC, #imm8 * 4
        r[(op >> 8) & 7] = ((cur_pc_ + 4) & ~3u) + (op & 0xff) * 4;
        return true;

    case 0x15: // ADD Rd, SP, #imm8 * 4
        r[(op >> 8) & 7] = r[13] + (op & 0xff) * 4;
        return true;

    case 0x16:
    case 0x17: // miscellaneous
        switch ((op >> 8) & 0xf)
        {
        case 0x0: // ADD/SUB SP, #imm7 * 4
            if (op & 0x80)
            {
                r[13] -= (op & 0x7f) * 4;
            }
            else
            {
                r[13] += (op & 0x7f) * 4;
            }

            return true;

        case 0x2: // SXTH, SXTB, UXTH, UXTB
            {
                const uint32_t rd = op & 7, value = r[(op >> 3) & 7];
                switch ((op >> 6) & 3)
                {
                case 0:
                    r[rd] = static_cast<uint32_t>(static_cast<int16_t>(value));
                    break;
                case 1:
                    r[rd] = static_cast<uint32_t>(static_cast<int8_t>(value));
                    break;
                case 2:
                    r[rd] = value & 0xffff;
                    break;
                default:
                    r[rd] = value & 0xff;
                    break;
                }

                return true;
            }

        case 0x4:
        case 0x5: // PUSH {list, LR}
            {
                const uint32_t list = (op & 0xff) | ((op & 0x100) ? 0x4000 : 0);
                uint32_t addr = r[13] - 4 * static_cast<uint32_t>(std::popcount(list));
                r[13] = addr;
                for (uint32_t i = 0; i < 15; i++)
                {
                    if (list & (1u << i))
                    {
                        mem_.Write32(addr, r[i]);
                        addr += 4;
                    }
                }
                return true;
            }

        case 0x6:
            if (op == 0xb650) // SETEND LE (big-endian data isn't supported)
            {
                return true;
            }
            if ((op & 0xffe8) == 0xb660) // CPS: no effect in user mode
            {
                return true;
            }

            return Undefined(op);

        case 0xa: // REV, REV16, REVSH
            {
                const uint32_t rd = op & 7, value = r[(op >> 3) & 7];
                switch ((op >> 6) & 3)
                {
                case 0:
                    r[rd] = ByteSwap32(value);
                    return true;

                case 1:
                    r[rd] = ((value >> 8) & 0x00ff00ff) | ((value << 8) & 0xff00ff00);
                    return true;

                case 3:
                    r[rd] = static_cast<uint32_t>(static_cast<int16_t>(((value >> 8) & 0xff) | ((value & 0xff) << 8)));
                    return true;

                default:
                    return Undefined(op);
                }
            }

        case 0xc:
        case 0xd: // POP {list, PC}
            {
                const uint32_t list = op & 0xff;
                uint32_t addr = r[13];
                for (uint32_t i = 0; i < 8; i++)
                {
                    if (list & (1u << i))
                    {
                        r[i] = mem_.Read32(addr);
                        addr += 4;
                    }
                }

                if (op & 0x100)
                {
                    const uint32_t target = mem_.Read32(addr);
                    addr += 4;
                    r[13] = addr;
                    BxWritePc(target);
                }
                else
                {
                    r[13] = addr;
                }

                return true;
            }

        default:
            return Undefined(op);
        }

    case 0x18: // STMIA Rn!, {list}
        {
            const uint32_t rn = (op >> 8) & 7, list = op & 0xff;
            uint32_t addr = r[rn];
            for (uint32_t i = 0; i < 8; i++)
            {
                if (list & (1u << i))
                {
                    mem_.Write32(addr, r[i]);
                    addr += 4;
                }
            }

            r[rn] = addr;
            return true;
        }

    case 0x19: // LDMIA Rn!, {list}
        {
            const uint32_t rn = (op >> 8) & 7, list = op & 0xff;
            uint32_t addr = r[rn];
            for (uint32_t i = 0; i < 8; i++)
            {
                if (list & (1u << i))
                {
                    r[i] = mem_.Read32(addr);
                    addr += 4;
                }
            }

            if (!(list & (1u << rn)))
            {
                r[rn] = addr;
            }

            return true;
        }

    case 0x1a:
    case 0x1b: // B<cond> / SVC
        {
            const uint32_t cond = (op >> 8) & 0xf;
            if (cond == 0xf)
            {
                return Svc(op & 0xff);
            }

            if (cond == 0xe)
            {
                return Undefined(op);
            }

            if (CheckCond(cond))
            {
                r[15] = cur_pc_ + 4 + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(op & 0xff)) * 2);
            }

            return true;
        }

    case 0x1c: // B
        {
            const int32_t offset = static_cast<int32_t>(op << 21) >> 20;
            r[15] = cur_pc_ + 4 + static_cast<uint32_t>(offset);
            return true;
        }

    case 0x1d: // BLX suffix: branch to ARM
        {
            if (op & 1)
            {
                return Undefined(op);
            }

            const uint32_t target = (r[14] + ((op & 0x7ff) << 1)) & ~3u;
            r[14] = (cur_pc_ + 2) | 1;
            state_.thumb = false;
            r[15] = target;
            return true;
        }

    case 0x1e: // BL/BLX prefix
        {
            const int32_t offset = static_cast<int32_t>(op << 21) >> 9;
            r[14] = cur_pc_ + 4 + static_cast<uint32_t>(offset);
            return true;
        }

    default: // BL suffix
        {
            const uint32_t target = r[14] + ((op & 0x7ff) << 1);
            r[14] = (cur_pc_ + 2) | 1;
            r[15] = target;
            return true;
        }
    }
}

} // namespace threesf::arm
