// SPDX-License-Identifier: MIT

// ARM11 interpreter: memory, run loop and the ARM instruction set (see cpu.h).

#include "arm/cpu.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace threesf::arm
{

// --------------------------------------------------------------------------------------------- Memory
Memory::Memory() : pages_(new uint8_t*[kPageCount]()), write_pages_(new uint8_t*[kPageCount]())
{
}

void Memory::Map(uint32_t vaddr, uint8_t* host, uint32_t size)
{
    generation_ = next_generation_++;
    for (uint32_t off = 0; off < size; off += kPageSize)
    {
        pages_[(vaddr + off) >> kPageBits] = host + off;
        write_pages_[(vaddr + off) >> kPageBits] = Watched(host + off) ? nullptr : host + off;
    }
}

void Memory::Unmap(uint32_t vaddr, uint32_t size)
{
    generation_ = next_generation_++;
    for (uint32_t off = 0; off < size; off += kPageSize)
    {
        pages_[(vaddr + off) >> kPageBits] = nullptr;
        write_pages_[(vaddr + off) >> kPageBits] = nullptr;
    }
}

Memory::Snapshot Memory::Save(const Snapshot* previous) const
{
    Snapshot snapshot;
    snapshot.generation = generation_;
    snapshot.fault = fault_;
    snapshot.fault_address = fault_address_;
    snapshot.fault_write = fault_write_;

    if (previous && previous->generation == generation_)
    {
        snapshot.runs = previous->runs;
        return snapshot;
    }

    auto runs = std::make_shared<std::vector<Snapshot::Run>>();
    for (uint32_t page = 0; page < kPageCount; page++)
    {
        uint8_t* host = pages_[page];
        if (!host)
        {
            continue;
        }

        // Runs are at most 2 GiB, so that Restore can map each with a 32-bit size.
        if (!runs->empty())
        {
            Snapshot::Run& last = runs->back();
            const auto end = reinterpret_cast<std::uintptr_t>(last.host) + std::uintptr_t{last.pages} * kPageSize;
            if (last.first_page + last.pages == page && end == reinterpret_cast<std::uintptr_t>(host) &&
                last.pages < kPageCount / 2)
            {
                last.pages++;
                continue;
            }
        }

        runs->push_back({page, 1, host});
    }

    snapshot.runs = std::move(runs);

    return snapshot;
}

void Memory::Restore(const Snapshot& snapshot)
{
    fault_ = snapshot.fault;
    fault_address_ = snapshot.fault_address;
    fault_write_ = snapshot.fault_write;

    if (snapshot.generation == generation_)
    {
        return;
    }

    std::fill_n(pages_.get(), kPageCount, nullptr);
    std::fill_n(write_pages_.get(), kPageCount, nullptr);
    for (const Snapshot::Run& run : *snapshot.runs)
    {
        Map(run.first_page << kPageBits, run.host, run.pages << kPageBits);
    }

    generation_ = snapshot.generation;
}

void Memory::WatchWrites(const uint8_t* host, uint32_t size, std::function<void()> on_write)
{
    watch_start_ = host;
    watch_end_ = host + size;
    on_watched_write_ = std::move(on_write);
    for (uint32_t page = 0; page < kPageCount; ++page)
    {
        if (pages_[page] && Watched(pages_[page]))
        {
            write_pages_[page] = nullptr;
        }
    }
}

void Memory::WriteSlow8(uint32_t a, uint8_t v)
{
    uint8_t* p = pages_[a >> kPageBits];
    if (!p)
    {
        Fault(a, true);
        return;
    }

    p[a & kPageMask] = v;
    on_watched_write_();
}

void Memory::ReadBlock(uint32_t vaddr, void* dst, std::size_t size) const
{
    auto* d = static_cast<uint8_t*>(dst);
    while (size)
    {
        const uint8_t* p = HostPtr(vaddr);
        if (!p)
        {
            return;
        }

        const std::size_t chunk = std::min<std::size_t>(size, kPageSize - (vaddr & kPageMask));
        std::memcpy(d, p, chunk);
        d += chunk;
        vaddr += static_cast<uint32_t>(chunk);
        size -= chunk;
    }
}

void Memory::WriteBlock(uint32_t vaddr, const void* src, std::size_t size)
{
    const auto* s = static_cast<const uint8_t*>(src);
    while (size)
    {
        uint8_t* p = HostPtr(vaddr);
        if (!p)
        {
            return;
        }

        const std::size_t chunk = std::min<std::size_t>(size, kPageSize - (vaddr & kPageMask));
        std::memcpy(p, s, chunk);
        if (Watched(p))
        {
            on_watched_write_();
        }
        s += chunk;
        vaddr += static_cast<uint32_t>(chunk);
        size -= chunk;
    }
}

void Memory::ZeroBlock(uint32_t vaddr, std::size_t size)
{
    while (size)
    {
        uint8_t* p = HostPtr(vaddr);
        if (!p)
        {
            return;
        }

        const std::size_t chunk = std::min<std::size_t>(size, kPageSize - (vaddr & kPageMask));
        std::memset(p, 0, chunk);
        if (Watched(p))
        {
            on_watched_write_();
        }
        vaddr += static_cast<uint32_t>(chunk);
        size -= chunk;
    }
}

std::string Memory::ReadCString(uint32_t vaddr, std::size_t max) const
{
    std::string s;
    while (s.size() < max)
    {
        const uint8_t* p = HostPtr(vaddr++);
        if (!p || *p == 0)
        {
            break;
        }

        s.push_back(static_cast<char>(*p));
    }

    return s;
}

// --------------------------------------------------------------------------------------------- State
uint32_t CpuState::Cpsr() const
{
    return (static_cast<uint32_t>(n) << 31) | (static_cast<uint32_t>(z) << 30) | (static_cast<uint32_t>(c) << 29) |
           (static_cast<uint32_t>(v) << 28) | (static_cast<uint32_t>(q) << 27) | (static_cast<uint32_t>(ge) << 16) |
           (static_cast<uint32_t>(thumb) << 5) | 0x10; // user mode
}

std::string Cpu::Describe() const
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "pc=%08x (%s) cpsr=%08x\n"
                  " r0=%08x  r1=%08x  r2=%08x  r3=%08x\n"
                  " r4=%08x  r5=%08x  r6=%08x  r7=%08x\n"
                  " r8=%08x  r9=%08x r10=%08x r11=%08x\n"
                  "r12=%08x  sp=%08x  lr=%08x fpscr=%08x",
                  state_.r[15], state_.thumb ? "thumb" : "arm", state_.Cpsr(), state_.r[0], state_.r[1], state_.r[2],
                  state_.r[3], state_.r[4], state_.r[5], state_.r[6], state_.r[7], state_.r[8], state_.r[9],
                  state_.r[10], state_.r[11], state_.r[12], state_.r[13], state_.r[14], state_.fpscr);

    return buf;
}

