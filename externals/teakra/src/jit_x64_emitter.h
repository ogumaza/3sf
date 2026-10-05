#pragma once

// A small x86-64 assembler for the JIT (jit.h): the instructions the translated DSP code uses,
// written into a byte vector, with labels for jumps. It runs on any host; only running the code
// needs x86-64.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include "common_types.h"
#include "crash.h"

namespace Teakra::X64 {

enum class Reg : u8 {
    rax,
    rcx,
    rdx,
    rbx,
    rsp,
    rbp,
    rsi,
    rdi,
    r8,
    r9,
    r10,
    r11,
    r12,
    r13,
    r14,
    r15,
};

// A memory operand: [base + index * scale + disp].
struct Mem {
    Reg base;
    s32 disp = 0;
    bool has_index = false;
    Reg index = Reg::rax;
    u8 scale = 1;
};

inline Mem Ptr(Reg base, s32 disp = 0) {
    return Mem{base, disp};
}

inline Mem Ptr(Reg base, Reg index, u8 scale, s32 disp = 0) {
    return Mem{base, disp, true, index, scale};
}

// Condition codes, as in the jcc, setcc and cmovcc encodings.
enum class CC : u8 {
    o,
    no,
    b,
    ae,
    e,
    ne,
    be,
    a,
    s,
    ns,
    p,
    np,
    l,
    ge,
    le,
    g,
};

enum class Arith : u8 { add, or_, adc, sbb, and_, sub, xor_, cmp };
enum class Shift : u8 { rol = 0, ror = 1, shl = 4, shr = 5, sar = 7 };

class Label {
public:
    bool Bound() const {
        return position != Unbound;
    }
    // True if a jump to it has been emitted.
    bool Used() const {
        return Bound() || !fixups.empty();
    }

private:
    friend class Emitter;
    static constexpr std::size_t Unbound = std::numeric_limits<std::size_t>::max();
    std::size_t position = Unbound;
    std::vector<std::size_t> fixups; // offsets of rel32 fields that jump here
};

class Emitter {
public:
    std::vector<u8> code;

    std::size_t Size() const {
        return code.size();
    }

    // --- Labels and jumps (always rel32, so that a jump's size doesn't depend on its target).

    void Bind(Label& label) {
        ASSERT(!label.Bound());
        label.position = code.size();
        for (std::size_t fixup : label.fixups) {
            Patch32(fixup, static_cast<u32>(label.position - (fixup + 4)));
        }
        label.fixups.clear();
    }

    void Jmp(Label& label) {
        Byte(0xE9);
        Rel32(label);
    }

    void J(CC cond, Label& label) {
        Byte(0x0F);
        Byte(0x80 | static_cast<u8>(cond));
        Rel32(label);
    }

    // --- Data movement. `bits` is the operand size: 8, 16, 32 or 64. A 32-bit write to a
    // register clears its upper half, as on the hardware.

    void Mov(Reg dst, Reg src, int bits) {
        RR(bits == 8 ? 0x88 : 0x89, bits, src, dst);
    }
    void Mov(Reg dst, const Mem& src, int bits) {
        RM(bits == 8 ? 0x8A : 0x8B, bits, dst, src);
    }
    void Mov(const Mem& dst, Reg src, int bits) {
        RM(bits == 8 ? 0x88 : 0x89, bits, src, dst);
    }
    // Stores an immediate; a 64-bit store sign-extends it from 32 bits.
    void MovImm(const Mem& dst, s32 imm, int bits) {
        RM(bits == 8 ? 0xC6 : 0xC7, bits, 0, dst, ImmSize(bits));
        Imm(imm, ImmSize(bits));
    }
    // Loads an immediate into a register, in the shortest form that gives its 64-bit value.
    void MovImm(Reg dst, u64 imm) {
        if (imm <= 0xFFFF'FFFF) {
            Rex(false, 0, 0, Num(dst));
            Byte(0xB8 | (Num(dst) & 7));
            Imm(static_cast<s32>(static_cast<u32>(imm)), 4);
        } else if (static_cast<s64>(imm) >= std::numeric_limits<s32>::min() &&
                   static_cast<s64>(imm) <= std::numeric_limits<s32>::max()) {
            RR(0xC7, 64, Reg::rax, dst); // reg field 0: mov r/m64, imm32
            Imm(static_cast<s32>(imm), 4);
        } else {
            Rex(true, 0, 0, Num(dst), true);
            Byte(0xB8 | (Num(dst) & 7));
            for (int i = 0; i < 8; ++i) {
                Byte(static_cast<u8>(imm >> (i * 8)));
            }
        }
    }

