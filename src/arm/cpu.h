// SPDX-License-Identifier: MIT

// ARM11 MPCore (ARMv6K) user-mode interpreter: ARM and Thumb-1 instruction sets and VFPv2.
//
// Written for 3SF, which runs a 3DS game's sound code (nw::snd / nn::snd, compiled with armcc) under a high-level
// emulation of the Horizon kernel. Only user-mode behaviour is modelled: there are no exceptions other than SVC and
// faults, which stop execution and are reported to the caller.

#pragma once

#include <array>
#include <bit>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "common/bytes.h"

namespace threesf::arm
{

constexpr uint32_t ByteSwap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}

// Flat 32-bit address space backed by host memory in 4 KiB pages.
class Memory
{
public:
    static constexpr uint32_t kPageBits = 12;
    static constexpr uint32_t kPageSize = 1u << kPageBits;
    static constexpr uint32_t kPageMask = kPageSize - 1;
    static constexpr uint32_t kPageCount = 1u << (32 - kPageBits);

    Memory();

    // Maps [vaddr, vaddr + size) to host memory. vaddr and size must be page aligned.
    void Map(uint32_t vaddr, uint8_t* host, uint32_t size);
    void Unmap(uint32_t vaddr, uint32_t size);

    // Calls `on_write` after CPU or kernel writes (WriteBlock, ZeroBlock) to [host, host + size), through any current
    // or future virtual mapping. host and size must be page aligned. Supports one watch at a time, with no overhead for
    // writes to other pages.
    void WatchWrites(const uint8_t* host, uint32_t size, std::function<void()> on_write);

    // Host pointer for a virtual address, or nullptr if unmapped.
    uint8_t* HostPtr(uint32_t vaddr) const
    {
        uint8_t* p = pages_[vaddr >> kPageBits];
        return p ? p + (vaddr & kPageMask) : nullptr;
    }

    uint8_t Read8(uint32_t a)
    {
        if (uint8_t* p = pages_[a >> kPageBits])
        {
            return p[a & kPageMask];
        }

        return static_cast<uint8_t>(Fault(a, false));
    }

    uint16_t Read16(uint32_t a)
    {
        uint8_t* p = pages_[a >> kPageBits];
        if (p && (a & kPageMask) <= kPageSize - 2)
        {
            return LoadLe16(p + (a & kPageMask));
        }

        return static_cast<uint16_t>(Read8(a) | (Read8(a + 1) << 8));
    }

    uint32_t Read32(uint32_t a)
    {
        uint8_t* p = pages_[a >> kPageBits];
        if (p && (a & kPageMask) <= kPageSize - 4)
        {
            return LoadLe32(p + (a & kPageMask));
        }

        return Read16(a) | (static_cast<uint32_t>(Read16(a + 2)) << 16);
    }

    void Write8(uint32_t a, uint8_t v)
    {
        if (uint8_t* p = write_pages_[a >> kPageBits])
        {
            p[a & kPageMask] = v;
        }
        else
        {
            WriteSlow8(a, v);
        }
    }

    void Write16(uint32_t a, uint16_t v)
    {
        uint8_t* p = write_pages_[a >> kPageBits];
        if (p && (a & kPageMask) <= kPageSize - 2)
        {
            StoreLe16(p + (a & kPageMask), v);
            return;
        }

        Write8(a, static_cast<uint8_t>(v));
        Write8(a + 1, static_cast<uint8_t>(v >> 8));
    }

    void Write32(uint32_t a, uint32_t v)
    {
        uint8_t* p = write_pages_[a >> kPageBits];
        if (p && (a & kPageMask) <= kPageSize - 4)
        {
            StoreLe32(p + (a & kPageMask), v);
            return;
        }

        Write16(a, static_cast<uint16_t>(v));
        Write16(a + 2, static_cast<uint16_t>(v >> 16));
    }

    void Write64(uint32_t a, uint64_t v)
    {
        Write32(a, static_cast<uint32_t>(v));
        Write32(a + 4, static_cast<uint32_t>(v >> 32));
    }

    // Copy between host buffers and virtual memory, for the kernel HLE. They stop at the first unmapped page, without
    // setting the fault flag.
    void ReadBlock(uint32_t vaddr, void* dst, std::size_t size) const;
    void WriteBlock(uint32_t vaddr, const void* src, std::size_t size);
    void ZeroBlock(uint32_t vaddr, std::size_t size);
    std::string ReadCString(uint32_t vaddr, std::size_t max) const;