// --------------------------------------------------------------------------------------------- Run loop
StopReason Cpu::Run(uint64_t max_instructions)
{
    HostFpuMode fpu_guard;
    host_rmode_ = 0xffffffff;
    UpdateHostFpu();
    reason_ = StopReason::kNone;

    uint64_t count = 0;
    while (count < max_instructions)
    {
        const uint32_t pc = state_.r[15];
        cur_pc_ = pc;
        bool ok;
        if (state_.thumb)
        {
            const uint32_t op = mem_.Read16(pc);
            if (mem_.fault_)
            {
                reason_ = StopReason::kFault;
                stop_pc_ = pc;
                break;
            }

            state_.r[15] = pc + 2;
            ok = StepThumb(op);
        }
        else
        {
            const uint32_t op = mem_.Read32(pc);
            if (mem_.fault_)
            {
                reason_ = StopReason::kFault;
                stop_pc_ = pc;
                break;
            }

            state_.r[15] = pc + 4;
            ok = StepArm(op);
        }

        count++;

        if (mem_.fault_)
        {
            reason_ = StopReason::kFault;
            stop_pc_ = pc;
            state_.r[15] = pc;
            break;
        }

        if (!ok)
        {
            stop_pc_ = pc;
            if (reason_ == StopReason::kUndefined)
            {
                state_.r[15] = pc;
            }
            break;
        }
    }
    if (reason_ == StopReason::kNone)
    {
        reason_ = StopReason::kBudget;
    }

    executed_ += count;

    return reason_;
}

bool Cpu::Undefined(uint32_t op)
{
    stop_opcode_ = op;
    reason_ = StopReason::kUndefined;

    return false;
}

bool Cpu::Svc(uint32_t number)
{
    svc_number_ = number;
    reason_ = StopReason::kSvc;

    return false;
}