    // Zero-extends 8 or 16 bits into a 32-bit register (which clears the upper half too).
    void Movzx(Reg dst, Reg src, int src_bits) {
        ASSERT(src_bits == 8 || src_bits == 16);
        Rex(false, Num(dst), 0, Num(src), src_bits == 8 && Num(src) >= 4);
        Byte(0x0F);
        Byte(src_bits == 8 ? 0xB6 : 0xB7);
        ModRMReg(Num(dst), Num(src));
    }
    void Movzx(Reg dst, const Mem& src, int src_bits) {
        ASSERT(src_bits == 8 || src_bits == 16);
        RexMem(false, Num(dst), src, false);
        Byte(0x0F);
        Byte(src_bits == 8 ? 0xB6 : 0xB7);
        ModRMMem(Num(dst), src);
    }
    // Sign-extends 8, 16 or 32 bits into a 32- or 64-bit register.
    void Movsx(Reg dst, Reg src, int src_bits, int dst_bits) {
        ASSERT(dst_bits == 32 || dst_bits == 64);
        if (src_bits == 32) {
            ASSERT(dst_bits == 64);
            RR(0x63, 64, dst, src); // movsxd: the reg field is the destination
            return;
        }
        ASSERT(src_bits == 8 || src_bits == 16);
        Rex(dst_bits == 64, Num(dst), 0, Num(src), src_bits == 8 && Num(src) >= 4);
        Byte(0x0F);
        Byte(src_bits == 8 ? 0xBE : 0xBF);
        ModRMReg(Num(dst), Num(src));
    }
    void Movsx(Reg dst, const Mem& src, int src_bits, int dst_bits) {
        ASSERT(dst_bits == 32 || dst_bits == 64);
        if (src_bits == 32) {
            ASSERT(dst_bits == 64);
            RexMem(true, Num(dst), src, false);
            Byte(0x63);
            ModRMMem(Num(dst), src);
            return;
        }
        ASSERT(src_bits == 8 || src_bits == 16);
        RexMem(dst_bits == 64, Num(dst), src, false);
        Byte(0x0F);
        Byte(src_bits == 8 ? 0xBE : 0xBF);
        ModRMMem(Num(dst), src);
    }

    void Lea(Reg dst, const Mem& src, int bits = 64) {
        ASSERT(bits == 32 || bits == 64);
        RM(0x8D, bits, dst, src);
    }

    // --- Arithmetic and logic.

    void Op(Arith op, Reg dst, Reg src, int bits) {
        const u8 base = static_cast<u8>(op) << 3;
        RR(bits == 8 ? base : base | 1, bits, src, dst);
    }
    void Op(Arith op, Reg dst, const Mem& src, int bits) {
        const u8 base = static_cast<u8>(op) << 3;
        RM(bits == 8 ? base | 2 : base | 3, bits, dst, src);
    }
    void Op(Arith op, const Mem& dst, Reg src, int bits) {
        const u8 base = static_cast<u8>(op) << 3;
        RM(bits == 8 ? base : base | 1, bits, src, dst);
    }
    // With a 64-bit operand the immediate is sign-extended from 32 bits.
    void OpImm(Arith op, Reg dst, s32 imm, int bits) {
        const u8 ext = static_cast<u8>(op);
        if (bits == 8) {
            RRExt(0x80, bits, ext, dst);
            Imm(imm, 1);
        } else if (imm >= -128 && imm <= 127) {
            RRExt(0x83, bits, ext, dst);
            Imm(imm, 1);
        } else {
            RRExt(0x81, bits, ext, dst);
            Imm(imm, ImmSize(bits));
        }
    }
    void OpImm(Arith op, const Mem& dst, s32 imm, int bits) {
        const u8 ext = static_cast<u8>(op);
        if (bits == 8) {
            RM(0x80, bits, ext, dst, 1);
            Imm(imm, 1);
        } else if (imm >= -128 && imm <= 127) {
            RM(0x83, bits, ext, dst, 1);
            Imm(imm, 1);
        } else {
            RM(0x81, bits, ext, dst, ImmSize(bits));
            Imm(imm, ImmSize(bits));
        }
    }