    // Set by the first access to unmapped memory. Cpu::Run stops with StopReason::kFault, and the caller clears the
    // flag once it has handled the fault.
    bool fault_ = false;
    uint32_t fault_address_ = 0;
    bool fault_write_ = false;

private:
    // A write to a watched page (see WatchWrites) or an unmapped one.
    void WriteSlow8(uint32_t a, uint8_t v);

    bool Watched(const uint8_t* host) const
    {
        return host >= watch_start_ && host < watch_end_;
    }

    uint32_t Fault(uint32_t a, bool write)
    {
        if (!fault_)
        {
            fault_ = true;
            fault_address_ = a;
            fault_write_ = write;
        }

        return 0;
    }

    std::unique_ptr<uint8_t*[]> pages_;

    // The same pages for writing, except that watched pages are missing, so that their writes take the slow path.
    std::unique_ptr<uint8_t*[]> write_pages_;

    const uint8_t* watch_start_ = nullptr;
    const uint8_t* watch_end_ = nullptr;
    std::function<void()> on_watched_write_;
};

// Register state of one thread.
struct CpuState
{
    uint32_t Cpsr() const;

    std::array<uint32_t, 16> r{};

    // CPSR flags kept unpacked for speed.
    bool n = false, z = false, c = false, v = false, q = false;
    uint8_t ge = 0; // GE[3:0]
    bool thumb = false;
    std::array<uint32_t, 32> vfp{}; // S0..S31; D0..D15 alias the pairs (VFPv2 has 16 D registers)
    uint32_t fpscr = 0;
    uint32_t tls = 0;      // CP15 c13, c0, 3: user read-only thread ID register (the TLS pointer)
    uint32_t tpidrurw = 0; // CP15 c13, c0, 2: user read/write thread ID register
};

enum class StopReason
{
    kNone,
    kSvc,       // SVC executed; svc_number_ is set, PC points after the instruction
    kBudget,    // instruction budget exhausted
    kFault,     // memory fault (see Memory::fault_address_); PC is left at the instruction
    kUndefined, // undefined or unsupported instruction at stop_pc_
    kYield,     // YIELD, WFE or WFI hint: a good point to switch threads
};

// ARM interpreter. state_ holds the current thread's registers; Run executes until it reaches a StopReason.
class Cpu
{
public:
    explicit Cpu(Memory& memory) : mem_(memory)
    {
    }

    // Runs until an SVC, a fault, an undefined instruction, a yield hint, or the budget.
    StopReason Run(uint64_t max_instructions);

    // Clears the exclusive monitor (the kernel does this on every thread switch).
    void ClearExclusive()
    {
        exclusive_valid_ = false;
    }

    std::string Describe() const;

    CpuState state_;
    uint32_t svc_number_ = 0;
    uint32_t stop_pc_ = 0;     // address of the instruction that stopped execution
    uint32_t stop_opcode_ = 0; // the instruction, when Run stopped with kUndefined
    uint64_t executed_ = 0;    // instructions executed since construction

private:
    // Each returns false to stop execution, with reason_ set.
    bool StepArm(uint32_t op);
    bool StepThumb(uint32_t op);
    bool ArmUnconditional(uint32_t op);
    bool ArmDataProcessing(uint32_t op);
    bool ArmMisc(uint32_t op);
    bool ArmMultiply(uint32_t op);
    bool ArmSwapExclusive(uint32_t op);
    bool ArmExtraLoadStore(uint32_t op);
    bool ArmLoadStore(uint32_t op);
    bool ArmBlockTransfer(uint32_t op);
    bool ArmCoprocessor(uint32_t op);
    bool ArmMsrImmediate(uint32_t op);
    bool Media(uint32_t op);
    bool Vfp(uint32_t op);
    bool VfpLoadStore(uint32_t op);
    bool VfpTransfer(uint32_t op);
    bool VfpTransfer64(uint32_t op);
    bool Cp15(uint32_t op);
    bool Undefined(uint32_t op);
    bool Svc(uint32_t number);