// --------------------------------------------------------------------------------------------- Shifter
uint32_t Cpu::ShiftImm(uint32_t value, uint32_t type, uint32_t amount, bool& carry)
{
    switch (type)
    {
    case 0: // LSL
        if (amount == 0)
        {
            return value;
        }

        carry = (value >> (32 - amount)) & 1;
        return value << amount;

    case 1: // LSR (#0 encodes #32)
        if (amount == 0)
        {
            carry = value >> 31;
            return 0;
        }

        carry = (value >> (amount - 1)) & 1;
        return value >> amount;

    case 2: // ASR (#0 encodes #32)
        if (amount == 0)
        {
            carry = value >> 31;
            return static_cast<uint32_t>(static_cast<int32_t>(value) >> 31);
        }

        carry = (value >> (amount - 1)) & 1;
        return static_cast<uint32_t>(static_cast<int32_t>(value) >> amount);

    default: // ROR (#0 encodes RRX)
        if (amount == 0)
        {
            const uint32_t result = (static_cast<uint32_t>(carry) << 31) | (value >> 1);
            carry = value & 1;
            return result;
        }

        carry = (value >> (amount - 1)) & 1;
        return std::rotr(value, static_cast<int>(amount));
    }
}

uint32_t Cpu::ShiftReg(uint32_t value, uint32_t type, uint32_t amount, bool& carry)
{
    if (amount == 0)
    {
        return value;
    }

    switch (type)
    {
    case 0: // LSL
        if (amount < 32)
        {
            carry = (value >> (32 - amount)) & 1;
            return value << amount;
        }

        carry = amount == 32 ? (value & 1) : 0;
        return 0;

    case 1: // LSR
        if (amount < 32)
        {
            carry = (value >> (amount - 1)) & 1;
            return value >> amount;
        }

        carry = amount == 32 ? (value >> 31) : 0;
        return 0;

    case 2: // ASR
        if (amount < 32)
        {
            carry = (value >> (amount - 1)) & 1;
            return static_cast<uint32_t>(static_cast<int32_t>(value) >> amount);
        }

        carry = value >> 31;
        return static_cast<uint32_t>(static_cast<int32_t>(value) >> 31);

    default: // ROR
        amount &= 31;
        if (amount == 0)
        {
            carry = value >> 31;
            return value;
        }

        carry = (value >> (amount - 1)) & 1;
        return std::rotr(value, static_cast<int>(amount));
    }
}

// --------------------------------------------------------------------------------------------- ARM decoder
bool Cpu::StepArm(uint32_t op)
{
    const uint32_t cond = op >> 28;
    if (cond != 0xe)
    {
        if (cond == 0xf)
        {
            return ArmUnconditional(op);
        }
        if (!CheckCond(cond))
        {
            return true;
        }
    }

    switch ((op >> 25) & 7)
    {
    case 0:
        if ((op & 0x90) == 0x90)
        {
            if ((op & 0x60) == 0)
            {
                return (op & (1u << 24)) ? ArmSwapExclusive(op) : ArmMultiply(op);
            }

            return ArmExtraLoadStore(op);
        }

        if ((op & 0x01900000) == 0x01000000)
        {
            return ArmMisc(op);
        }

        return ArmDataProcessing(op);

    case 1:
        if ((op & 0x01900000) == 0x01000000)
        {
            if (op & (1u << 21))
            {
                return ArmMsrImmediate(op);
            }

            return Undefined(op); // MOVW/MOVT are ARMv6T2
        }

        return ArmDataProcessing(op);

    case 2:
        return ArmLoadStore(op);

    case 3:
        if (op & 0x10)
        {
            return Media(op);
        }

        return ArmLoadStore(op);

    case 4:
        return ArmBlockTransfer(op);

    case 5:
        {
            const int32_t offset = static_cast<int32_t>(op << 8) >> 6;
            if (op & (1u << 24))
            {
                state_.r[14] = cur_pc_ + 4;
            }
            state_.r[15] = cur_pc_ + 8 + static_cast<uint32_t>(offset);
            return true;
        }

    case 6:
        return ArmCoprocessor(op);

    default:
        if (op & (1u << 24))
        {
            return Svc(op & 0xffffff);
        }

        return ArmCoprocessor(op);
    }
}

bool Cpu::ArmUnconditional(uint32_t op)
{
    if ((op & 0x0e000000) == 0x0a000000) // BLX immediate
    {
        const int32_t offset = static_cast<int32_t>(op << 8) >> 6;
        state_.r[14] = cur_pc_ + 4;
        state_.r[15] = cur_pc_ + 8 + static_cast<uint32_t>(offset) + ((op >> 23) & 2);
        state_.thumb = true;
        return true;
    }

    if ((op & 0x0d700000) == 0x05500000) // PLD
    {
        return true;
    }

    if (op == 0xf57ff01f) // CLREX
    {
        exclusive_valid_ = false;
        return true;
    }

    if ((op & 0xfffffff0) == 0xf57ff040 || (op & 0xfffffff0) == 0xf57ff050 ||
        (op & 0xfffffff0) == 0xf57ff060) // DSB/DMB/ISB (ARMv7 encodings; harmless)
    {
        return true;
    }

    if ((op & 0xfff1fe20) == 0xf1000000) // CPS: no effect in user mode
    {
        return true;
    }

    if ((op & 0xfffffdff) == 0xf1010000) // SETEND
    {
        if (op & 0x200)
        {
            return Undefined(op); // big-endian data isn't supported
        }

        return true;
    }

    return Undefined(op);
}