    void Test(Reg a, Reg b, int bits) {
        RR(bits == 8 ? 0x84 : 0x85, bits, b, a);
    }
    void TestImm(Reg a, s32 imm, int bits) {
        RRExt(bits == 8 ? 0xF6 : 0xF7, bits, 0, a);
        Imm(imm, bits == 8 ? 1 : ImmSize(bits));
    }

    void ShiftImm(Shift op, Reg dst, u8 count, int bits) {
        if (count == 1) {
            RRExt(bits == 8 ? 0xD0 : 0xD1, bits, static_cast<u8>(op), dst);
        } else {
            RRExt(bits == 8 ? 0xC0 : 0xC1, bits, static_cast<u8>(op), dst);
            Byte(count);
        }
    }
    // Shifts by cl.
    void ShiftCl(Shift op, Reg dst, int bits) {
        RRExt(bits == 8 ? 0xD2 : 0xD3, bits, static_cast<u8>(op), dst);
    }

    void Imul(Reg dst, Reg src, int bits) {
        ASSERT(bits != 8);
        Prefix16(bits);
        Rex(bits == 64, Num(dst), 0, Num(src));
        Byte(0x0F);
        Byte(0xAF);
        ModRMReg(Num(dst), Num(src));
    }

    void Neg(Reg dst, int bits) {
        RRExt(bits == 8 ? 0xF6 : 0xF7, bits, 3, dst);
    }
    void Not(Reg dst, int bits) {
        RRExt(bits == 8 ? 0xF6 : 0xF7, bits, 2, dst);
    }

    // Sets the low byte of dst to 1 if cond holds, else 0 (the rest of dst is unchanged).
    void Set(CC cond, Reg dst) {
        Rex(false, 0, 0, Num(dst), Num(dst) >= 4);
        Byte(0x0F);
        Byte(0x90 | static_cast<u8>(cond));
        ModRMReg(0, Num(dst));
    }

    void Cmov(CC cond, Reg dst, Reg src, int bits) {
        ASSERT(bits != 8);
        Prefix16(bits);
        Rex(bits == 64, Num(dst), 0, Num(src));
        Byte(0x0F);
        Byte(0x40 | static_cast<u8>(cond));
        ModRMReg(Num(dst), Num(src));
    }

    // --- Calls and the stack.

    void Push(Reg r) {
        Rex(false, 0, 0, Num(r));
        Byte(0x50 | (Num(r) & 7));
    }
    void Pop(Reg r) {
        Rex(false, 0, 0, Num(r));
        Byte(0x58 | (Num(r) & 7));
    }
    void Ret() {
        Byte(0xC3);
    }
    void CallReg(Reg target) {
        RRExt(0xFF, 32, 2, target); // call r/m64 needs no REX.W
    }

private:
    static u8 Num(Reg r) {
        return static_cast<u8>(r);
    }

    static int ImmSize(int bits) {
        return bits == 8 ? 1 : bits == 16 ? 2 : 4;
    }

    void Byte(u8 b) {
        code.push_back(b);
    }

    void Imm(s32 value, int size) {
        for (int i = 0; i < size; ++i) {
            Byte(static_cast<u8>(static_cast<u32>(value) >> (i * 8)));
        }
    }