    bool CheckCond(uint32_t cond) const
    {
        switch (cond)
        {
        case 0x0:
            return state_.z;
        case 0x1:
            return !state_.z;
        case 0x2:
            return state_.c;
        case 0x3:
            return !state_.c;
        case 0x4:
            return state_.n;
        case 0x5:
            return !state_.n;
        case 0x6:
            return state_.v;
        case 0x7:
            return !state_.v;
        case 0x8:
            return state_.c && !state_.z;
        case 0x9:
            return !state_.c || state_.z;
        case 0xa:
            return state_.n == state_.v;
        case 0xb:
            return state_.n != state_.v;
        case 0xc:
            return !state_.z && state_.n == state_.v;
        case 0xd:
            return state_.z || state_.n != state_.v;
        default:
            return true;
        }
    }

    static uint32_t ShiftImm(uint32_t value, uint32_t type, uint32_t amount, bool& carry);
    static uint32_t ShiftReg(uint32_t value, uint32_t type, uint32_t amount, bool& carry);

    // Writes PC with interworking: bit 0 selects Thumb (BX, BLX, LDR/LDM/POP to PC).
    void BxWritePc(uint32_t value)
    {
        state_.thumb = (value & 1) != 0;
        state_.r[15] = value & ~1u; // bit 1 set in ARM state is unpredictable; kept as is
    }

    // Writes PC without changing state, for data processing that writes PC (ARM, and Thumb ADD and MOV).
    void BranchWritePc(uint32_t value)
    {
        state_.r[15] = value & (state_.thumb ? ~1u : ~3u);
    }

    // Register read with PC = instruction address + 8 (ARM) or + 4 (Thumb).
    uint32_t Reg(uint32_t i) const
    {
        return i == 15 ? cur_pc_ + (state_.thumb ? 4 : 8) : state_.r[i];
    }

    void SetNZ(uint32_t v)
    {
        state_.n = (v >> 31) != 0;
        state_.z = v == 0;
    }

    uint32_t AddWithCarry(uint32_t a, uint32_t b, bool carry_in, bool set_flags)
    {
        const uint64_t unsigned_sum = static_cast<uint64_t>(a) + b + carry_in;
        const uint32_t result = static_cast<uint32_t>(unsigned_sum);
        if (set_flags)
        {
            SetNZ(result);
            state_.c = (unsigned_sum >> 32) != 0;
            state_.v = ((~(a ^ b) & (a ^ result)) >> 31) != 0;
        }

        return result;
    }

    // VFP registers. D0-D15 are the pairs S0/S1 to S30/S31.
    float S(uint32_t i) const
    {
        return std::bit_cast<float>(state_.vfp[i]);
    }

    // Writes an arithmetic result: applies flush-to-zero and default-NaN modes.
    void SetS(uint32_t i, float f);

    double D(uint32_t i) const
    {
        const uint64_t bits = state_.vfp[2 * i] | (static_cast<uint64_t>(state_.vfp[2 * i + 1]) << 32);
        return std::bit_cast<double>(bits);
    }

    void SetD(uint32_t i, double d);
    float FlushIn(float f) const;
    double FlushIn(double d) const;

    void SetDBits(uint32_t i, uint64_t bits)
    {
        state_.vfp[2 * i] = static_cast<uint32_t>(bits);
        state_.vfp[2 * i + 1] = static_cast<uint32_t>(bits >> 32);
    }

    uint64_t DBits(uint32_t i) const
    {
        return state_.vfp[2 * i] | (static_cast<uint64_t>(state_.vfp[2 * i + 1]) << 32);
    }

    void FpCompare(double a, double b);
    void UpdateHostFpu();

    Memory& mem_;
    uint32_t cur_pc_ = 0;
    StopReason reason_ = StopReason::kNone;
    bool exclusive_valid_ = false;
    uint32_t exclusive_addr_ = 0;
    uint32_t host_rmode_ = 0xffffffff; // the FPSCR.RMode bits the host rounds by during Run, or none
};

// Saves the host's floating-point environment and restores it when destroyed. In between, Apply sets the host rounding
// mode from the guest's FPSCR, so that host float arithmetic rounds the way VFP arithmetic does.
class HostFpuMode
{
public:
    HostFpuMode();
    HostFpuMode(const HostFpuMode&) = delete;
    HostFpuMode& operator=(const HostFpuMode&) = delete;
    ~HostFpuMode();

    // Sets the host rounding mode from FPSCR.RMode. Flush-to-zero and default NaN are applied in software, so this is
    // the only host state that has to change.
    static void Apply(uint32_t fpscr);

private:
    std::fenv_t saved_;
};

} // namespace threesf::arm