bool Cpu::ArmDataProcessing(uint32_t op)
{
    const uint32_t opcode = (op >> 21) & 0xf;
    const bool s = (op >> 20) & 1;
    const uint32_t rn = (op >> 16) & 0xf;
    const uint32_t rd = (op >> 12) & 0xf;
    bool carry = state_.c;
    uint32_t b;
    uint32_t a;
    if (op & (1u << 25))
    {
        const uint32_t rot = ((op >> 8) & 0xf) * 2;
        b = std::rotr(op & 0xff, static_cast<int>(rot));
        if (rot)
        {
            carry = b >> 31;
        }
        a = Reg(rn);
    }
    else if (op & 0x10)
    {
        // Register-specified shift: PC reads as the instruction address + 12.
        const uint32_t rm = op & 0xf;
        const uint32_t rs = (op >> 8) & 0xf;
        const uint32_t vm = rm == 15 ? cur_pc_ + 12 : state_.r[rm];
        b = ShiftReg(vm, (op >> 5) & 3, state_.r[rs] & 0xff, carry);
        a = rn == 15 ? cur_pc_ + 12 : state_.r[rn];
    }
    else
    {
        b = ShiftImm(Reg(op & 0xf), (op >> 5) & 3, (op >> 7) & 0x1f, carry);
        a = Reg(rn);
    }

    if (s && rd == 15 && (opcode < 8 || opcode > 11))
    {
        return Undefined(op); // exception return (SUBS PC, LR etc.) is privileged
    }

    uint32_t result;
    bool logical = false;
    switch (opcode)
    {
    case 0x0: // AND
        result = a & b;
        logical = true;
        break;

    case 0x1: // EOR
        result = a ^ b;
        logical = true;
        break;

    case 0x2: // SUB
        result = AddWithCarry(a, ~b, true, s);
        break;

    case 0x3: // RSB
        result = AddWithCarry(~a, b, true, s);
        break;

    case 0x4: // ADD
        result = AddWithCarry(a, b, false, s);
        break;

    case 0x5: // ADC
        result = AddWithCarry(a, b, state_.c, s);
        break;

    case 0x6: // SBC
        result = AddWithCarry(a, ~b, state_.c, s);
        break;

    case 0x7: // RSC
        result = AddWithCarry(~a, b, state_.c, s);
        break;

    case 0x8: // TST
        SetNZ(a & b);
        state_.c = carry;
        return true;

    case 0x9: // TEQ
        SetNZ(a ^ b);
        state_.c = carry;
        return true;

    case 0xa: // CMP
        AddWithCarry(a, ~b, true, true);
        return true;

    case 0xb: // CMN
        AddWithCarry(a, b, false, true);
        return true;

    case 0xc: // ORR
        result = a | b;
        logical = true;
        break;

    case 0xd: // MOV
        result = b;
        logical = true;
        break;

    case 0xe: // BIC
        result = a & ~b;
        logical = true;
        break;

    default: // MVN
        result = ~b;
        logical = true;
        break;
    }

    if (s && logical)
    {
        SetNZ(result);
        state_.c = carry;
    }

    if (rd == 15)
    {
        BranchWritePc(result);
    }
    else
    {
        state_.r[rd] = result;
    }

    return true;
}

namespace
{

int32_t SignedSat32(int64_t value, bool& saturated)
{
    if (value > INT32_MAX)
    {
        saturated = true;
        return INT32_MAX;
    }

    if (value < INT32_MIN)
    {
        saturated = true;
        return INT32_MIN;
    }

    return static_cast<int32_t>(value);
}

int32_t Half(uint32_t value, bool top)
{
    return top ? static_cast<int16_t>(value >> 16) : static_cast<int16_t>(value);
}

} // namespace