    void Patch32(std::size_t at, u32 value) {
        for (int i = 0; i < 4; ++i) {
            code[at + i] = static_cast<u8>(value >> (i * 8));
        }
    }

    void Rel32(Label& label) {
        const std::size_t at = code.size();
        Imm(0, 4);
        if (label.Bound()) {
            Patch32(at, static_cast<u32>(label.position - (at + 4)));
        } else {
            label.fixups.push_back(at);
        }
    }

    void Prefix16(int bits) {
        if (bits == 16) {
            Byte(0x66);
        }
    }

    // A REX prefix when one is needed: for a 64-bit operand, a register numbered 8 or more, or
    // (force) an 8-bit access to spl, bpl, sil or dil.
    void Rex(bool w, u8 reg, u8 index, u8 base, bool force = false) {
        const u8 rex =
            0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((index & 8) ? 2 : 0) | ((base & 8) ? 1 : 0);
        if (rex != 0x40 || force) {
            Byte(rex);
        }
    }

    void RexMem(bool w, u8 reg, const Mem& m, bool force) {
        Rex(w, reg, m.has_index ? Num(m.index) : 0, Num(m.base), force);
    }

    void ModRMReg(u8 reg, u8 rm) {
        Byte(0xC0 | ((reg & 7) << 3) | (rm & 7));
    }

    // ModRM, SIB and displacement for a memory operand.
    void ModRMMem(u8 reg, const Mem& m) {
        const u8 base = Num(m.base) & 7;
        u8 mod;
        if (m.disp == 0 && base != 5) { // rbp and r13 as a base always take a displacement
            mod = 0;
        } else if (m.disp >= -128 && m.disp <= 127) {
            mod = 1;
        } else {
            mod = 2;
        }
        if (m.has_index || base == 4) { // rsp and r12 as a base need a SIB byte
            ASSERT(!m.has_index || m.index != Reg::rsp);
            const u8 scale = m.scale == 1 ? 0 : m.scale == 2 ? 1 : m.scale == 4 ? 2 : 3;
            const u8 index = m.has_index ? (Num(m.index) & 7) : 4;
            Byte((mod << 6) | ((reg & 7) << 3) | 4);
            Byte((scale << 6) | (index << 3) | base);
        } else {
            Byte((mod << 6) | ((reg & 7) << 3) | base);
        }
        if (mod == 1) {
            Imm(m.disp, 1);
        } else if (mod == 2) {
            Imm(m.disp, 4);
        }
    }

    // An instruction with a register (reg field) and a register (r/m field).
    void RR(u8 opcode, int bits, Reg reg, Reg rm) {
        Prefix16(bits);
        Rex(bits == 64, Num(reg), 0, Num(rm), bits == 8 && (Num(reg) >= 4 || Num(rm) >= 4));
        Byte(opcode);
        ModRMReg(Num(reg), Num(rm));
    }

    // An instruction with an opcode extension in the reg field and a register in r/m.
    void RRExt(u8 opcode, int bits, u8 ext, Reg rm) {
        Prefix16(bits);
        Rex(bits == 64, 0, 0, Num(rm), bits == 8 && Num(rm) >= 4);
        Byte(opcode);
        ModRMReg(ext, Num(rm));
    }

    // An instruction with a register or an opcode extension in the reg field and memory in r/m.
    void RM(u8 opcode, int bits, Reg reg, const Mem& m) {
        Prefix16(bits);
        RexMem(bits == 64, Num(reg), m, bits == 8 && Num(reg) >= 4);
        Byte(opcode);
        ModRMMem(Num(reg), m);
    }
    void RM(u8 opcode, int bits, u8 ext, const Mem& m, int /*immediate size*/) {
        Prefix16(bits);
        RexMem(bits == 64, ext, m, false);
        Byte(opcode);
        ModRMMem(ext, m);
    }
};

} // namespace Teakra::X64