bool Cpu::ArmMisc(uint32_t op)
{
    const uint32_t op1 = (op >> 21) & 3;
    const uint32_t rd = (op >> 12) & 0xf;
    const uint32_t rm = op & 0xf;
    if (op & 0x80)
    {
        // Signed 16-bit multiplies: op & 0x90 == 0x80 (bit 4 clear).
        const uint32_t rdh = (op >> 16) & 0xf; // Rd (or RdHi)
        const uint32_t ra = (op >> 12) & 0xf;  // Ra (or RdLo)
        const uint32_t rs = (op >> 8) & 0xf;
        const bool x = (op >> 5) & 1;
        const bool y = (op >> 6) & 1;
        switch (op1)
        {
        case 0: // SMLAxy
            {
                const int64_t sum = static_cast<int64_t>(Half(state_.r[rm], x)) * Half(state_.r[rs], y) +
                                    static_cast<int32_t>(state_.r[ra]);
                if (sum != static_cast<int32_t>(sum))
                {
                    state_.q = true;
                }

                state_.r[rdh] = static_cast<uint32_t>(sum);
                return true;
            }

        case 1: // SMLAWy / SMULWy
            {
                const int64_t product =
                    static_cast<int64_t>(static_cast<int32_t>(state_.r[rm])) * Half(state_.r[rs], y);
                const int32_t top = static_cast<int32_t>(product >> 16);
                if (!x)
                {
                    const int64_t sum = static_cast<int64_t>(top) + static_cast<int32_t>(state_.r[ra]);
                    if (sum != static_cast<int32_t>(sum))
                    {
                        state_.q = true;
                    }

                    state_.r[rdh] = static_cast<uint32_t>(sum);
                }
                else
                {
                    state_.r[rdh] = static_cast<uint32_t>(top);
                }

                return true;
            }

        case 2: // SMLALxy: a 64-bit accumulation that wraps, done in unsigned arithmetic
            {
                const uint64_t acc = (static_cast<uint64_t>(state_.r[rdh]) << 32) | state_.r[ra];
                const int64_t product = static_cast<int64_t>(Half(state_.r[rm], x)) * Half(state_.r[rs], y);
                const uint64_t sum = acc + static_cast<uint64_t>(product);
                state_.r[ra] = static_cast<uint32_t>(sum);
                state_.r[rdh] = static_cast<uint32_t>(sum >> 32);
                return true;
            }

        default: // SMULxy
            state_.r[rdh] = static_cast<uint32_t>(Half(state_.r[rm], x) * Half(state_.r[rs], y));
            return true;
        }
    }

    switch ((op >> 4) & 0xf)
    {
    case 0x0:
        if (op & (1u << 22))
        {
            return Undefined(op); // SPSR access is privileged
        }

        if (op1 == 0) // MRS
        {
            state_.r[rd] = state_.Cpsr();
            return true;
        }

        if (op1 == 1) // MSR CPSR, Rm
        {
            const uint32_t value = state_.r[rm];
            const uint32_t mask = (op >> 16) & 0xf;
            if (mask & 8)
            {
                state_.n = (value >> 31) & 1;
                state_.z = (value >> 30) & 1;
                state_.c = (value >> 29) & 1;
                state_.v = (value >> 28) & 1;
                state_.q = (value >> 27) & 1;
            }

            if (mask & 4)
            {
                state_.ge = static_cast<uint8_t>((value >> 16) & 0xf);
            }

            return true;
        }
        break;

    case 0x1:
        if (op1 == 1) // BX
        {
            BxWritePc(Reg(rm));
            return true;
        }

        if (op1 == 3) // CLZ
        {
            state_.r[rd] = static_cast<uint32_t>(std::countl_zero(state_.r[rm]));
            return true;
        }
        break;

    case 0x2:
        if (op1 == 1) // BXJ: no Jazelle, behaves as BX
        {
            BxWritePc(Reg(rm));
            return true;
        }
        break;

    case 0x3:
        if (op1 == 1) // BLX register
        {
            const uint32_t target = Reg(rm);
            state_.r[14] = cur_pc_ + 4;
            BxWritePc(target);
            return true;
        }
        break;

    case 0x5: // QADD, QSUB, QDADD, QDSUB
        {
            const uint32_t rn = (op >> 16) & 0xf;
            bool sat = false;
            int64_t n = static_cast<int32_t>(state_.r[rn]);
            if (op1 & 2)
            {
                n = SignedSat32(2 * n, sat);
            }

            const int64_t m = static_cast<int32_t>(state_.r[rm]);
            const int32_t result = SignedSat32((op1 & 1) ? m - n : m + n, sat);
            if (sat)
            {
                state_.q = true;
            }

            state_.r[rd] = static_cast<uint32_t>(result);
            return true;
        }

    default:
        break;
    }

    return Undefined(op);
}

bool Cpu::ArmMsrImmediate(uint32_t op)
{
    const uint32_t mask = (op >> 16) & 0xf;

    if (op & (1u << 22))
    {
        return Undefined(op);
    }

    if (mask == 0)
    {
        switch (op & 0xff)
        {
        case 0: // NOP
        case 4: // SEV
            return true;

        case 1: // YIELD
        case 2: // WFE
        case 3: // WFI
            reason_ = StopReason::kYield;
            return false;

        default:
            return true;
        }
    }

    const uint32_t rot = ((op >> 8) & 0xf) * 2;
    const uint32_t value = std::rotr(op & 0xff, static_cast<int>(rot));
    if (mask & 8)
    {
        state_.n = (value >> 31) & 1;
        state_.z = (value >> 30) & 1;
        state_.c = (value >> 29) & 1;
        state_.v = (value >> 28) & 1;
        state_.q = (value >> 27) & 1;
    }

    if (mask & 4)
    {
        state_.ge = static_cast<uint8_t>((value >> 16) & 0xf);
    }

    return true;
}

bool Cpu::ArmMultiply(uint32_t op)
{
    const bool s = (op >> 20) & 1;
    const uint32_t rdhi = (op >> 16) & 0xf;
    const uint32_t rdlo = (op >> 12) & 0xf;
    const uint32_t rs = (op >> 8) & 0xf;
    const uint32_t rm = op & 0xf;
    switch ((op >> 21) & 7)
    {
    case 0: // MUL
        {
            const uint32_t result = state_.r[rm] * state_.r[rs];
            state_.r[rdhi] = result;
            if (s)
            {
                SetNZ(result);
            }
            return true;
        }

    case 1: // MLA
        {
            const uint32_t result = state_.r[rm] * state_.r[rs] + state_.r[rdlo];
            state_.r[rdhi] = result;
            if (s)
            {
                SetNZ(result);
            }
            return true;
        }

    case 2: // UMAAL
        {
            if (s)
            {
                return Undefined(op);
            }

            const uint64_t result =
                static_cast<uint64_t>(state_.r[rm]) * state_.r[rs] + state_.r[rdhi] + state_.r[rdlo];
            state_.r[rdlo] = static_cast<uint32_t>(result);
            state_.r[rdhi] = static_cast<uint32_t>(result >> 32);
            return true;
        }

    case 3: // MLS (ARMv6T2)
        return Undefined(op);

    default:
        {
            const bool is_signed = (op >> 22) & 1;
            const bool accumulate = (op >> 21) & 1;
            uint64_t result;
            if (is_signed)
            {
                result = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(state_.r[rm])) *
                                               static_cast<int32_t>(state_.r[rs]));
            }
            else
            {
                result = static_cast<uint64_t>(state_.r[rm]) * state_.r[rs];
            }

            if (accumulate)
            {
                result += (static_cast<uint64_t>(state_.r[rdhi]) << 32) | state_.r[rdlo];
            }

            state_.r[rdlo] = static_cast<uint32_t>(result);
            state_.r[rdhi] = static_cast<uint32_t>(result >> 32);
            if (s)
            {
                state_.n = (result >> 63) != 0;
                state_.z = result == 0;
            }

            return true;
        }
    }
}

bool Cpu::ArmSwapExclusive(uint32_t op)
{
    const uint32_t rn = (op >> 16) & 0xf;
    const uint32_t rd = (op >> 12) & 0xf;
    const uint32_t rm = op & 0xf;
    const uint32_t addr = state_.r[rn];
    if (!(op & (1u << 23)))
    {
        if ((op & 0x00b00ff0) != 0x00000090)
        {
            return Undefined(op);
        }

        // SWP / SWPB
        if (op & (1u << 22))
        {
            const uint8_t old = mem_.Read8(addr);
            mem_.Write8(addr, static_cast<uint8_t>(state_.r[rm]));
            state_.r[rd] = old;
        }
        else
        {
            const uint32_t old = mem_.Read32(addr);
            mem_.Write32(addr, state_.r[rm]);
            state_.r[rd] = old;
        }

        return true;
    }

    const uint32_t type = (op >> 21) & 3; // 0 word, 1 doubleword, 2 byte, 3 halfword
    const bool load = (op >> 20) & 1;

    // LDREXD and STREXD transfer an even-numbered register and the one after it.
    const uint32_t rt = load ? rd : rm;
    if (type == 1 && (rt & 1))
    {
        return Undefined(op);
    }

    if (load)
    {
        // LDREX{B,H,D}
        exclusive_valid_ = true;
        exclusive_addr_ = addr;

        switch (type)
        {
        case 0:
            state_.r[rd] = mem_.Read32(addr);
            break;

        case 1:
            state_.r[rd] = mem_.Read32(addr);
            state_.r[rd + 1] = mem_.Read32(addr + 4);
            break;

        case 2:
            state_.r[rd] = mem_.Read8(addr);
            break;

        default:
            state_.r[rd] = mem_.Read16(addr);
            break;
        }

        return true;
    }

    // STREX{B,H,D}: rd receives the status (0 = stored). Every STREX clears the monitor, whether it stores or not.
    const bool stores = exclusive_valid_ && exclusive_addr_ == addr;
    exclusive_valid_ = false;
    if (!stores)
    {
        state_.r[rd] = 1;
        return true;
    }

    switch (type)
    {
    case 0:
        mem_.Write32(addr, state_.r[rm]);
        break;

    case 1:
        mem_.Write32(addr, state_.r[rm]);
        mem_.Write32(addr + 4, state_.r[rm + 1]);
        break;

    case 2:
        mem_.Write8(addr, static_cast<uint8_t>(state_.r[rm]));
        break;

    default:
        mem_.Write16(addr, static_cast<uint16_t>(state_.r[rm]));
        break;
    }

    state_.r[rd] = 0;

    return true;
}

bool Cpu::ArmExtraLoadStore(uint32_t op)
{
    const bool p = (op >> 24) & 1;
    const bool u = (op >> 23) & 1;
    const bool imm = (op >> 22) & 1;
    const bool w = (op >> 21) & 1;
    const bool l = (op >> 20) & 1;
    const uint32_t rn = (op >> 16) & 0xf;
    const uint32_t rt = (op >> 12) & 0xf;
    const uint32_t offset = imm ? (((op >> 4) & 0xf0) | (op & 0xf)) : Reg(op & 0xf);
    const uint32_t base = Reg(rn);
    const uint32_t offset_addr = u ? base + offset : base - offset;
    const uint32_t addr = p ? offset_addr : base;
    const bool wback = !p || w;
    switch ((op >> 5) & 3)
    {
    case 1: // LDRH / STRH
        if (l)
        {
            const uint32_t value = mem_.Read16(addr);
            if (wback)
            {
                state_.r[rn] = offset_addr;
            }
            state_.r[rt] = value;
        }
        else
        {
            mem_.Write16(addr, static_cast<uint16_t>(Reg(rt)));
            if (wback)
            {
                state_.r[rn] = offset_addr;
            }
        }

        return true;

    case 2: // LDRSB / LDRD
        if (l)
        {
            const uint32_t value = static_cast<uint32_t>(static_cast<int8_t>(mem_.Read8(addr)));
            if (wback)
            {
                state_.r[rn] = offset_addr;
            }
            state_.r[rt] = value;
        }
        else
        {
            if (rt & 1)
            {
                return Undefined(op);
            }

            const uint32_t lo = mem_.Read32(addr);
            const uint32_t hi = mem_.Read32(addr + 4);
            if (wback)
            {
                state_.r[rn] = offset_addr;
            }
            state_.r[rt] = lo;
            state_.r[rt + 1] = hi;
        }

        return true;

    default: // LDRSH / STRD
        if (l)
        {
            const uint32_t value = static_cast<uint32_t>(static_cast<int16_t>(mem_.Read16(addr)));
            if (wback)
            {
                state_.r[rn] = offset_addr;
            }
            state_.r[rt] = value;
        }
        else
        {
            if (rt & 1)
            {
                return Undefined(op);
            }

            mem_.Write32(addr, state_.r[rt]);
            mem_.Write32(addr + 4, Reg(rt + 1));
            if (wback)
            {
                state_.r[rn] = offset_addr;
            }
        }

        return true;
    }
}

bool Cpu::ArmLoadStore(uint32_t op)
{
    const bool reg = (op >> 25) & 1;
    const bool p = (op >> 24) & 1;
    const bool u = (op >> 23) & 1;
    const bool byte = (op >> 22) & 1;
    const bool w = (op >> 21) & 1;
    const bool l = (op >> 20) & 1;
    const uint32_t rn = (op >> 16) & 0xf;
    const uint32_t rt = (op >> 12) & 0xf;
    uint32_t offset;
    if (reg)
    {
        bool carry = state_.c;
        offset = ShiftImm(Reg(op & 0xf), (op >> 5) & 3, (op >> 7) & 0x1f, carry);
    }
    else
    {
        offset = op & 0xfff;
    }

    const uint32_t base = Reg(rn);
    const uint32_t offset_addr = u ? base + offset : base - offset;
    const uint32_t addr = p ? offset_addr : base;
    const bool wback = !p || w;
    if (l)
    {
        const uint32_t value = byte ? mem_.Read8(addr) : mem_.Read32(addr);
        if (wback)
        {
            state_.r[rn] = offset_addr;
        }

        if (rt == 15)
        {
            BxWritePc(value);
        }
        else
        {
            state_.r[rt] = value;
        }
    }
    else
    {
        const uint32_t value = Reg(rt);
        if (byte)
        {
            mem_.Write8(addr, static_cast<uint8_t>(value));
        }
        else
        {
            mem_.Write32(addr, value);
        }

        if (wback)
        {
            state_.r[rn] = offset_addr;
        }
    }

    return true;
}

bool Cpu::ArmBlockTransfer(uint32_t op)
{
    const bool p = (op >> 24) & 1;
    const bool u = (op >> 23) & 1;
    const bool w = (op >> 21) & 1;
    const bool l = (op >> 20) & 1;
    const uint32_t rn = (op >> 16) & 0xf;
    const uint32_t list = op & 0xffff;
    if ((op & (1u << 22)) || list == 0)
    {
        return Undefined(op); // user-bank transfers and exception returns are privileged
    }

    const uint32_t count = static_cast<uint32_t>(std::popcount(list));
    const uint32_t base = state_.r[rn];
    uint32_t addr;
    if (u)
    {
        addr = p ? base + 4 : base;
    }
    else
    {
        addr = p ? base - 4 * count : base - 4 * count + 4;
    }

    const uint32_t new_base = u ? base + 4 * count : base - 4 * count;
    if (l)
    {
        if (w)
        {
            state_.r[rn] = new_base;
        }

        for (uint32_t i = 0; i < 16; i++)
        {
            if (!(list & (1u << i)))
            {
                continue;
            }

            const uint32_t value = mem_.Read32(addr);
            addr += 4;
            if (i == 15)
            {
                BxWritePc(value);
            }
            else
            {
                state_.r[i] = value;
            }
        }
    }
    else
    {
        for (uint32_t i = 0; i < 16; i++)
        {
            if (!(list & (1u << i)))
            {
                continue;
            }

            mem_.Write32(addr, Reg(i));
            addr += 4;
        }

        if (w)
        {
            state_.r[rn] = new_base;
        }
    }

    return true;
}

bool Cpu::ArmCoprocessor(uint32_t op)
{
    const uint32_t cp = (op >> 8) & 0xf;
    const bool vfp = cp == 10 || cp == 11;
    if (((op >> 25) & 7) == 6)
    {
        if (!vfp)
        {
            return Undefined(op);
        }
        if ((op & 0x0fe00000) == 0x0c400000)
        {
            return VfpTransfer64(op);
        }

        return VfpLoadStore(op);
    }

    // Bits 27-24 are 1110: register transfers when bit 4 is set, data processing otherwise.
    if (op & 0x10)
    {
        if (vfp)
        {
            return VfpTransfer(op);
        }
        if (cp == 15)
        {
            return Cp15(op);
        }

        return Undefined(op);
    }

    if (vfp)
    {
        return Vfp(op);
    }

    return Undefined(op);
}

bool Cpu::Cp15(uint32_t op)
{
    const bool load = (op >> 20) & 1; // MRC
    const uint32_t opc1 = (op >> 21) & 7;
    const uint32_t crn = (op >> 16) & 0xf;
    const uint32_t rt = (op >> 12) & 0xf;
    const uint32_t opc2 = (op >> 5) & 7;
    const uint32_t crm = op & 0xf;

    if (opc1 != 0)
    {
        return Undefined(op);
    }

    if (crn == 13 && crm == 0)
    {
        if (opc2 == 3 && load)
        {
            state_.r[rt] = state_.tls;
            return true;
        }

        if (opc2 == 2)
        {
            if (load)
            {
                state_.r[rt] = state_.tpidrurw;
            }
            else
            {
                state_.tpidrurw = state_.r[rt];
            }
            return true;
        }
    }

    if (crn == 7 && !load)
    {
        // c7, c10, 4 (DSB), c7, c10, 5 (DMB), c7, c5, 4 (flush prefetch buffer)
        if ((crm == 10 && (opc2 == 4 || opc2 == 5)) || (crm == 5 && opc2 == 4))
        {
            return true;
        }
    }

    if (crn == 0 && crm == 0 && opc2 == 5 && load) // CPU ID register: the application core
    {
        state_.r[rt] = 0;
        return true;
    }

    return Undefined(op);
}

} // namespace threesf::arm
