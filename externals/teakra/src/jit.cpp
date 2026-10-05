#include "jit.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "bit.h"
#include "decoder.h"
#include "jit_x64_emitter.h"
#include "memory_interface.h"
#include "register.h"
#include "shared_memory.h"
#include "teakra/disassembler.h"

#if defined(__x86_64__) || defined(_M_X64)
#define TEAKRA_JIT_X64 1
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#else
#define TEAKRA_JIT_X64 0
#endif

namespace Teakra {

#if TEAKRA_JIT_X64

namespace {

using namespace X64;

constexpr u32 ProgramWords = 0x20000; // the program memory, where blocks can start
constexpr u32 NoLoopEnd = 0xFFFFFFFF; // as Interpreter::NoLoopEnd
constexpr u32 MaxBlockInstructions = 64;
constexpr u32 MaxVariants = 8; // blocks from one address, for other loops or modes
constexpr std::size_t CodeSize = 32 << 20;

// What a block's code reads and reports. Its layout is part of the generated code.
struct Context {
    RegisterState* regs;
    u8* memory;
    u64 limit;         // the most instructions this call may run
    u32 exit;          // why the block returned (Exit)
    u32 flags_pending; // in a block that loops, whether fz, fm, fn and fe are pending
    const MemoryInterfaceUnit* miu;
    u64 flag_value;    // the value pending flags come from (see Translator::SetAccFlag)
    void* interpreter; // for Jit::Handler (Translator::ViaHandler)
};

enum Exit : u32 {
    Done = 0,        // it ran its instructions, and regs.pc is the next one
    Bailed = 1,      // it stopped before an instruction the interpreter has to run
    GuardFailed = 2, // the mode registers aren't those it was translated for; it ran nothing
    Refused = 3,     // a data page is one the interpreter asserts for; it ran nothing
};

using BlockCode = u64 (*)(Context*);

// Executable memory. Code is written while the pages are writable and runs once they're
// executable again; they're never both.
class CodeMemory {
public:
    explicit CodeMemory(std::size_t size) : size(size) {
#ifdef _WIN32
        base =
            static_cast<u8*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        page = 4096;
#else
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base = p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
        page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
        // Some systems refuse executable memory, such as macOS for a hardened app without the
        // JIT entitlement. The JIT is off there.
        usable = base != nullptr && Protect(base, page, true) && Protect(base, page, false);
    }

    ~CodeMemory() {
        if (base) {
#ifdef _WIN32
            VirtualFree(base, 0, MEM_RELEASE);
#else
            munmap(base, size);
#endif
        }
    }

    bool Usable() const {
        return usable;
    }

    // Copies code in and returns where it starts, or nullptr if there's no room left.
    const u8* Add(const std::vector<u8>& code) {
        const std::size_t start = (used + 15) & ~std::size_t{15};
        if (!usable || start + code.size() > size) {
            return nullptr;
        }
        u8* const first = base + (start & ~(page - 1));
        const std::size_t length = base + start + code.size() - first;
        if (!Protect(first, length, false)) {
            return nullptr;
        }
        std::memcpy(base + start, code.data(), code.size());
        if (!Protect(first, length, true)) {
            usable = false;
            return nullptr;
        }
        used = start + code.size();
        return base + start;
    }

    void Clear() {
        used = 0;
    }

private:
    bool Protect(u8* at, std::size_t length, bool executable) {
#ifdef _WIN32
        DWORD old;
        if (!VirtualProtect(at, length, executable ? PAGE_EXECUTE_READ : PAGE_READWRITE, &old)) {
            return false;
        }
        if (executable) {
            FlushInstructionCache(GetCurrentProcess(), at, length);
        }
        return true;
#else
        return mprotect(at, length, executable ? PROT_READ | PROT_EXEC : PROT_READ | PROT_WRITE) ==
               0;
#endif
    }

    u8* base = nullptr;
    std::size_t size = 0;
    std::size_t page = 4096;
    std::size_t used = 0;
    bool usable = false;
};

struct Guard {
    const void* absolute; // for a field outside the registers; nullptr for a register field
    s32 offset;           // for a register field
    u8 size;
    u32 value;
};

struct Block {
    u32 start = 0;
    std::vector<Guard> guards; // with TEAKRA_JIT_STATS, to report which one fails
    u32 loop_end = NoLoopEnd;
    u32 length = 0;          // instructions in one pass; 0 if the first can't be translated
    std::vector<u16> source; // the program words it was translated from, from `start`
    u64 checked = 0;         // the generation in which `source` last matched the memory
    BlockCode code = nullptr;
    std::unique_ptr<Block> next; // another block from the same address
};

// Registers during a block. rbx, rbp and r12 to r15 keep their values; the rest are scratch.
constexpr Reg RegsBase = Reg::rbx;   // RegisterState*
constexpr Reg MemoryBase = Reg::rbp; // the DSP memory
constexpr Reg ContextReg = Reg::r12; // Context*
constexpr Reg Executed = Reg::r13;   // instructions run in earlier passes
constexpr Reg XPage = Reg::r14;      // the data memory of the X page (or the Z page), word 0
constexpr Reg YPage = Reg::r15;      // the data memory of the Y page (or the Z page), word 0

// Why an instruction can't be translated. A mode refusal depends on the mode registers, so a block
// that starts with that instruction still gets code, which checks the modes and stops at once.
enum class Refusal { None, Kind, Mode };

// The names of all instructions in the decode table, each of which the translator needs.
// clang-format off
#define TEAKRA_JIT_INSTRUCTIONS                                                                    \
    X(add) X(add_add) X(add_p1) X(add_sub) X(add_sub_i_mov_j) X(add_sub_j_mov_i) X(add_sub_sv)     \
    X(addhp) X(alb) X(alb_r6) X(alm) X(alm_r6) X(alu) X(and_) X(app) X(banke) X(bankr) X(bitrev)   \
    X(bitrev_dbrv) X(bitrev_ebrv) X(bkrep) X(bkrep_r6) X(bkreprst) X(bkreprst_memsp) X(bkrepsto)   \
    X(bkrepsto_memsp) X(br) X(break_) X(brr) X(call) X(calla) X(callr) X(cbs) X(clr) X(clrp)       \
    X(clrp0) X(clrp1) X(clrr) X(cmp) X(cmp_b0_b1) X(cmp_b1_b0) X(cmp_p1_to) X(cntx_r) X(cntx_s)    \
    X(dint) X(divs) X(eint) X(exchange_iaj) X(exchange_jai) X(exchange_riaj) X(exchange_rjai)      \
    X(exp) X(exp_r6) X(lim) X(load_modi) X(load_modj) X(load_movpd) X(load_page) X(load_ps)        \
    X(load_ps01) X(load_stepi) X(load_stepj) X(mac1) X(mac_x1to0) X(max2_vtr) X(max2_vtr_movh)     \
    X(max2_vtr_movij) X(max2_vtr_movji) X(max2_vtr_movl) X(max_ge) X(max_ge_r0) X(max_gt)          \
    X(max_gt_r0) X(min2_vtr) X(min2_vtr_movh) X(min2_vtr_movij) X(min2_vtr_movji) X(min2_vtr_movl) \
    X(min_le) X(min_le_r0) X(min_lt) X(min_lt_r0) X(mma) X(mma_mov) X(mma_mx_xy) X(mma_my_my)      \
    X(mma_xy_mx) X(moda3) X(moda4) X(modr) X(modr_d2) X(modr_d2_dmod) X(modr_ddmod) X(modr_demod)  \
    X(modr_dmod) X(modr_edmod) X(modr_eemod) X(modr_i2) X(modr_i2_dmod) X(mov) X(mov2)             \
    X(mov2_abh_m) X(mov2_ax_mij) X(mov2_ax_mji) X(mov2_axh_m_y0_m) X(mov2_mij_ax) X(mov2_mji_ax)   \
    X(mov2s) X(mov_a0h_stepi0) X(mov_a0h_stepj0) X(mov_dvm) X(mov_dvm_to) X(mov_eu) X(mov_ext0)    \
    X(mov_ext1) X(mov_ext2) X(mov_ext3) X(mov_icr) X(mov_icr_to) X(mov_memsp_r6) X(mov_memsp_to)   \
    X(mov_mixp) X(mov_mixp_r6) X(mov_mixp_to) X(mov_p0) X(mov_p0h_r6) X(mov_p0h_to) X(mov_p1_to)   \
    X(mov_pc) X(mov_prpage) X(mov_prpage_to) X(mov_r6) X(mov_r6_mixp) X(mov_r6_to) X(mov_repc)     \
    X(mov_repc_to) X(mov_stepi0) X(mov_stepi0_a0h) X(mov_stepj0) X(mov_stepj0_a0h) X(mov_sv)       \
    X(mov_sv_app) X(mov_sv_to) X(mov_x0) X(mov_x0_to) X(mov_x1) X(mov_x1_to) X(mov_y1)             \
    X(mov_y1_to) X(mova) X(movd) X(movp) X(movpdw) X(movr) X(movr_r6_to) X(movs) X(movs_r6_to)     \
    X(movsi) X(mpyi) X(msu) X(msusu) X(mul) X(mul_y0) X(mul_y0_r6) X(norm) X(nop) X(or_) X(pacr1)  \
    X(pop) X(pop_prpage) X(pop_r6) X(pop_repc) X(pop_x0) X(pop_x1) X(pop_y1) X(popa) X(push)       \
    X(push_prpage) X(push_r6) X(push_repc) X(push_x0) X(push_x1) X(push_y1) X(pusha) X(rep)        \
    X(rep_r6) X(ret) X(retd) X(reti) X(retic) X(retid) X(retidc) X(rets) X(shfc) X(shfi)           \
    X(sqr_mpysu_add3a) X(sqr_sqr_add3) X(sub) X(sub_add) X(sub_add_i_mov_j_sv)                     \
    X(sub_add_j_mov_i_sv) X(sub_add_sv) X(sub_p1) X(sub_sub) X(swap) X(trap) X(tst4b) X(tstb)      \
    X(tstb_r6) X(vtrclr) X(vtrclr0) X(vtrclr1) X(vtrmov) X(vtrmov0) X(vtrmov1) X(vtrshr)
// clang-format on

// A value in a host register, or a constant.
struct Operand40 {
    bool constant;
    Reg reg;
    u64 value;
};

// Translates instructions into x86-64 code for one block. The decoder calls one of its methods
// for each instruction, with the instruction's operands, as it calls the interpreter's; each
// either emits code with the same effect as the interpreter's method and returns true, or refuses
// and returns false. The methods mirror interpreter.h, which is the reference for their effect.
class Translator {
public:
    using instruction_return_type = bool;

    Translator(Emitter& e, const RegisterState& modes, const MemoryInterfaceUnit& miu,
               const MemoryInterfaceUnit* live_miu, u32 loop_end, Jit::Handler handler)
        : e(e), modes(modes), miu(miu), live_miu(live_miu), loop_end(loop_end), handler(handler) {}

    // Starts the instruction at `at`, whose successor is at `after`. `bail` is where its code
    // jumps to stop the block before it.
    void Begin(Label* bail_label, u32 at, u32 after, u16 op, u16 expansion, u32 position) {
        bail = bail_label;
        refusal = Refusal::None;
        address = at;
        next = after;
        opcode = op;
        expansion_word = expansion;
        index = position;
        sets_pc = false;
    }

    // Where the block's code returns (with rax free), for an instruction that failed in the
    // interpreter (ViaHandler).
    void SetEpilogue(Label* label) {
        epilogue = label;
    }

    // True if the instruction set regs.pc itself (a jump), which ends the block.
    bool SetsPc() const {
        return sets_pc;
    }

    Refusal GetRefusal() const {
        return refusal;
    }

    const std::vector<Guard>& Guards() const {
        return guards;
    }

    // Discards the guards recorded since `count` (an instruction that was refused).
    void TruncateGuards(std::size_t count) {
        guards.resize(count);
    }

    // The flags fz, fm, fn and fe of the last value SetAccFlag saw are only worked out when
    // something reads them or the block stops. Until then SetAccFlag only keeps the value
    // (Context::flag_value), and they're pending. At the top of a block inside a loop, which its
    // code may jump back to, they may be either: Context::flags_pending says.
    enum class Flags { Computed, Pending, Either };

    Flags FlagState() const {
        return flags;
    }
    void SetFlagState(Flags state) {
        flags = state;
    }

    // Works out pending flags into regs. Clobbers rax, rcx, rdx and r8.
    void ComputeFlags() {
        EmitComputeFlags(flags);
        flags = Flags::Computed;
    }

    // The code that works out flags in the given state (for the ways out of a block).
    void EmitComputeFlags(Flags state) {
        if (state == Flags::Computed) {
            return;
        }
        Label skip;
        if (state == Flags::Either) {
            e.OpImm(Arith::cmp, Ptr(ContextReg, offsetof(Context, flags_pending)), 0, 32);
            e.J(CC::e, skip);
        }
        // SetAccFlag in the interpreter.
        e.Mov(Reg::rax, Ptr(ContextReg, offsetof(Context, flag_value)), 64);
        // fz = value == 0
        e.Op(Arith::xor_, Reg::rdx, Reg::rdx, 32);
        e.Test(Reg::rax, Reg::rax, 64);
        e.Set(CC::e, Reg::rdx);
        e.Mov(R(modes.fz), Reg::rdx, 16);
        // fm = (value >> 39) != 0
        e.Mov(Reg::rcx, Reg::rax, 64);
        e.ShiftImm(Shift::shr, Reg::rcx, 39, 64);
        e.Set(CC::ne, Reg::rcx);
        e.Movzx(Reg::rcx, Reg::rcx, 8);
        e.Mov(R(modes.fm), Reg::rcx, 16);
        // fe = value != SignExtend<32>(value)
        e.Movsx(Reg::rcx, Reg::rax, 32, 64);
        e.Op(Arith::cmp, Reg::rcx, Reg::rax, 64);
        e.Set(CC::ne, Reg::rcx);
        e.Movzx(Reg::rcx, Reg::rcx, 8);
        e.Mov(R(modes.fe), Reg::rcx, 16);
        // fn = fz || (!fe && bit 31 != bit 30): bit 31 of value ^ (value << 1)
        e.Lea(Reg::r8, Ptr(Reg::rax, Reg::rax, 1, 0), 32);
        e.Op(Arith::xor_, Reg::r8, Reg::rax, 32);
        e.ShiftImm(Shift::shr, Reg::r8, 31, 32);
        e.OpImm(Arith::xor_, Reg::rcx, 1, 32);
        e.Op(Arith::and_, Reg::r8, Reg::rcx, 32);
        e.Op(Arith::or_, Reg::r8, Reg::rdx, 32);
        e.Mov(R(modes.fn), Reg::r8, 16);
        if (state == Flags::Either) {
            e.Bind(skip);
        }
    }

    bool undefined(u16) {
        return Refuse();
    }

    // Every instruction the translator doesn't know is refused.
#define X(name)                                                                                    \
    template <typename... Operands>                                                                \
    bool name(Operands...) {                                                                       \
        return Refuse();                                                                           \
    }
    TEAKRA_JIT_INSTRUCTIONS
#undef X

    // ------------------------------------------------------------------ Through the interpreter

    // Runs the instruction in the interpreter (Jit::Handler), for instructions whose only data
    // memory is the stack words at sp + each of `stack` (checked for MMIO first). With
    // `ends_block`, it may change a mode register, and the block ends after it.
    bool ViaHandler(bool ends_block, std::initializer_list<s32> stack = {}) {
        if (!handler || (ends_block && next == loop_end)) {
            return Refuse();
        }
        if (stack.size() != 0) {
            e.Movzx(Reg::rax, R(modes.sp), 16);
            for (s32 offset : stack) {
                e.Lea(Reg::rcx, Ptr(Reg::rax, offset), 32);
                e.Movzx(Reg::rcx, Reg::rcx, 16);
                CheckData(Reg::rcx);
            }
        }
        ComputeFlags(); // the handler reads and writes them in regs
#ifdef _WIN32
        e.Mov(Reg::rcx, Ptr(ContextReg, offsetof(Context, interpreter)), 64);
        e.MovImm(Reg::rdx, opcode);
        e.MovImm(Reg::r8, expansion_word);
#else
        e.Mov(Reg::rdi, Ptr(ContextReg, offsetof(Context, interpreter)), 64);
        e.MovImm(Reg::rsi, opcode);
        e.MovImm(Reg::rdx, expansion_word);
#endif
        e.MovImm(Reg::rax, static_cast<u64>(reinterpret_cast<std::uintptr_t>(handler)));
        e.CallReg(Reg::rax);
        // If it threw, the block stops before it, as the interpreter would, with whatever it
        // changed, and the flags as they are.
        Label completed;
        e.Test(Reg::rax, Reg::rax, 8);
        e.J(CC::ne, completed);
        e.OpImm(Arith::add, Executed, static_cast<s32>(index), 64);
        e.MovImm(R(modes.pc), static_cast<s32>(address), 32);
        e.MovImm(Ptr(ContextReg, offsetof(Context, exit)), static_cast<s32>(Exit::Bailed), 32);
        e.Jmp(*epilogue);
        e.Bind(completed);
        if (ends_block) {
            e.MovImm(R(modes.pc), static_cast<s32>(next), 32);
            sets_pc = true;
        }
        return true;
    }

    // Instructions on registers the translator doesn't handle (the mode registers among them) and
    // the stack.
    bool push(ArArpSttMod) {
        return ViaHandler(false, {-1});
    }
    bool pop(ArArpSttMod) {
        return ViaHandler(true, {0});
    }
    bool pusha(Ax) {
        return ViaHandler(false, {-1, -2});
    }
    bool pusha(Bx) {
        return ViaHandler(false, {-1, -2});
    }
    bool popa(Ab) {
        return ViaHandler(false, {0, 1});
    }
    bool push(Abe) {
        return ViaHandler(false, {-1});
    }
    bool push_r6() {
        return ViaHandler(false, {-1});
    }
    bool pop_r6() {
        return ViaHandler(false, {0});
    }
    bool mov(Imm16, ArArp) {
        return ViaHandler(true);
    }
    bool alb(Alb, Imm16, SttMod) {
        return ViaHandler(true);
    }
    bool load_stepi(Imm7s) {
        return ViaHandler(true);
    }
    bool load_stepj(Imm7s) {
        return ViaHandler(true);
    }
    bool mov_stepi0_a0h() {
        return ViaHandler(false);
    }
    bool mov_stepj0_a0h() {
        return ViaHandler(false);
    }
    bool mov_a0h_stepi0() {
        return ViaHandler(true);
    }
    bool mov_a0h_stepj0() {
        return ViaHandler(true);
    }

    // ------------------------------------------------------------------ Translated instructions

    bool nop() {
        return true;
    }

    // mov: register to register.
    bool mov(Register a, Register b) {
        if (a.GetName() == RegName::p) {
            // b loses its usual meaning here (see the interpreter)
            const RegName b_name = b.GetNameForMovFromP();
            ProductToBus40(Reg::rax, 0);
            SatAndSetAccAndFlag(b_name, Reg::rax);
            return true;
        }
        if (!CanReadBus16(a.GetName(), true) || !CanWriteBus16(b.GetName())) {
            return Refuse();
        }
        RegToBus16(Reg::rsi, a.GetName(), true);
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    // mov: register to [Rn].
    bool mov(Register a, Rn b, StepZIDS bs) {
        if (!CanReadBus16(a.GetName(), true)) {
            return Refuse();
        }
        const unsigned unit = b.Index();
        RegToBus16(Reg::rsi, a.GetName(), true); // before the step, which can change the register
        RnAndModify(Reg::rax, Reg::rcx, unit, bs.GetName(), false);
        CheckData(Reg::rax);
        StoreData(Reg::rax, Reg::rsi);
        e.Mov(R(modes.r[unit]), Reg::rcx, 16);
        return true;
    }

    // mov: [Rn] to register.
    bool mov(Rn a, StepZIDS as, Register b) {
        if (!CanWriteBus16(b.GetName())) {
            return Refuse();
        }
        const unsigned unit = a.Index();
        RnAndModify(Reg::rax, Reg::rcx, unit, as.GetName(), false);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        e.Mov(R(modes.r[unit]), Reg::rcx, 16); // before the destination, which may be the same
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    // mov: register to [page:imm8].
    bool mov(Ablh a, MemImm8 b) {
        if (!CanReadBus16(a.GetName(), true)) {
            return Refuse();
        }
        RegToBus16(Reg::rsi, a.GetName(), true);
        PageAddress(Reg::rax, b);
        CheckData(Reg::rax);
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    // mov: [page:imm8] to an accumulator.
    bool mov(MemImm8 a, Ab b) {
        PageAddress(Reg::rax, a);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    bool mov(Imm16 a, Register b) {
        if (!CanWriteBus16(b.GetName())) {
            return Refuse();
        }
        e.MovImm(Reg::rsi, a.Unsigned16());
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    // mova: an accumulator's low 32 bits to two words at [arrn], the low one at the offset address.
    bool mova(Ab a, ArRn2 b, ArStep2 bs) {
        const unsigned unit = Mode(modes.arrn[b.Index()]);
        const StepValue step = ArStep(Mode(modes.arstep[bs.Index()]));
        const OffsetValue offset = static_cast<OffsetValue>(Mode(modes.aroffset[bs.Index()]));
        if (!OffsetSupported(unit, offset, false)) {
            return RefuseMode();
        }
        GetAndSatAcc(Reg::rsi, a.GetName());
        RnAndModify(Reg::rax, Reg::rcx, unit, step, false);
        OffsetAddress(Reg::rdx, Reg::rax, unit, offset, false);
        CheckData(Reg::rdx);
        CheckData(Reg::rax);
        e.Mov(R(modes.r[unit]), Reg::rcx, 16);
        StoreData(Reg::rdx, Reg::rsi); // the low word first, as the interpreter writes them
        e.ShiftImm(Shift::shr, Reg::rsi, 16, 32);
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    // mova: two words at [arrn] to an accumulator.
    bool mova(ArRn2 a, ArStep2 as, Ab b) {
        const unsigned unit = Mode(modes.arrn[a.Index()]);
        const StepValue step = ArStep(Mode(modes.arstep[as.Index()]));
        const OffsetValue offset = static_cast<OffsetValue>(Mode(modes.aroffset[as.Index()]));
        if (!OffsetSupported(unit, offset, false)) {
            return RefuseMode();
        }
        RnAndModify(Reg::rax, Reg::rcx, unit, step, false);
        OffsetAddress(Reg::rdx, Reg::rax, unit, offset, false);
        CheckData(Reg::rdx);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rdx); // l
        LoadData(Reg::rdi, Reg::rax); // h
        e.Mov(R(modes.r[unit]), Reg::rcx, 16);
        e.ShiftImm(Shift::shl, Reg::rdi, 16, 32);
        e.Op(Arith::or_, Reg::rdi, Reg::rsi, 32);
        e.Movsx(Reg::rax, Reg::rdi, 32, 64); // SignExtend<32>
        SatAndSetAccAndFlag(b.GetName(), Reg::rax);
        return true;
    }

    bool alm(Alm op, MemImm8 a, Ax b) {
        if (!AlmSupported(op.GetName())) {
            return Refuse();
        }
        PageAddress(Reg::rax, a);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        ExtendOperandForAlm(Reg::rsi, op.GetName());
        AlmGeneric(op.GetName(), Reg::rsi, b.GetName());
        return true;
    }

    bool alm(Alm op, Rn a, StepZIDS as, Ax b) {
        if (!AlmSupported(op.GetName())) {
            return Refuse();
        }
        const unsigned unit = a.Index();
        RnAndModify(Reg::rax, Reg::rcx, unit, as.GetName(), false);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        e.Mov(R(modes.r[unit]), Reg::rcx, 16);
        ExtendOperandForAlm(Reg::rsi, op.GetName());
        AlmGeneric(op.GetName(), Reg::rsi, b.GetName());
        return true;
    }

    bool tstb(Rn a, StepZIDS as, Imm4 b) {
        const unsigned unit = a.Index();
        ComputeFlags(); // it sets fz
        RnAndModify(Reg::rax, Reg::rcx, unit, as.GetName(), false);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        e.Mov(R(modes.r[unit]), Reg::rcx, 16);
        // fz = (value >> b) & 1
        if (b.Unsigned16() != 0) {
            e.ShiftImm(Shift::shr, Reg::rsi, static_cast<u8>(b.Unsigned16()), 32);
        }
        e.OpImm(Arith::and_, Reg::rsi, 1, 32);
        e.Mov(R(modes.fz), Reg::rsi, 16);
        return true;
    }

    bool alm(Alm op, Register a, Ax b) {
        const AlmOp name = op.GetName();
        if (!AlmSupported(name)) {
            return Refuse();
        }
        const RegName source = a.GetName();
        switch (source) {
        case RegName::p:
        case RegName::a0:
        case RegName::a1: {
            // A 40-bit operand, which only some operations take (the interpreter throws for the
            // others).
            switch (name) {
            case AlmOp::Or:
            case AlmOp::And:
            case AlmOp::Xor:
            case AlmOp::Add:
            case AlmOp::Cmp:
            case AlmOp::Sub:
                break;
            default:
                return Refuse();
            }
            if (source == RegName::p) {
                ProductToBus40(Reg::rsi, 0);
            } else {
                LoadAcc(Reg::rsi, source);
            }
            AlmGeneric(name, Reg::rsi, b.GetName());
            return true;
        }
        default:
            break;
        }
        if (!CanReadBus16(source, false)) {
            return Refuse();
        }
        RegToBus16(Reg::rsi, source, false);
        ExtendOperandForAlm(Reg::rsi, name);
        AlmGeneric(name, Reg::rsi, b.GetName());
        return true;
    }

    bool shfi(Ab a, Ab b, Imm6s s) {
        LoadAcc(Reg::rax, a.GetName());
        ShiftBus40Constant(Reg::rax, s.Signed16(), b.GetName());
        return true;
    }

    bool moda4(Moda4 op, Ax a, Cond cond) {
        return Moda(op.GetName(), a.GetName(), cond);
    }

    bool moda3(Moda3 op, Bx a, Cond cond) {
        return Moda(op.GetName(), a.GetName(), cond);
    }

    bool add(Ab a, Bx b) {
        LoadAcc(Reg::rsi, a.GetName());
        LoadAcc(Reg::rax, b.GetName());
        AddSub(Reg::rax, Reg::rsi, false);
        SatAndSetAccAndFlag(b.GetName(), Reg::rax);
        return true;
    }

    bool add(Bx a, Ax b) {
        LoadAcc(Reg::rsi, a.GetName());
        LoadAcc(Reg::rax, b.GetName());
        AddSub(Reg::rax, Reg::rsi, false);
        SatAndSetAccAndFlag(b.GetName(), Reg::rax);
        return true;
    }

    bool and_(Ab a, Ab b, Ax c) {
        LoadAcc(Reg::rax, a.GetName());
        LoadAcc(Reg::rsi, b.GetName());
        e.Op(Arith::and_, Reg::rax, Reg::rsi, 64);
        SetAccAndFlag(c.GetName(), Reg::rax);
        return true;
    }

    bool or_(Ab a, Ax b, Ax c) {
        return Or(a.GetName(), b.GetName(), c.GetName());
    }
    bool or_(Ax a, Bx b, Ax c) {
        return Or(a.GetName(), b.GetName(), c.GetName());
    }
    bool or_(Bx a, Bx b, Ax c) {
        return Or(a.GetName(), b.GetName(), c.GetName());
    }

    bool clrp0() {
        ClearProduct(0);
        return true;
    }
    bool clrp1() {
        ClearProduct(1);
        return true;
    }
    bool clrp() {
        ClearProduct(0);
        ClearProduct(1);
        return true;
    }

    bool movs(Register a, Ab b) {
        if (!CanReadBus16(a.GetName(), false)) {
            return Refuse();
        }
        RegToBus16(Reg::rax, a.GetName(), false);
        e.Movsx(Reg::rax, Reg::rax, 16, 64);
        ShiftBus40Variable(Reg::rax, b.GetName());
        return true;
    }

    bool movs(Rn a, StepZIDS as, Ab b) {
        const unsigned unit = a.Index();
        RnAndModify(Reg::rsi, Reg::rcx, unit, as.GetName(), false);
        CheckData(Reg::rsi);
        LoadData(Reg::rax, Reg::rsi);
        e.Mov(R(modes.r[unit]), Reg::rcx, 16);
        e.Movsx(Reg::rax, Reg::rax, 16, 64);
        ShiftBus40Variable(Reg::rax, b.GetName());
        return true;
    }

    bool movs(MemImm8 a, Ab b) {
        PageAddress(Reg::rsi, a);
        CheckData(Reg::rsi);
        LoadData(Reg::rax, Reg::rsi);
        e.Movsx(Reg::rax, Reg::rax, 16, 64);
        ShiftBus40Variable(Reg::rax, b.GetName());
        return true;
    }

    template <typename ArpRnX, typename ArpStepX>
    bool mma(ArpRnX xy, ArpStepX i, ArpStepX j, bool dmodi, bool dmodj, RegName a, bool x0_sign,
             bool y0_sign, bool x1_sign, bool y1_sign, SumBase base, bool sub_p0, bool p0_align,
             bool sub_p1, bool p1_align) {
        const unsigned ui = Mode(modes.arprni[xy.Index()]);
        const unsigned uj = Mode(modes.arprnj[xy.Index()]) + 4u;
        const StepValue si = ArStep(Mode(modes.arpstepi[i.Index()]));
        const StepValue sj = ArStep(Mode(modes.arpstepj[j.Index()]));
        const OffsetValue oi = static_cast<OffsetValue>(Mode(modes.arpoffseti[i.Index()]));
        const OffsetValue oj = static_cast<OffsetValue>(Mode(modes.arpoffsetj[j.Index()]));
        if (!OffsetSupported(ui, oi, dmodi) || !OffsetSupported(uj, oj, dmodj)) {
            return RefuseMode();
        }
        // The four addresses: x and y at the two units, and the offset one after each.
        RnAndModify(Reg::r8, Reg::r10, ui, si, dmodi);
        RnAndModify(Reg::r9, Reg::r11, uj, sj, dmodj);
        OffsetAddress(Reg::rsi, Reg::r8, ui, oi, dmodi);
        OffsetAddress(Reg::rdi, Reg::r9, uj, oj, dmodj);
        CheckData(Reg::r8);
        CheckData(Reg::r9);
        CheckData(Reg::rsi);
        CheckData(Reg::rdi);
        // Nothing can stop the instruction from here on. The interpreter sums the products before
        // it reads the factors and steps the unit registers, but the sum reads neither, so the
        // factors and the unit registers can change first, and nothing needs keeping across it.
        LoadData(Reg::rax, Reg::r8);
        e.Mov(R(modes.x[0]), Reg::rax, 16);
        LoadData(Reg::rax, Reg::r9);
        e.Mov(R(modes.y[0]), Reg::rax, 16);
        LoadData(Reg::rax, Reg::rsi);
        e.Mov(R(modes.x[1]), Reg::rax, 16);
        LoadData(Reg::rax, Reg::rdi);
        e.Mov(R(modes.y[1]), Reg::rax, 16);
        e.Mov(R(modes.r[ui]), Reg::r10, 16);
        e.Mov(R(modes.r[uj]), Reg::r11, 16);
        ProductSum(base, a, sub_p0, p0_align, sub_p1, p1_align);
        DoMultiplication(0, x0_sign, y0_sign);
        DoMultiplication(1, x1_sign, y1_sign);
        return true;
    }

    bool mma(RegName a, bool x0_sign, bool y0_sign, bool x1_sign, bool y1_sign, SumBase base,
             bool sub_p0, bool p0_align, bool sub_p1, bool p1_align) {
        ProductSum(base, a, sub_p0, p0_align, sub_p1, p1_align);
        // std::swap(regs.x[0], regs.x[1])
        e.Movzx(Reg::rax, R(modes.x[0]), 16);
        e.Movzx(Reg::rcx, R(modes.x[1]), 16);
        e.Mov(R(modes.x[0]), Reg::rcx, 16);
        e.Mov(R(modes.x[1]), Reg::rax, 16);
        DoMultiplication(0, x0_sign, y0_sign);
        DoMultiplication(1, x1_sign, y1_sign);
        return true;
    }

    bool push(Register a) {
        if (!CanReadBus16(a.GetName(), true)) {
            return ViaHandler(false, {-1});
        }
        RegToBus16(Reg::rsi, a.GetName(), true);
        e.Movzx(Reg::rax, R(modes.sp), 16);
        e.OpImm(Arith::sub, Reg::rax, 1, 32);
        e.Movzx(Reg::rax, Reg::rax, 16);
        CheckData(Reg::rax);
        e.Mov(R(modes.sp), Reg::rax, 16);
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    bool pop(Register a) {
        if (!CanWriteBus16(a.GetName())) {
            return ViaHandler(true, {0});
        }
        e.Movzx(Reg::rax, R(modes.sp), 16);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        e.OpImm(Arith::add, Reg::rax, 1, 32);
        e.Mov(R(modes.sp), Reg::rax, 16); // before the destination, which may be sp
        RegFromBus16(a.GetName(), Reg::rsi);
        return true;
    }

    bool push(Px a) {
        ProductToBus40(Reg::rsi, a.Index());
        e.Movzx(Reg::rax, R(modes.sp), 16);
        e.OpImm(Arith::sub, Reg::rax, 1, 32);
        e.Movzx(Reg::rax, Reg::rax, 16); // the low word's address
        e.Lea(Reg::rcx, Ptr(Reg::rax, -1), 32);
        e.Movzx(Reg::rcx, Reg::rcx, 16); // the high word's address
        CheckData(Reg::rax);
        CheckData(Reg::rcx);
        e.Mov(R(modes.sp), Reg::rcx, 16);
        StoreData(Reg::rax, Reg::rsi);
        e.ShiftImm(Shift::shr, Reg::rsi, 16, 32);
        StoreData(Reg::rcx, Reg::rsi);
        return true;
    }

    bool pop(Px a) {
        e.Movzx(Reg::rax, R(modes.sp), 16); // the high word's address
        e.Lea(Reg::rcx, Ptr(Reg::rax, 1), 32);
        e.Movzx(Reg::rcx, Reg::rcx, 16); // the low word's address
        CheckData(Reg::rax);
        CheckData(Reg::rcx);
        LoadData(Reg::rsi, Reg::rax);
        LoadData(Reg::rdi, Reg::rcx);
        e.Lea(Reg::rcx, Ptr(Reg::rcx, 1), 32);
        e.Mov(R(modes.sp), Reg::rcx, 16);
        e.ShiftImm(Shift::shl, Reg::rsi, 16, 32);
        e.Op(Arith::or_, Reg::rsi, Reg::rdi, 32);
        ProductFromBus32(a.Index(), Reg::rsi);
        return true;
    }

    // mov2_abh_m: the high halves of two accumulators (saturated, without flm) to two words at
    // [arrn], ay's at the offset address.
    bool mov2_abh_m(Abh ax, Abh ay, ArRn1 b, ArStep1 bs) {
        const unsigned unit = Mode(modes.arrn[b.Index()]);
        const StepValue step = ArStep(Mode(modes.arstep[bs.Index()]));
        const OffsetValue offset = static_cast<OffsetValue>(Mode(modes.aroffset[bs.Index()]));
        if (!OffsetSupported(unit, offset, false)) {
            return RefuseMode();
        }
        GetAndSatAccNoFlag(Reg::rsi, ax.GetName());
        GetAndSatAccNoFlag(Reg::rdi, ay.GetName());
        e.ShiftImm(Shift::shr, Reg::rsi, 16, 64);
        e.ShiftImm(Shift::shr, Reg::rdi, 16, 64);
        RnAndModify(Reg::rax, Reg::rcx, unit, step, false);
        OffsetAddress(Reg::r8, Reg::rax, unit, offset, false);
        CheckData(Reg::r8);
        CheckData(Reg::rax);
        e.Mov(R(modes.r[unit]), Reg::rcx, 16);
        StoreData(Reg::r8, Reg::rdi); // v first, as the interpreter writes them
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    // mov2: a product (unshifted) to two words at [arrn], the low one at the offset address.
    bool mov2(Px a, ArRn2 b, ArStep2 bs) {
        const unsigned unit = Mode(modes.arrn[b.Index()]);
        const StepValue step = ArStep(Mode(modes.arstep[bs.Index()]));
        const OffsetValue offset = static_cast<OffsetValue>(Mode(modes.aroffset[bs.Index()]));
        if (!OffsetSupported(unit, offset, false)) {
            return RefuseMode();
        }
        e.Mov(Reg::rsi, R(modes.p[a.Index()]), 32);
        RnAndModify(Reg::rax, Reg::rcx, unit, step, false);
        OffsetAddress(Reg::r8, Reg::rax, unit, offset, false);
        CheckData(Reg::r8);
        CheckData(Reg::rax);
        e.Mov(R(modes.r[unit]), Reg::rcx, 16);
        StoreData(Reg::r8, Reg::rsi);
        e.ShiftImm(Shift::shr, Reg::rsi, 16, 32);
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    // mov2: two words at [arrn] to a product.
    bool mov2(ArRn2 a, ArStep2 as, Px b) {
        const unsigned unit = Mode(modes.arrn[a.Index()]);
        const StepValue step = ArStep(Mode(modes.arstep[as.Index()]));
        const OffsetValue offset = static_cast<OffsetValue>(Mode(modes.aroffset[as.Index()]));
        if (!OffsetSupported(unit, offset, false)) {
            return RefuseMode();
        }
        RnAndModify(Reg::rax, Reg::rcx, unit, step, false);
        OffsetAddress(Reg::r8, Reg::rax, unit, offset, false);
        CheckData(Reg::r8);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::r8);  // l
        LoadData(Reg::rdi, Reg::rax); // h
        e.Mov(R(modes.r[unit]), Reg::rcx, 16);
        e.ShiftImm(Shift::shl, Reg::rdi, 16, 32);
        e.Op(Arith::or_, Reg::rdi, Reg::rsi, 32);
        ProductFromBus32(b.Index(), Reg::rdi);
        return true;
    }

    bool mov(MemImm16 a, Ax b) {
        e.MovImm(Reg::rax, a.Unsigned16());
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    bool mov(MemR7Imm7s a, Ax b) {
        R7Address(Reg::rax, a.Signed16());
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    bool mov(MemImm8 a, Ablh b) {
        PageAddress(Reg::rax, a);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    bool mov(Ab a, Ab b) {
        LoadAcc(Reg::rax, a.GetName());
        SatAndSetAccAndFlag(b.GetName(), Reg::rax);
        return true;
    }

    bool mov(Register a, Bx b) {
        if (a.GetName() == RegName::p) {
            ProductToBus40(Reg::rax, 0);
            SatAndSetAccAndFlag(b.GetName(), Reg::rax);
            return true;
        }
        if (a.GetName() == RegName::a0 || a.GetName() == RegName::a1) {
            LoadAcc(Reg::rax, a.GetName());
            SatAndSetAccAndFlag(b.GetName(), Reg::rax);
            return true;
        }
        if (!CanReadBus16(a.GetName(), true)) {
            return Refuse();
        }
        RegToBus16(Reg::rsi, a.GetName(), true);
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    bool mov_y1(Abl a) {
        RegToBus16(Reg::rsi, a.GetName(), true);
        e.Mov(R(modes.y[1]), Reg::rsi, 16);
        return true;
    }

    bool mov_r6(Imm16 a) {
        e.MovImm(R(modes.r[6]), static_cast<s16>(a.Unsigned16()), 16);
        return true;
    }

    bool alu(Alu op, MemImm16 a, Ax b) {
        if (!AluSupported(op.GetName())) {
            return Refuse();
        }
        e.MovImm(Reg::rax, a.Unsigned16());
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        ExtendOperandForAlm(Reg::rsi, op.GetName());
        AlmGeneric(op.GetName(), Reg::rsi, b.GetName());
        return true;
    }

    bool alu(Alu op, MemR7Imm16 a, Ax b) {
        if (!AluSupported(op.GetName())) {
            return Refuse();
        }
        R7Address(Reg::rax, a.Unsigned16());
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        ExtendOperandForAlm(Reg::rsi, op.GetName());
        AlmGeneric(op.GetName(), Reg::rsi, b.GetName());
        return true;
    }

    bool alu(Alu op, MemR7Imm7s a, Ax b) {
        if (!AluSupported(op.GetName())) {
            return Refuse();
        }
        R7Address(Reg::rax, a.Signed16());
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        ExtendOperandForAlm(Reg::rsi, op.GetName());
        AlmGeneric(op.GetName(), Reg::rsi, b.GetName());
        return true;
    }

    bool alu(Alu op, Imm16 a, Ax b) {
        if (!AluSupported(op.GetName())) {
            return Refuse();
        }
        e.MovImm(Reg::rsi, a.Unsigned16());
        ExtendOperandForAlm(Reg::rsi, op.GetName());
        AlmGeneric(op.GetName(), Reg::rsi, b.GetName());
        return true;
    }

    bool alu(Alu op, Imm8 a, Ax b) {
        const AlmOp name = op.GetName();
        if (!AluSupported(name)) {
            return Refuse();
        }
        if (name == AlmOp::And) {
            // Bits 8 to 15 of the accumulator stay, though the flags are set as if they changed.
            e.Mov(Reg::rdi, Acc(b.GetName()), 64);
            e.OpImm(Arith::and_, Reg::rdi, 0xFF00, 32);
        }
        e.MovImm(Reg::rsi, a.Unsigned16());
        ExtendOperandForAlm(Reg::rsi, name);
        e.Push(Reg::rdi);
        AlmGeneric(name, Reg::rsi, b.GetName());
        e.Pop(Reg::rdi);
        if (name == AlmOp::And) {
            e.Mov(Reg::rax, Acc(b.GetName()), 64);
            e.MovImm(Reg::rcx, 0xFFFF'FFFF'FFFF'00FF);
            e.Op(Arith::and_, Reg::rax, Reg::rcx, 64);
            e.Op(Arith::or_, Reg::rax, Reg::rdi, 64);
            StoreAcc(b.GetName(), Reg::rax);
        }
        return true;
    }

    bool mul_y0(Mul3 op, Register x, Ax a) {
        if (!CanReadBus16(x.GetName(), false)) {
            return Refuse();
        }
        RegToBus16(Reg::rsi, x.GetName(), false);
        e.Mov(R(modes.x[0]), Reg::rsi, 16);
        MulGeneric(op.GetName(), a.GetName());
        return true;
    }

    bool mul_y0(Mul3 op, Rn x, StepZIDS xs, Ax a) {
        const unsigned unit = x.Index();
        RnAndModify(Reg::rax, Reg::rcx, unit, xs.GetName(), false);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        e.Mov(R(modes.r[unit]), Reg::rcx, 16);
        e.Mov(R(modes.x[0]), Reg::rsi, 16);
        MulGeneric(op.GetName(), a.GetName());
        return true;
    }

    bool mul_y0(Mul2 op, MemImm8 x, Ax a) {
        PageAddress(Reg::rax, x);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        e.Mov(R(modes.x[0]), Reg::rsi, 16);
        MulGeneric(op.GetName(), a.GetName());
        return true;
    }

    bool app(Ab c, SumBase base, bool sub_p0, bool p0_align, bool sub_p1, bool p1_align) {
        ProductSum(base, c.GetName(), sub_p0, p0_align, sub_p1, p1_align);
        return true;
    }

    bool modr(Rn a, StepZIDS as) {
        return Modr(a.Index(), as.GetName(), false);
    }
    bool modr_dmod(Rn a, StepZIDS as) {
        return Modr(a.Index(), as.GetName(), true);
    }
    bool modr_i2(Rn a) {
        return Modr(a.Index(), StepValue::Increase2Mode1, false);
    }
    bool modr_i2_dmod(Rn a) {
        return Modr(a.Index(), StepValue::Increase2Mode1, true);
    }
    bool modr_d2(Rn a) {
        return Modr(a.Index(), StepValue::Decrease2Mode1, false);
    }
    bool modr_d2_dmod(Rn a) {
        return Modr(a.Index(), StepValue::Decrease2Mode1, true);
    }
    bool modr_eemod(ArpRn2 a, ArpStep2 asi, ArpStep2 asj) {
        return ModrPair(a, asi, asj, false, false);
    }
    bool modr_edmod(ArpRn2 a, ArpStep2 asi, ArpStep2 asj) {
        return ModrPair(a, asi, asj, false, true);
    }
    bool modr_demod(ArpRn2 a, ArpStep2 asi, ArpStep2 asj) {
        return ModrPair(a, asi, asj, true, false);
    }
    bool modr_ddmod(ArpRn2 a, ArpStep2 asi, ArpStep2 asj) {
        return ModrPair(a, asi, asj, true, true);
    }

    bool Modr(unsigned unit, StepValue step, bool dmod) {
        RnAndModifyRaw(Reg::rax, Reg::rcx, unit, step, dmod);
        e.Mov(R(modes.r[unit]), Reg::rcx, 16);
        // fr = r[unit] == 0, after the step
        e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
        e.Test(Reg::rcx, Reg::rcx, 32);
        e.Set(CC::e, Reg::rax);
        e.Mov(R(modes.fr), Reg::rax, 16);
        return true;
    }

    bool ModrPair(ArpRn2 a, ArpStep2 asi, ArpStep2 asj, bool dmodi, bool dmodj) {
        const unsigned ui = Mode(modes.arprni[a.Index()]);
        const unsigned uj = Mode(modes.arprnj[a.Index()]) + 4u;
        const StepValue si = ArStep(Mode(modes.arpstepi[asi.Index()]));
        const StepValue sj = ArStep(Mode(modes.arpstepj[asj.Index()]));
        RnAndModifyRaw(Reg::rax, Reg::rcx, ui, si, dmodi);
        e.Mov(R(modes.r[ui]), Reg::rcx, 16);
        RnAndModifyRaw(Reg::rax, Reg::rcx, uj, sj, dmodj);
        e.Mov(R(modes.r[uj]), Reg::rcx, 16);
        return true;
    }

    bool shfc(Ab a, Ab b, Cond cond) {
        Label skip;
        if (!JumpUnlessCondition(cond, skip)) {
            return Refuse();
        }
        LoadAcc(Reg::rax, a.GetName());
        ShiftBus40Variable(Reg::rax, b.GetName());
        EndConditional(skip);
        return true;
    }

    // Where the ways past a condition (JumpUnlessCondition) join: the flags an operation left
    // pending are worked out on its way, as the other way has them computed.
    void EndConditional(Label& skip) {
        if (skip.Used()) {
            ComputeFlags();
        }
        e.Bind(skip);
    }

    bool alb(Alb op, Imm16 a, Register b) {
        ComputeFlags(); // it sets fz and fm
        const RegName name = b.GetName();
        if (name == RegName::a0 || name == RegName::a1) {
            return Refuse(); // the interpreter throws
        }
        if (name != RegName::p && !CanReadBus16(name, false)) {
            return ViaHandler(true);
        }
        if (name == RegName::p) {
            ProductToBus40(Reg::rsi, 0);
            e.ShiftImm(Shift::shr, Reg::rsi, 16, 64);
            e.Movzx(Reg::rsi, Reg::rsi, 16);
        } else {
            RegToBus16(Reg::rsi, name, false);
        }
        GenericAlb(op.GetName(), a.Unsigned16(), Reg::rsi);
        if (!IsAlbModifying(op.GetName())) {
            return true;
        }
        // rsi = result
        switch (name) {
        case RegName::a0l:
        case RegName::a1l:
        case RegName::b0l:
        case RegName::b1l:
        case RegName::a0h:
        case RegName::a1h:
        case RegName::b0h:
        case RegName::b1h: {
            // Straight into the accumulator, without flags or saturation.
            const bool high = name == RegName::a0h || name == RegName::a1h ||
                              name == RegName::b0h || name == RegName::b1h;
            e.Mov(Reg::rax, Acc(name), 64);
            e.MovImm(Reg::rcx, high ? 0xFFFF'FFFF'0000'FFFF : 0xFFFF'FFFF'FFFF'0000);
            e.Op(Arith::and_, Reg::rax, Reg::rcx, 64);
            if (high) {
                e.ShiftImm(Shift::shl, Reg::rsi, 16, 64);
            }
            e.Op(Arith::or_, Reg::rax, Reg::rsi, 64);
            StoreAcc(name, Reg::rax);
            return true;
        }
        default:
            RegFromBus16(name, Reg::rsi);
            return true;
        }
    }

    bool alb(Alb op, Imm16 a, MemImm8 b) {
        ComputeFlags(); // it sets fz and fm
        PageAddress(Reg::rdi, b);
        CheckData(Reg::rdi);
        LoadData(Reg::rsi, Reg::rdi);
        GenericAlb(op.GetName(), a.Unsigned16(), Reg::rsi);
        if (IsAlbModifying(op.GetName())) {
            StoreData(Reg::rdi, Reg::rsi);
        }
        return true;
    }

    bool alb(Alb op, Imm16 a, Rn b, StepZIDS bs) {
        ComputeFlags(); // it sets fz and fm
        const unsigned unit = b.Index();
        RnAndModify(Reg::rdi, Reg::r8, unit, bs.GetName(), false);
        CheckData(Reg::rdi);
        LoadData(Reg::rsi, Reg::rdi);
        e.Mov(R(modes.r[unit]), Reg::r8, 16);
        GenericAlb(op.GetName(), a.Unsigned16(), Reg::rsi);
        if (IsAlbModifying(op.GetName())) {
            StoreData(Reg::rdi, Reg::rsi);
        }
        return true;
    }

    bool mma_my_my(ArRn1 x, ArStep1 xs, RegName a, bool x0_sign, bool y0_sign, bool x1_sign,
                   bool y1_sign, SumBase base, bool sub_p0, bool p0_align, bool sub_p1,
                   bool p1_align) {
        const unsigned unit = Mode(modes.arrn[x.Index()]);
        const StepValue step = ArStep(Mode(modes.arstep[xs.Index()]));
        const OffsetValue offset = static_cast<OffsetValue>(Mode(modes.aroffset[xs.Index()]));
        if (!OffsetSupported(unit, offset, false)) {
            return RefuseMode();
        }
        RnAndModify(Reg::r8, Reg::r10, unit, step, false);
        OffsetAddress(Reg::rsi, Reg::r8, unit, offset, false);
        CheckData(Reg::r8);
        CheckData(Reg::rsi);
        // As in mma: the sum reads neither the factors nor the unit register.
        LoadData(Reg::rax, Reg::r8);
        e.Mov(R(modes.x[0]), Reg::rax, 16);
        LoadData(Reg::rax, Reg::rsi);
        e.Mov(R(modes.x[1]), Reg::rax, 16);
        e.Mov(R(modes.r[unit]), Reg::r10, 16);
        ProductSum(base, a, sub_p0, p0_align, sub_p1, p1_align);
        DoMultiplication(0, x0_sign, y0_sign);
        DoMultiplication(1, x1_sign, y1_sign);
        return true;
    }

    // ---------------------------------------------------------------- Jumps, which end a block
    // The interpreter runs these in Run rather than RunOrdinary: before one, it takes the jump
    // back at the end of a block-repeat loop, which the translator doesn't do for them (it refuses
    // one that ends a loop), and after one, it checks for an interrupt, which none of these can
    // make due (they don't change ie, im or ip, and a block only runs when none is due).

    bool br(Address18_16 addr_low, Address18_2 addr_high, Cond cond) {
        if (next == loop_end) {
            return Refuse();
        }
        Label skip, done;
        if (!JumpUnlessCondition(cond, skip)) {
            return Refuse();
        }
        e.MovImm(R(modes.pc), static_cast<s32>(Address32(addr_low, addr_high)), 32);
        e.Jmp(done);
        e.Bind(skip);
        e.MovImm(R(modes.pc), static_cast<s32>(next), 32);
        e.Bind(done);
        sets_pc = true;
        return true;
    }

    bool brr(RelAddr7 addr, Cond cond) {
        const u32 relative = addr.Relative32();
        if (next == loop_end || relative == 0xFFFFFFFF) {
            return Refuse(); // a jump to itself idles, which the interpreter handles
        }
        Label skip, done;
        if (!JumpUnlessCondition(cond, skip)) {
            return Refuse();
        }
        e.MovImm(R(modes.pc), static_cast<s32>(next + relative), 32);
        e.Jmp(done);
        e.Bind(skip);
        e.MovImm(R(modes.pc), static_cast<s32>(next), 32);
        e.Bind(done);
        sets_pc = true;
        return true;
    }

    bool call(Address18_16 addr_low, Address18_2 addr_high, Cond cond) {
        if (next == loop_end) {
            return Refuse();
        }
        Label skip, done;
        if (!JumpUnlessCondition(cond, skip)) {
            return Refuse();
        }
        // PushPC: the return address's halves at sp - 1 and sp - 2, the high one first if cpc.
        const bool cpc = Mode(modes.cpc) == 1;
        e.Movzx(Reg::rax, R(modes.sp), 16);
        e.Lea(Reg::rcx, Ptr(Reg::rax, -1), 32);
        e.Movzx(Reg::rcx, Reg::rcx, 16);
        e.Lea(Reg::rax, Ptr(Reg::rax, -2), 32);
        e.Movzx(Reg::rax, Reg::rax, 16);
        CheckData(Reg::rcx);
        CheckData(Reg::rax);
        e.MovImm(Reg::rsi, cpc ? next >> 16 : next & 0xFFFF);
        StoreData(Reg::rcx, Reg::rsi);
        e.MovImm(Reg::rsi, cpc ? next & 0xFFFF : next >> 16);
        StoreData(Reg::rax, Reg::rsi);
        e.Mov(R(modes.sp), Reg::rax, 16);
        e.MovImm(R(modes.pc), static_cast<s32>(Address32(addr_low, addr_high)), 32);
        e.Jmp(done);
        e.Bind(skip);
        e.MovImm(R(modes.pc), static_cast<s32>(next), 32);
        e.Bind(done);
        sets_pc = true;
        return true;
    }

    bool ret(Cond c) {
        if (next == loop_end) {
            return Refuse();
        }
        Label skip, done;
        if (!JumpUnlessCondition(c, skip)) {
            return Refuse();
        }
        // PopPC: from sp and sp + 1, the low half first if cpc.
        const bool cpc = Mode(modes.cpc) == 1;
        e.Movzx(Reg::rax, R(modes.sp), 16);
        e.Lea(Reg::rcx, Ptr(Reg::rax, 1), 32);
        e.Movzx(Reg::rcx, Reg::rcx, 16);
        CheckData(Reg::rax);
        CheckData(Reg::rcx);
        LoadData(Reg::rsi, Reg::rax);
        LoadData(Reg::rdi, Reg::rcx);
        // rsi and rdi are the low and high halves if cpc, else the high and low
        const Reg high = cpc ? Reg::rdi : Reg::rsi;
        const Reg low = cpc ? Reg::rsi : Reg::rdi;
        e.OpImm(Arith::cmp, high, 4, 32); // SetPC asserts the address is below 0x40000
        e.J(CC::ae, *bail);
        e.ShiftImm(Shift::shl, high, 16, 32);
        e.Op(Arith::or_, high, low, 32);
        e.Mov(R(modes.pc), high, 32);
        e.Lea(Reg::rcx, Ptr(Reg::rcx, 1), 32);
        e.Mov(R(modes.sp), Reg::rcx, 16);
        e.Jmp(done);
        e.Bind(skip);
        e.MovImm(R(modes.pc), static_cast<s32>(next), 32);
        e.Bind(done);
        sets_pc = true;
        return true;
    }

    bool bkrep(Imm8 a, Address16 addr) {
        if (next == loop_end) {
            return Refuse();
        }
        e.MovImm(Reg::rsi, a.Unsigned16());
        BlockRepeat(Reg::rsi, addr.Address32() | (next & 0x30000));
        return true;
    }

    bool bkrep(Register a, Address18_16 addr_low, Address18_2 addr_high) {
        if (next == loop_end || !CanReadBus16(a.GetName(), false)) {
            return Refuse();
        }
        RegToBus16(Reg::rsi, a.GetName(), false);
        BlockRepeat(Reg::rsi, Address32(addr_low, addr_high));
        return true;
    }

    // ---------------------------------------------------------------- More data instructions

    bool sub(Bx a, Ax b) {
        LoadAcc(Reg::rsi, a.GetName());
        LoadAcc(Reg::rax, b.GetName());
        AddSub(Reg::rax, Reg::rsi, true);
        SatAndSetAccAndFlag(b.GetName(), Reg::rax);
        return true;
    }

    bool mov(Imm16 a, Bx b) {
        e.MovImm(Reg::rsi, a.Unsigned16());
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    bool mov(Imm8 a, Axl b) {
        e.MovImm(Reg::rsi, a.Unsigned16());
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    bool mov(Imm8s a, Axh b) {
        e.MovImm(Reg::rsi, a.Signed16());
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    bool mov(Imm8s a, RnOld b) {
        e.MovImm(Reg::rsi, a.Signed16());
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    bool mov_sv(MemImm8 a) {
        PageAddress(Reg::rax, a);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        e.Mov(R(modes.sv), Reg::rsi, 16);
        return true;
    }

    bool mov_sv(Imm8s a) {
        e.MovImm(R(modes.sv), static_cast<s16>(a.Signed16()), 16);
        return true;
    }

    bool mov_sv_to(MemImm8 b) {
        PageAddress(Reg::rax, b);
        CheckData(Reg::rax);
        e.Movzx(Reg::rsi, R(modes.sv), 16);
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    bool push_x0() {
        return PushWord(modes.x[0]);
    }
    bool push_x1() {
        return PushWord(modes.x[1]);
    }
    bool push_y1() {
        return PushWord(modes.y[1]);
    }
    bool pop_x0() {
        return PopWord(modes.x[0]);
    }
    bool pop_x1() {
        return PopWord(modes.x[1]);
    }
    bool pop_y1() {
        return PopWord(modes.y[1]);
    }

    bool max_ge(Ax a, StepZIDS bs) {
        return MinMax(a.GetName(), bs.GetName(), MinMaxOp::MaxGe, false);
    }
    bool max_gt(Ax a, StepZIDS bs) {
        return MinMax(a.GetName(), bs.GetName(), MinMaxOp::MaxGt, false);
    }
    bool min_le(Ax a, StepZIDS bs) {
        return MinMax(a.GetName(), bs.GetName(), MinMaxOp::MinLe, false);
    }
    bool min_lt(Ax a, StepZIDS bs) {
        return MinMax(a.GetName(), bs.GetName(), MinMaxOp::MinLt, false);
    }
    bool max_ge_r0(Ax a, StepZIDS bs) {
        return MinMax(a.GetName(), bs.GetName(), MinMaxOp::MaxGe, true);
    }
    bool max_gt_r0(Ax a, StepZIDS bs) {
        return MinMax(a.GetName(), bs.GetName(), MinMaxOp::MaxGt, true);
    }
    bool min_le_r0(Ax a, StepZIDS bs) {
        return MinMax(a.GetName(), bs.GetName(), MinMaxOp::MinLe, true);
    }
    bool min_lt_r0(Ax a, StepZIDS bs) {
        return MinMax(a.GetName(), bs.GetName(), MinMaxOp::MinLt, true);
    }

    bool mul(Mul3 op, R45 y, StepZIDS ys, R0123 x, StepZIDS xs, Ax a) {
        const unsigned uy = y.Index();
        const unsigned ux = x.Index();
        RnAndModify(Reg::r8, Reg::r10, uy, ys.GetName(), false);
        RnAndModify(Reg::r9, Reg::r11, ux, xs.GetName(), false);
        CheckData(Reg::r8);
        CheckData(Reg::r9);
        LoadData(Reg::rax, Reg::r8);
        e.Mov(R(modes.y[0]), Reg::rax, 16);
        LoadData(Reg::rax, Reg::r9);
        e.Mov(R(modes.x[0]), Reg::rax, 16);
        e.Mov(R(modes.r[uy]), Reg::r10, 16);
        e.Mov(R(modes.r[ux]), Reg::r11, 16);
        MulGeneric(op.GetName(), a.GetName());
        return true;
    }

    bool push(Imm16 a) {
        e.MovImm(Reg::rsi, a.Unsigned16());
        e.Movzx(Reg::rax, R(modes.sp), 16);
        e.OpImm(Arith::sub, Reg::rax, 1, 32);
        e.Movzx(Reg::rax, Reg::rax, 16);
        CheckData(Reg::rax);
        e.Mov(R(modes.sp), Reg::rax, 16);
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    bool mov(MemImm8 a, RnOld b) {
        PageAddress(Reg::rax, a);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    bool mov(RnOld a, MemImm8 b) {
        RegToBus16(Reg::rsi, a.GetName(), false);
        PageAddress(Reg::rax, b);
        CheckData(Reg::rax);
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    bool mov(Axl a, MemImm16 b) {
        RegToBus16(Reg::rsi, a.GetName(), true);
        e.MovImm(Reg::rax, b.Unsigned16());
        CheckData(Reg::rax);
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    bool mov(Axl a, MemR7Imm7s b) {
        RegToBus16(Reg::rsi, a.GetName(), true);
        R7Address(Reg::rax, b.Signed16());
        CheckData(Reg::rax);
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    bool mov(Axl a, MemR7Imm16 b) {
        RegToBus16(Reg::rsi, a.GetName(), true);
        R7Address(Reg::rax, b.Unsigned16());
        CheckData(Reg::rax);
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    bool mov(MemR7Imm16 a, Ax b) {
        R7Address(Reg::rax, a.Unsigned16());
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        RegFromBus16(b.GetName(), Reg::rsi);
        return true;
    }

    bool sub(Ab a, Bx b) {
        LoadAcc(Reg::rsi, a.GetName());
        LoadAcc(Reg::rax, b.GetName());
        AddSub(Reg::rax, Reg::rsi, true);
        SatAndSetAccAndFlag(b.GetName(), Reg::rax);
        return true;
    }

    bool swap(SwapType swap) {
        RegName s0, d0, s1, d1;
        const auto both = [&](RegName x, RegName y) {
            // u = x, v = y; x = sat(v); y = sat(u)
            LoadAcc(Reg::rsi, x);
            LoadAcc(Reg::rdi, y);
            SatAndSetAccAndFlag(x, Reg::rdi);
            SatAndSetAccAndFlag(y, Reg::rsi);
        };
        switch (swap.GetName()) {
        case SwapTypeValue::a0b0:
            s0 = d1 = RegName::a0;
            s1 = d0 = RegName::b0;
            break;
        case SwapTypeValue::a0b1:
            s0 = d1 = RegName::a0;
            s1 = d0 = RegName::b1;
            break;
        case SwapTypeValue::a1b0:
            s0 = d1 = RegName::a1;
            s1 = d0 = RegName::b0;
            break;
        case SwapTypeValue::a1b1:
            s0 = d1 = RegName::a1;
            s1 = d0 = RegName::b1;
            break;
        case SwapTypeValue::a0b0a1b1:
            both(RegName::a1, RegName::b1);
            s0 = d1 = RegName::a0;
            s1 = d0 = RegName::b0;
            break;
        case SwapTypeValue::a0b1a1b0:
            both(RegName::a1, RegName::b0);
            s0 = d1 = RegName::a0;
            s1 = d0 = RegName::b1;
            break;
        case SwapTypeValue::a0b0a1:
            s0 = RegName::a0;
            d0 = s1 = RegName::b0;
            d1 = RegName::a1;
            break;
        case SwapTypeValue::a0b1a1:
            s0 = RegName::a0;
            d0 = s1 = RegName::b1;
            d1 = RegName::a1;
            break;
        case SwapTypeValue::a1b0a0:
            s0 = RegName::a1;
            d0 = s1 = RegName::b0;
            d1 = RegName::a0;
            break;
        case SwapTypeValue::a1b1a0:
            s0 = RegName::a1;
            d0 = s1 = RegName::b1;
            d1 = RegName::a0;
            break;
        case SwapTypeValue::b0a0b1:
            s0 = d1 = RegName::a0;
            d0 = RegName::b1;
            s1 = RegName::b0;
            break;
        case SwapTypeValue::b0a1b1:
            s0 = d1 = RegName::a1;
            d0 = RegName::b1;
            s1 = RegName::b0;
            break;
        case SwapTypeValue::b1a0b0:
            s0 = d1 = RegName::a0;
            d0 = RegName::b0;
            s1 = RegName::b1;
            break;
        case SwapTypeValue::b1a1b0:
            s0 = d1 = RegName::a1;
            d0 = RegName::b0;
            s1 = RegName::b1;
            break;
        default:
            return Refuse();
        }
        LoadAcc(Reg::rsi, s0);
        LoadAcc(Reg::rdi, s1);
        SatAndSetAccAndFlag(d0, Reg::rsi);
        SatAndSetAccAndFlag(d1, Reg::rdi); // its flags are the ones that stay
        return true;
    }

    bool rets(Imm8 a) {
        if (next == loop_end) {
            return Refuse();
        }
        // PopPC, then sp += a
        const bool cpc = Mode(modes.cpc) == 1;
        e.Movzx(Reg::rax, R(modes.sp), 16);
        e.Lea(Reg::rcx, Ptr(Reg::rax, 1), 32);
        e.Movzx(Reg::rcx, Reg::rcx, 16);
        CheckData(Reg::rax);
        CheckData(Reg::rcx);
        LoadData(Reg::rsi, Reg::rax);
        LoadData(Reg::rdi, Reg::rcx);
        const Reg high = cpc ? Reg::rdi : Reg::rsi;
        const Reg low = cpc ? Reg::rsi : Reg::rdi;
        e.OpImm(Arith::cmp, high, 4, 32);
        e.J(CC::ae, *bail);
        e.ShiftImm(Shift::shl, high, 16, 32);
        e.Op(Arith::or_, high, low, 32);
        e.Mov(R(modes.pc), high, 32);
        e.Lea(Reg::rcx, Ptr(Reg::rcx, 1 + static_cast<s32>(a.Unsigned16())), 32);
        e.Mov(R(modes.sp), Reg::rcx, 16);
        sets_pc = true;
        return true;
    }

    bool bkrepsto_memsp() {
        if (next == loop_end) {
            return Refuse();
        }
        // StoreBlockRepeat(sp): frame 0 at sp - 1 to sp - 4, then the frames move down.
        const s32 frame_size = static_cast<s32>(sizeof(RegisterState::BlockRepeatFrame));
        const s32 f0 = Offset(modes.bkrep_stack[0]);
        const s32 start = f0 + static_cast<s32>(offsetof(RegisterState::BlockRepeatFrame, start));
        const s32 end = f0 + static_cast<s32>(offsetof(RegisterState::BlockRepeatFrame, end));
        const s32 lc = f0 + static_cast<s32>(offsetof(RegisterState::BlockRepeatFrame, lc));
        e.Movzx(Reg::r8, R(modes.sp), 16);
        const Reg addr[4] = {Reg::r9, Reg::r10, Reg::r11, Reg::rdi};
        for (int k = 0; k < 4; ++k) {
            e.Lea(addr[k], Ptr(Reg::r8, -(k + 1)), 32);
            e.Movzx(addr[k], addr[k], 16);
            CheckData(addr[k]);
        }
        e.Movzx(Reg::rsi, Ptr(RegsBase, lc), 16);
        StoreData(addr[0], Reg::rsi);
        e.Mov(Reg::rsi, Ptr(RegsBase, start), 32);
        StoreData(addr[1], Reg::rsi);
        e.Mov(Reg::rsi, Ptr(RegsBase, end), 32);
        StoreData(addr[2], Reg::rsi);
        // flag = lp << 15 | start >> 16 | (end >> 16) << 8
        e.Movzx(Reg::rsi, R(modes.lp), 16);
        e.ShiftImm(Shift::shl, Reg::rsi, 15, 32);
        e.Mov(Reg::rax, Ptr(RegsBase, start), 32);
        e.ShiftImm(Shift::shr, Reg::rax, 16, 32);
        e.Op(Arith::or_, Reg::rsi, Reg::rax, 32);
        e.Mov(Reg::rax, Ptr(RegsBase, end), 32);
        e.ShiftImm(Shift::shr, Reg::rax, 16, 32);
        e.ShiftImm(Shift::shl, Reg::rax, 8, 32);
        e.Op(Arith::or_, Reg::rsi, Reg::rax, 32);
        StoreData(addr[3], Reg::rsi);
        e.Mov(R(modes.sp), addr[3], 16);
        // if lp: frames 1 .. bcn - 1 move to 0 .. bcn - 2; --bcn; lp = bcn != 0
        Label done, moved;
        e.OpImm(Arith::cmp, R(modes.lp), 0, 16);
        e.J(CC::e, done);
        e.Movzx(Reg::rax, R(modes.bcn), 16);
        for (int k = 1; k <= 3; ++k) {
            e.OpImm(Arith::cmp, Reg::rax, k + 1, 32); // frame k moves if k < bcn
            e.J(CC::b, moved);
            e.Mov(Reg::rcx, Ptr(RegsBase, f0 + k * frame_size), 64);
            e.Mov(Ptr(RegsBase, f0 + (k - 1) * frame_size), Reg::rcx, 64);
            e.Mov(Reg::rcx, Ptr(RegsBase, f0 + k * frame_size + 8), 32);
            e.Mov(Ptr(RegsBase, f0 + (k - 1) * frame_size + 8), Reg::rcx, 32);
        }
        e.Bind(moved);
        e.OpImm(Arith::sub, R(modes.bcn), 1, 16);
        e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
        e.OpImm(Arith::cmp, R(modes.bcn), 0, 16);
        e.Set(CC::ne, Reg::rax);
        e.Mov(R(modes.lp), Reg::rax, 16);
        e.Bind(done);
        e.MovImm(R(modes.pc), static_cast<s32>(next), 32);
        sets_pc = true;
        return true;
    }

    bool bkreprst_memsp() {
        if (next == loop_end) {
            return Refuse();
        }
        // RestoreBlockRepeat(sp): read four words, check the asserts, then change anything.
        const s32 frame_size = static_cast<s32>(sizeof(RegisterState::BlockRepeatFrame));
        const s32 f0 = Offset(modes.bkrep_stack[0]);
        const s32 start = f0 + static_cast<s32>(offsetof(RegisterState::BlockRepeatFrame, start));
        const s32 end = f0 + static_cast<s32>(offsetof(RegisterState::BlockRepeatFrame, end));
        const s32 lc = f0 + static_cast<s32>(offsetof(RegisterState::BlockRepeatFrame, lc));
        e.Movzx(Reg::r8, R(modes.sp), 16);
        const Reg addr[4] = {Reg::r9, Reg::r10, Reg::r11, Reg::rdi};
        for (int k = 0; k < 4; ++k) {
            e.Lea(addr[k], Ptr(Reg::r8, k), 32);
            e.Movzx(addr[k], addr[k], 16);
            CheckData(addr[k]);
        }
        // Load into the address registers themselves: flag, end, start, lc.
        for (int k = 0; k < 4; ++k) {
            LoadData(Reg::rax, addr[k]);
            e.Mov(addr[k], Reg::rax, 32);
        }
        // The asserts: if lp, bcn <= 3 and the flag's valid bit.
        Label no_loop, checked;
        e.OpImm(Arith::cmp, R(modes.lp), 0, 16);
        e.J(CC::e, no_loop);
        e.OpImm(Arith::cmp, R(modes.bcn), 3, 16);
        e.J(CC::a, *bail);
        e.TestImm(addr[0], 0x8000, 32);
        e.J(CC::e, *bail);
        // copy_backward frames 0 .. bcn - 1 to 1 .. bcn; ++bcn
        e.Movzx(Reg::rax, R(modes.bcn), 16);
        for (int k = 2; k >= 0; --k) {
            Label skip;
            e.OpImm(Arith::cmp, Reg::rax, k + 1, 32); // frame k moves if k < bcn
            e.J(CC::b, skip);
            e.Mov(Reg::rcx, Ptr(RegsBase, f0 + k * frame_size), 64);
            e.Mov(Ptr(RegsBase, f0 + (k + 1) * frame_size), Reg::rcx, 64);
            e.Mov(Reg::rcx, Ptr(RegsBase, f0 + k * frame_size + 8), 32);
            e.Mov(Ptr(RegsBase, f0 + (k + 1) * frame_size + 8), Reg::rcx, 32);
            e.Bind(skip);
        }
        e.OpImm(Arith::add, R(modes.bcn), 1, 16);
        e.Jmp(checked);
        e.Bind(no_loop);
        // if valid: lp = bcn = 1
        e.TestImm(addr[0], 0x8000, 32);
        e.J(CC::e, checked);
        e.MovImm(R(modes.lp), 1, 16);
        e.MovImm(R(modes.bcn), 1, 16);
        e.Bind(checked);
        // frame 0: end, start (with bits from the flag) and lc
        e.Mov(Reg::rax, addr[0], 32);
        e.ShiftImm(Shift::shr, Reg::rax, 8, 32);
        e.OpImm(Arith::and_, Reg::rax, 3, 32);
        e.ShiftImm(Shift::shl, Reg::rax, 16, 32);
        e.Op(Arith::or_, Reg::rax, addr[1], 32);
        e.Mov(Ptr(RegsBase, end), Reg::rax, 32);
        e.Mov(Reg::rax, addr[0], 32);
        e.OpImm(Arith::and_, Reg::rax, 3, 32);
        e.ShiftImm(Shift::shl, Reg::rax, 16, 32);
        e.Op(Arith::or_, Reg::rax, addr[2], 32);
        e.Mov(Ptr(RegsBase, start), Reg::rax, 32);
        e.Mov(Ptr(RegsBase, lc), addr[3], 16);
        e.Lea(Reg::rax, Ptr(Reg::r8, 4), 32);
        e.Mov(R(modes.sp), Reg::rax, 16);
        e.MovImm(R(modes.pc), static_cast<s32>(next), 32);
        sets_pc = true;
        return true;
    }

private:
    enum class MinMaxOp { MaxGe, MaxGt, MinLe, MinLt };

    // max_ge and the others: compares the accumulator with its counterpart (or with the
    // sign-extended word at [r0]), and takes the other value if it's greater (or less, or equal as
    // well), setting fm and mixp; r0 steps.
    bool MinMax(RegName a, StepValue step, MinMaxOp op, bool from_memory) {
        ComputeFlags();                                    // it sets fm
        RnAndModifyRaw(Reg::rdi, Reg::r8, 0, step, false); // rdi = the old r0, r8 = the new one
        if (from_memory) {
            e.Mov(Reg::rax, Reg::rdi, 32);
            RnAddress(Reg::rax, 0);
            CheckData(Reg::rax);
            LoadData(Reg::rsi, Reg::rax);
            e.Movsx(Reg::rsi, Reg::rsi, 16, 64); // v
        } else {
            LoadAcc(Reg::rsi, CounterAcc(a)); // v
        }
        e.Mov(R(modes.r[0]), Reg::r8, 16);
        LoadAcc(Reg::rax, a); // u
        e.Mov(Reg::rcx, Reg::rsi, 64);
        e.Op(Arith::sub, Reg::rcx, Reg::rax, 64); // d = v - u
        // take = the condition on d
        Label take, done;
        switch (op) {
        case MinMaxOp::MaxGe: // d >= 0 (sign clear)
            e.Test(Reg::rcx, Reg::rcx, 64);
            e.J(CC::ns, take);
            break;
        case MinMaxOp::MaxGt: // sign clear and d != 0
            e.Test(Reg::rcx, Reg::rcx, 64);
            e.J(CC::g, take);
            break;
        case MinMaxOp::MinLe: // sign set or d == 0
            e.Test(Reg::rcx, Reg::rcx, 64);
            e.J(CC::le, take);
            break;
        case MinMaxOp::MinLt: // sign set
            e.Test(Reg::rcx, Reg::rcx, 64);
            e.J(CC::s, take);
            break;
        }
        e.MovImm(R(modes.fm), 0, 16);
        e.Jmp(done);
        e.Bind(take);
        e.MovImm(R(modes.fm), 1, 16);
        e.Mov(R(modes.mixp), Reg::rdi, 16);
        StoreAcc(a, Reg::rsi);
        e.Bind(done);
        return true;
    }

    static RegName CounterAcc(RegName in) {
        switch (in) {
        case RegName::a0:
            return RegName::a1;
        case RegName::a1:
            return RegName::a0;
        case RegName::b0:
            return RegName::b1;
        default:
            return RegName::b0;
        }
    }

    // push: a register's word to --sp.
    bool PushWord(const u16& field) {
        e.Movzx(Reg::rsi, R(field), 16);
        e.Movzx(Reg::rax, R(modes.sp), 16);
        e.OpImm(Arith::sub, Reg::rax, 1, 32);
        e.Movzx(Reg::rax, Reg::rax, 16);
        CheckData(Reg::rax);
        e.Mov(R(modes.sp), Reg::rax, 16);
        StoreData(Reg::rax, Reg::rsi);
        return true;
    }

    // pop: the word at sp++ to a register.
    bool PopWord(const u16& field) {
        e.Movzx(Reg::rax, R(modes.sp), 16);
        CheckData(Reg::rax);
        LoadData(Reg::rsi, Reg::rax);
        e.OpImm(Arith::add, Reg::rax, 1, 32);
        e.Mov(R(modes.sp), Reg::rax, 16);
        e.Mov(R(field), Reg::rsi, 16);
        return true;
    }

    // BlockRepeat(lc, address): a new innermost loop from the next instruction to `end`, with
    // the count in `lc`. Ends the block, since the loop changes where blocks end.
    void BlockRepeat(Reg lc, u32 end) {
        const s32 stack = Offset(modes.bkrep_stack[0]);
        const s32 frame_size = static_cast<s32>(sizeof(RegisterState::BlockRepeatFrame));
        e.Movzx(Reg::rax, R(modes.bcn), 16);
        e.OpImm(Arith::cmp, Reg::rax, 3, 32); // the interpreter asserts bcn <= 3
        e.J(CC::a, *bail);
        e.MovImm(Reg::rcx, static_cast<u64>(frame_size));
        e.Imul(Reg::rcx, Reg::rax, 64);
        e.Lea(Reg::rcx, Ptr(RegsBase, Reg::rcx, 1, stack), 64);
        e.MovImm(Ptr(Reg::rcx, static_cast<s32>(offsetof(RegisterState::BlockRepeatFrame, start))),
                 static_cast<s32>(next), 32);
        e.MovImm(Ptr(Reg::rcx, static_cast<s32>(offsetof(RegisterState::BlockRepeatFrame, end))),
                 static_cast<s32>(end), 32);
        e.Mov(Ptr(Reg::rcx, static_cast<s32>(offsetof(RegisterState::BlockRepeatFrame, lc))), lc,
              16);
        e.MovImm(R(modes.lp), 1, 16);
        e.OpImm(Arith::add, R(modes.bcn), 1, 16);
        e.MovImm(R(modes.pc), static_cast<s32>(next), 32);
        sets_pc = true;
    }

    enum class OffsetValue : u16 {
        Zero = 0,
        PlusOne = 1,
        MinusOne = 2,
        MinusOneDmod = 3,
    };

    bool Refuse() {
        refusal = Refusal::Kind;
        return false;
    }

    bool RefuseMode() {
        refusal = Refusal::Mode;
        return false;
    }

    // ------------------------------------------------------------------ Registers and modes

    template <typename T>
    s32 Offset(const T& field) const {
        return static_cast<s32>(reinterpret_cast<const u8*>(&field) -
                                reinterpret_cast<const u8*>(&modes));
    }

    // The field of the registers, in the block's memory operand form.
    template <typename T>
    Mem R(const T& field) const {
        return Ptr(RegsBase, Offset(field));
    }

    // A mode register's value at translation time, which the block's code then requires.
    template <typename T>
    T Mode(const T& field) {
        static_assert(sizeof(T) == 2 || sizeof(T) == 4);
        AddGuard(
            Guard{nullptr, Offset(field), static_cast<u8>(sizeof(T)), static_cast<u32>(field)});
        return field;
    }

    template <typename T>
    T MiuMode(const T& field) {
        static_assert(sizeof(T) == 2);
        const auto delta = reinterpret_cast<const u8*>(&field) - reinterpret_cast<const u8*>(&miu);
        AddGuard(Guard{reinterpret_cast<const u8*>(live_miu) + delta, static_cast<s32>(delta), 2,
                       static_cast<u32>(field)});
        return field;
    }

    void AddGuard(const Guard& guard) {
        for (const Guard& g : guards) {
            if (g.absolute == guard.absolute && g.offset == guard.offset) {
                return;
            }
        }
        guards.push_back(guard);
    }

    static StepValue ArStep(u16 value) {
        return value <= 7 ? static_cast<StepValue>(value) : StepValue::Zero;
    }

    Mem Acc(RegName name) const {
        switch (name) {
        case RegName::a0:
        case RegName::a0h:
        case RegName::a0l:
        case RegName::a0e:
            return R(modes.a[0]);
        case RegName::a1:
        case RegName::a1h:
        case RegName::a1l:
        case RegName::a1e:
            return R(modes.a[1]);
        case RegName::b0:
        case RegName::b0h:
        case RegName::b0l:
        case RegName::b0e:
            return R(modes.b[0]);
        case RegName::b1:
        case RegName::b1h:
        case RegName::b1l:
        case RegName::b1e:
            return R(modes.b[1]);
        default:
            UNREACHABLE();
        }
    }

    static bool IsAcc(RegName name) {
        switch (name) {
        case RegName::a0:
        case RegName::a0h:
        case RegName::a0l:
        case RegName::a0e:
        case RegName::a1:
        case RegName::a1h:
        case RegName::a1l:
        case RegName::a1e:
        case RegName::b0:
        case RegName::b0h:
        case RegName::b0l:
        case RegName::b0e:
        case RegName::b1:
        case RegName::b1h:
        case RegName::b1l:
        case RegName::b1e:
            return true;
        default:
            return false;
        }
    }

    void LoadAcc(Reg dst, RegName name) {
        e.Mov(dst, Acc(name), 64);
    }

    void StoreAcc(RegName name, Reg src) {
        e.Mov(Acc(name), src, 64);
    }

    static int RnIndex(RegName name) {
        switch (name) {
        case RegName::r0:
            return 0;
        case RegName::r1:
            return 1;
        case RegName::r2:
            return 2;
        case RegName::r3:
            return 3;
        case RegName::r4:
            return 4;
        case RegName::r5:
            return 5;
        case RegName::r6:
            return 6;
        case RegName::r7:
            return 7;
        default:
            return -1;
        }
    }

    // The registers RegToBus16 and RegFromBus16 handle here; the rest are refused.
    static bool CanReadBus16(RegName name, bool /*enable_sat_for_mov*/) {
        if (IsAcc(name)) {
            return name != RegName::a0e && name != RegName::a1e && name != RegName::b0e &&
                   name != RegName::b1e;
        }
        return RnIndex(name) >= 0 || name == RegName::y0 || name == RegName::p ||
               name == RegName::sp || name == RegName::sv;
    }

    static bool CanWriteBus16(RegName name) {
        return CanReadBus16(name, false);
    }

    // RegToBus16: the register's value on the 16-bit bus, zero-extended into dst. Clobbers rcx and
    // rdx.
    void RegToBus16(Reg dst, RegName name, bool enable_sat_for_mov) {
        switch (name) {
        case RegName::a0:
        case RegName::a1:
        case RegName::b0:
        case RegName::b1:
            // the low half, never saturated
            e.Movzx(dst, Acc(name), 16);
            return;
        case RegName::a0l:
        case RegName::a1l:
        case RegName::b0l:
        case RegName::b1l:
            if (enable_sat_for_mov) {
                GetAndSatAcc(dst, name);
                e.Movzx(dst, dst, 16);
            } else {
                e.Movzx(dst, Acc(name), 16);
            }
            return;
        case RegName::a0h:
        case RegName::a1h:
        case RegName::b0h:
        case RegName::b1h:
            if (enable_sat_for_mov) {
                GetAndSatAcc(dst, name);
                e.ShiftImm(Shift::shr, dst, 16, 64);
                e.Movzx(dst, dst, 16);
            } else {
                e.Mov(dst, Acc(name), 64);
                e.ShiftImm(Shift::shr, dst, 16, 64);
                e.Movzx(dst, dst, 16);
            }
            return;
        case RegName::y0:
            e.Movzx(dst, R(modes.y[0]), 16);
            return;
        case RegName::p:
            ProductToBus40(dst, 0);
            e.ShiftImm(Shift::shr, dst, 16, 64);
            e.Movzx(dst, dst, 16);
            return;
        case RegName::sp:
            e.Movzx(dst, R(modes.sp), 16);
            return;
        case RegName::sv:
            e.Movzx(dst, R(modes.sv), 16);
            return;
        default:
            break;
        }
        const int rn = RnIndex(name);
        ASSERT(rn >= 0);
        e.Movzx(dst, R(modes.r[rn]), 16);
    }

    // RegFromBus16: value (zero-extended 16 bits, in a register other than rax and rcx) into the
    // register. Clobbers rax, rcx, rdx and r8 to r10.
    void RegFromBus16(RegName name, Reg value) {
        switch (name) {
        case RegName::a0:
        case RegName::a1:
        case RegName::b0:
        case RegName::b1:
            e.Movsx(Reg::rax, value, 16, 64); // SignExtend<16>
            SatAndSetAccAndFlag(name, Reg::rax);
            return;
        case RegName::a0l:
        case RegName::a1l:
        case RegName::b0l:
        case RegName::b1l:
            e.Movzx(Reg::rax, value, 16);
            SatAndSetAccAndFlag(name, Reg::rax);
            return;
        case RegName::a0h:
        case RegName::a1h:
        case RegName::b0h:
        case RegName::b1h:
            e.Movzx(Reg::rax, value, 16);
            e.ShiftImm(Shift::shl, Reg::rax, 16, 32);
            e.Movsx(Reg::rax, Reg::rax, 32, 64); // SignExtend<32>(value << 16)
            SatAndSetAccAndFlag(name, Reg::rax);
            return;
        case RegName::y0:
            e.Mov(R(modes.y[0]), value, 16);
            return;
        case RegName::p: {
            // p0h: pe = value > 0x7FFF; p = (p & 0xFFFF) | value << 16
            e.Movzx(Reg::rax, value, 16);
            e.Mov(Reg::rcx, Reg::rax, 32);
            e.ShiftImm(Shift::shr, Reg::rcx, 15, 32);
            e.Mov(R(modes.pe[0]), Reg::rcx, 16);
            e.ShiftImm(Shift::shl, Reg::rax, 16, 32);
            e.Movzx(Reg::rcx, R(modes.p[0]), 16);
            e.Op(Arith::or_, Reg::rax, Reg::rcx, 32);
            e.Mov(R(modes.p[0]), Reg::rax, 32);
            return;
        }
        case RegName::sp:
            e.Mov(R(modes.sp), value, 16);
            return;
        case RegName::sv:
            e.Mov(R(modes.sv), value, 16);
            return;
        default:
            break;
        }
        const int rn = RnIndex(name);
        ASSERT(rn >= 0);
        e.Mov(R(modes.r[rn]), value, 16);
    }

    // ------------------------------------------------------------------ Accumulators and flags

    // SetAccFlag: fz, fm, fe and fn from the value in `value` (unchanged), which only keeps the
    // value (see Flags).
    void SetAccFlag(Reg value) {
        e.Mov(Ptr(ContextReg, offsetof(Context, flag_value)), value, 64);
        flags = Flags::Pending;
    }

    // SaturateAcc on the value in `value`: to 32 bits, to the sign of bit 39, if it doesn't fit,
    // which sets flm. Clobbers rcx and rdx.
    void SaturateAcc(Reg value) {
        ASSERT(value != Reg::rcx && value != Reg::rdx);
        Label done;
        e.Movsx(Reg::rcx, value, 32, 64);
        e.Op(Arith::cmp, Reg::rcx, value, 64);
        e.J(CC::e, done);
        e.MovImm(R(modes.flm), 1, 16);
        e.Mov(Reg::rdx, value, 64);
        e.ShiftImm(Shift::shr, Reg::rdx, 39, 64);
        e.MovImm(value, 0x7FFF'FFFF);
        e.MovImm(Reg::rcx, 0xFFFF'FFFF'8000'0000);
        e.Test(Reg::rdx, Reg::rdx, 64);
        e.Cmov(CC::ne, value, Reg::rcx, 64);
        e.Bind(done);
    }

    void SetAccAndFlag(RegName name, Reg value) {
        SetAccFlag(value);
        StoreAcc(name, value);
    }

    void SatAndSetAccAndFlag(RegName name, Reg value) {
        SetAccFlag(value);
        if (!Mode(modes.sata)) {
            SaturateAcc(value);
        }
        StoreAcc(name, value);
    }

    // GetAndSatAcc: the accumulator, saturated to 32 bits unless sat is set, which sets flm.
    // Setting flm before the instruction may still stop is fine: the interpreter, running the
    // instruction instead, sets it again, and nothing it does depends on it. Clobbers rcx, rdx.
    void GetAndSatAcc(Reg dst, RegName name) {
        LoadAcc(dst, name);
        if (Mode(modes.sat)) {
            return;
        }
        // value != SignExtend<32>(value): saturate to the sign of bit 39
        Label done;
        e.Movsx(Reg::rcx, dst, 32, 64);
        e.Op(Arith::cmp, Reg::rcx, dst, 64);
        e.J(CC::e, done);
        e.MovImm(R(modes.flm), 1, 16);
        e.Mov(Reg::rdx, dst, 64);
        e.ShiftImm(Shift::shr, Reg::rdx, 39, 64);
        e.MovImm(dst, 0x7FFF'FFFF);
        e.MovImm(Reg::rcx, 0xFFFF'FFFF'8000'0000);
        e.Test(Reg::rdx, Reg::rdx, 64);
        e.Cmov(CC::ne, dst, Reg::rcx, 64);
        e.Bind(done);
    }

    // GetAndSatAccNoFlag: GetAndSatAcc without setting flm. Clobbers rcx, rdx.
    void GetAndSatAccNoFlag(Reg dst, RegName name) {
        LoadAcc(dst, name);
        if (Mode(modes.sat)) {
            return;
        }
        Label done;
        e.Movsx(Reg::rcx, dst, 32, 64);
        e.Op(Arith::cmp, Reg::rcx, dst, 64);
        e.J(CC::e, done);
        e.Mov(Reg::rdx, dst, 64);
        e.ShiftImm(Shift::shr, Reg::rdx, 39, 64);
        e.MovImm(dst, 0x7FFF'FFFF);
        e.MovImm(Reg::rcx, 0xFFFF'FFFF'8000'0000);
        e.Test(Reg::rdx, Reg::rdx, 64);
        e.Cmov(CC::ne, dst, Reg::rcx, 64);
        e.Bind(done);
    }

    // The address of an [r7 + imm] operand into dst.
    void R7Address(Reg dst, u16 imm) {
        e.Movzx(dst, R(modes.r[7]), 16);
        e.OpImm(Arith::add, dst, imm, 32);
        e.Movzx(dst, dst, 16);
    }

    static bool AluSupported(AlmOp op) {
        switch (op) {
        case AlmOp::Or:
        case AlmOp::And:
        case AlmOp::Xor:
        case AlmOp::Add:
        case AlmOp::Cmp:
        case AlmOp::Sub:
            return true;
        default:
            return false;
        }
    }

    // MulGeneric(op, a): the accumulator gets the product unless op is Mpy or Mpysu, then
    // x0 * y0 with op's signs. Clobbers rax to r10.
    void MulGeneric(MulOp op, RegName a) {
        if (op != MulOp::Mpy && op != MulOp::Mpysu) {
            LoadAcc(Reg::rax, a);
            ProductToBus40(Reg::rsi, 0);
            if (op == MulOp::Maa || op == MulOp::Maasu) {
                // product = SignExtend<24>(product >> 16), the shift being logical
                e.ShiftImm(Shift::shr, Reg::rsi, 16, 64);
                e.ShiftImm(Shift::shl, Reg::rsi, 40, 64);
                e.ShiftImm(Shift::sar, Reg::rsi, 40, 64);
            }
            AddSub(Reg::rax, Reg::rsi, false);
            SatAndSetAccAndFlag(a, Reg::rax);
        }
        switch (op) {
        case MulOp::Mpy:
        case MulOp::Mac:
        case MulOp::Maa:
            DoMultiplication(0, true, true);
            break;
        case MulOp::Mpysu:
        case MulOp::Macsu:
        case MulOp::Maasu:
            DoMultiplication(0, false, true);
            break;
        case MulOp::Macus:
            DoMultiplication(0, true, false);
            break;
        case MulOp::Macuu:
            DoMultiplication(0, false, false);
            break;
        }
    }

    static bool IsAlbModifying(AlbOp op) {
        switch (op) {
        case AlbOp::Set:
        case AlbOp::Rst:
        case AlbOp::Chng:
        case AlbOp::Addv:
        case AlbOp::Subv:
            return true;
        default:
            return false;
        }
    }

    // GenericAlb(op, a, b) with b (zero-extended 16 bits) in rsi, which gets the result; sets fz,
    // and fm and fc0 as the operation does. Clobbers rax, rcx, rdx.
    void GenericAlb(AlbOp op, u16 a, Reg b) {
        ASSERT(b == Reg::rsi);
        switch (op) {
        case AlbOp::Set:
        case AlbOp::Rst:
        case AlbOp::Chng:
            if (op == AlbOp::Set) {
                e.OpImm(Arith::or_, Reg::rsi, a, 32);
            } else if (op == AlbOp::Rst) {
                e.OpImm(Arith::and_, Reg::rsi, static_cast<u16>(~a), 32);
            } else {
                e.OpImm(Arith::xor_, Reg::rsi, a, 32);
            }
            e.Mov(Reg::rax, Reg::rsi, 32);
            e.ShiftImm(Shift::shr, Reg::rax, 15, 32);
            e.Mov(R(modes.fm), Reg::rax, 16);
            break;
        case AlbOp::Addv:
        case AlbOp::Cmpv:
        case AlbOp::Subv: {
            const bool add = op == AlbOp::Addv;
            // r = b + a or b - a (32 bits); fc0 = r >> 16 != 0
            e.Mov(Reg::rax, Reg::rsi, 32);
            e.OpImm(add ? Arith::add : Arith::sub, Reg::rax, a, 32);
            e.Op(Arith::xor_, Reg::rcx, Reg::rcx, 32);
            e.Mov(Reg::rdx, Reg::rax, 32);
            e.ShiftImm(Shift::shr, Reg::rdx, 16, 32);
            e.Set(CC::ne, Reg::rcx);
            e.Mov(R(modes.fc0), Reg::rcx, 16);
            // fm = (SignExtend<16>(b) +/- SignExtend<16>(a)) >> 31
            e.Movsx(Reg::rdx, Reg::rsi, 16, 32);
            e.OpImm(add ? Arith::add : Arith::sub, Reg::rdx, static_cast<s16>(a), 32);
            e.ShiftImm(Shift::shr, Reg::rdx, 31, 32);
            e.Mov(R(modes.fm), Reg::rdx, 16);
            e.Movzx(Reg::rsi, Reg::rax, 16);
            break;
        }
        case AlbOp::Tst0:
            // result = (a & b) != 0
            e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
            e.TestImm(Reg::rsi, a, 32);
            e.Set(CC::ne, Reg::rax);
            e.Mov(Reg::rsi, Reg::rax, 32);
            break;
        case AlbOp::Tst1:
            // result = (a & ~b) != 0
            e.Not(Reg::rsi, 32);
            e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
            e.TestImm(Reg::rsi, a, 32);
            e.Set(CC::ne, Reg::rax);
            e.Mov(Reg::rsi, Reg::rax, 32);
            break;
        default:
            UNREACHABLE();
        }
        // fz = result == 0
        e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
        e.Test(Reg::rsi, Reg::rsi, 32);
        e.Set(CC::e, Reg::rax);
        e.Mov(R(modes.fz), Reg::rax, 16);
    }

    // AddSub(a, b, sub) with the result in `a` (sign-extended from 40 bits); sets fc0, fv and fvl.
    // `b` is clobbered. Clobbers rcx, rdx.
    void AddSub(Reg a, Reg b, bool sub) {
        ASSERT(a != Reg::rcx && a != Reg::rdx && b != Reg::rcx && b != Reg::rdx);
        e.Op(Arith::xor_, Reg::rcx, Reg::rcx, 32);
        e.Op(Arith::xor_, Reg::rdx, Reg::rdx, 32);
        AddSubShifted(a, b, sub, Reg::rcx, Reg::rdx);
        e.ShiftImm(Shift::sar, a, 24, 64); // SignExtend<40>(result)
        e.Mov(R(modes.fc0), Reg::rcx, 16);
        e.Mov(R(modes.fv), Reg::rdx, 16);
        e.Op(Arith::or_, R(modes.fvl), Reg::rdx, 16); // fvl = 1 if fv
    }

    // The 40-bit a + b or a - b of AddSub, worked out with both moved up by 24 bits, where the
    // carry (fc0) and the signed overflow (fv) of bit 39 are those of the 64-bit operation. `a`
    // gets the result, still moved up, and the low bytes of `carry` and `overflow`, which the
    // caller zeroes beforehand, fc0 and fv. `b` is clobbered. With `a_moved`, `a` is already
    // moved up (an earlier result).
    void AddSubShifted(Reg a, Reg b, bool sub, Reg carry, Reg overflow, bool a_moved = false) {
        if (!a_moved) {
            e.ShiftImm(Shift::shl, a, 24, 64);
        }
        e.ShiftImm(Shift::shl, b, 24, 64);
        e.Op(sub ? Arith::sub : Arith::add, a, b, 64);
        e.Set(CC::b, carry);
        e.Set(CC::o, overflow);
    }

    // ProductToBus40: product `unit` with its shift, into dst. Clobbers rcx.
    void ProductToBus40(Reg dst, unsigned unit) {
        e.Mov(dst, R(modes.p[unit]), 32);
        e.Movzx(Reg::rcx, R(modes.pe[unit]), 16);
        e.ShiftImm(Shift::shl, Reg::rcx, 32, 64);
        e.Op(Arith::or_, dst, Reg::rcx, 64);
        switch (Mode(modes.ps[unit])) {
        case 0: // SignExtend<33>
            e.ShiftImm(Shift::shl, dst, 31, 64);
            e.ShiftImm(Shift::sar, dst, 31, 64);
            break;
        case 1: // >> 1, SignExtend<32>
            e.ShiftImm(Shift::shr, dst, 1, 64);
            e.Movsx(dst, dst, 32, 64);
            break;
        case 2: // << 1, SignExtend<34>
            e.ShiftImm(Shift::shl, dst, 31, 64);
            e.ShiftImm(Shift::sar, dst, 30, 64);
            break;
        default: // << 2, SignExtend<35>
            e.ShiftImm(Shift::shl, dst, 31, 64);
            e.ShiftImm(Shift::sar, dst, 29, 64);
            break;
        }
    }

    // ProductFromBus32: p = value, pe = value >> 31.
    void ProductFromBus32(unsigned unit, Reg value) {
        e.Mov(R(modes.p[unit]), value, 32);
        e.ShiftImm(Shift::shr, value, 31, 32);
        e.Mov(R(modes.pe[unit]), value, 16);
    }

    void ClearProduct(unsigned unit) {
        e.MovImm(R(modes.p[unit]), 0, 32);
        e.MovImm(R(modes.pe[unit]), 0, 16);
    }

    // DoMultiplication(unit, x_sign, y_sign). Clobbers rax, rcx, rdx.
    void DoMultiplication(unsigned unit, bool x_sign, bool y_sign) {
        if (x_sign) {
            e.Movsx(Reg::rax, R(modes.x[unit]), 16, 32);
        } else {
            e.Movzx(Reg::rax, R(modes.x[unit]), 16);
        }
        e.Movzx(Reg::rcx, R(modes.y[unit]), 16);
        const u16 hwm = Mode(modes.hwm);
        if (hwm == 1 || (hwm == 3 && unit == 0)) {
            e.ShiftImm(Shift::shr, Reg::rcx, 8, 32);
        } else if (hwm == 2 || (hwm == 3 && unit == 1)) {
            e.OpImm(Arith::and_, Reg::rcx, 0xFF, 32);
        }
        if (y_sign) {
            e.Movsx(Reg::rcx, Reg::rcx, 16, 32);
        }
        e.Imul(Reg::rax, Reg::rcx, 32);
        e.Mov(R(modes.p[unit]), Reg::rax, 32);
        if (x_sign || y_sign) {
            e.ShiftImm(Shift::shr, Reg::rax, 31, 32);
            e.Mov(R(modes.pe[unit]), Reg::rax, 16);
        } else {
            e.MovImm(R(modes.pe[unit]), 0, 16);
        }
    }

    // ProductSum. Clobbers rax, rcx, rdx, rsi, rdi and r8 to r10.
    void ProductSum(SumBase base, RegName acc, bool sub_p0, bool p0_align, bool sub_p1,
                    bool p1_align) {
        // value_a in rsi, value_b in rdi (both products, shifted), value_c in rax
        ProductToBus40(Reg::rsi, 0);
        ProductToBus40(Reg::rdi, 1);
        if (p0_align) { // SignExtend<24>(value >> 16)
            e.ShiftImm(Shift::sar, Reg::rsi, 16, 64);
            e.ShiftImm(Shift::shl, Reg::rsi, 40, 64);
            e.ShiftImm(Shift::sar, Reg::rsi, 40, 64);
        }
        if (p1_align) {
            e.ShiftImm(Shift::sar, Reg::rdi, 16, 64);
            e.ShiftImm(Shift::shl, Reg::rdi, 40, 64);
            e.ShiftImm(Shift::sar, Reg::rdi, 40, 64);
        }
        switch (base) {
        case SumBase::Zero:
            e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
            break;
        case SumBase::Acc:
            LoadAcc(Reg::rax, acc);
            break;
        case SumBase::Sv:
        case SumBase::SvRnd:
            // SignExtend<32>((u64)sv << 16), with | 0x8000 for SvRnd
            e.Movzx(Reg::rax, R(modes.sv), 16);
            e.ShiftImm(Shift::shl, Reg::rax, 16, 32);
            e.Movsx(Reg::rax, Reg::rax, 32, 64);
            if (base == SumBase::SvRnd) {
                e.OpImm(Arith::or_, Reg::rax, 0x8000, 64);
            }
            break;
        }
        // result = AddSub(value_c, value_a, sub_p0), with its fc0 and fv in r8 and r9, then
        // AddSub(result, value_b, sub_p1), with them in rcx and rdx. The result stays moved up
        // between the two, as the second takes its low 40 bits.
        e.Op(Arith::xor_, Reg::r8, Reg::r8, 32);
        e.Op(Arith::xor_, Reg::r9, Reg::r9, 32);
        e.Op(Arith::xor_, Reg::rcx, Reg::rcx, 32);
        e.Op(Arith::xor_, Reg::rdx, Reg::rdx, 32);
        AddSubShifted(Reg::rax, Reg::rsi, sub_p0, Reg::r8, Reg::r9);
        AddSubShifted(Reg::rax, Reg::rdi, sub_p1, Reg::rcx, Reg::rdx, true);
        e.ShiftImm(Shift::sar, Reg::rax, 24, 64); // SignExtend<40>(result)
        // fvl = 1 if either fv was; fc0 and fv of the two combined
        e.Mov(Reg::rsi, Reg::rdx, 32);
        e.Op(Arith::or_, Reg::rsi, Reg::r9, 32);
        e.Op(Arith::or_, R(modes.fvl), Reg::rsi, 16);
        const Arith combine = sub_p0 == sub_p1 ? Arith::or_ : Arith::xor_;
        e.Op(combine, Reg::rcx, Reg::r8, 32);
        e.Op(combine, Reg::rdx, Reg::r9, 32);
        e.Mov(R(modes.fc0), Reg::rcx, 16);
        e.Mov(R(modes.fv), Reg::rdx, 16);
        SatAndSetAccAndFlag(acc, Reg::rax);
    }

    // ------------------------------------------------------------------ ALU operations

    static bool AlmSupported(AlmOp op) {
        switch (op) {
        case AlmOp::Msu:
        case AlmOp::Sqr:
        case AlmOp::Sqra:
        case AlmOp::Or:
        case AlmOp::And:
        case AlmOp::Xor:
        case AlmOp::Tst0:
        case AlmOp::Tst1:
        case AlmOp::Cmp:
        case AlmOp::Cmpu:
        case AlmOp::Sub:
        case AlmOp::Subl:
        case AlmOp::Subh:
        case AlmOp::Add:
        case AlmOp::Addl:
        case AlmOp::Addh:
            return true;
        default:
            return false;
        }
    }

    // ExtendOperandForAlm on the zero-extended 16-bit value in `value`.
    void ExtendOperandForAlm(Reg value, AlmOp op) {
        switch (op) {
        case AlmOp::Cmp:
        case AlmOp::Sub:
        case AlmOp::Add:
            e.Movsx(value, value, 16, 64);
            break;
        case AlmOp::Addh:
        case AlmOp::Subh:
            e.ShiftImm(Shift::shl, value, 16, 32);
            e.Movsx(value, value, 32, 64);
            break;
        default:
            break;
        }
    }

    // AlmGeneric with the operand in `a` (a 64-bit value). Clobbers rax to r10 but r11... see
    // below.
    void AlmGeneric(AlmOp op, Reg a, RegName b) {
        switch (op) {
        case AlmOp::Or:
        case AlmOp::And:
        case AlmOp::Xor: {
            LoadAcc(Reg::rax, b);
            e.Op(op == AlmOp::Or    ? Arith::or_
                 : op == AlmOp::And ? Arith::and_
                                    : Arith::xor_,
                 Reg::rax, a, 64);
            e.ShiftImm(Shift::shl, Reg::rax, 24, 64); // SignExtend<40>
            e.ShiftImm(Shift::sar, Reg::rax, 24, 64);
            SetAccAndFlag(b, Reg::rax);
            break;
        }
        case AlmOp::Tst0:
        case AlmOp::Tst1: {
            ComputeFlags(); // it sets fz; only the operand (rsi) is kept
            ASSERT(a == Reg::rsi);
            // fz = ((acc & 0xFFFF) & a) == 0, or & ~a for Tst1
            e.Movzx(Reg::rax, Acc(b), 16);
            if (op == AlmOp::Tst1) {
                e.Not(a, 64);
            }
            e.Op(Arith::xor_, Reg::rcx, Reg::rcx, 32);
            e.Test(Reg::rax, a, 64);
            e.Set(CC::e, Reg::rcx);
            e.Mov(R(modes.fz), Reg::rcx, 16);
            break;
        }
        case AlmOp::Msu:
        case AlmOp::Sqra: {
            // acc -= p0 (Msu) or += p0 (Sqra), then x0 (and y0 for Sqra) = a, and x0 * y0
            e.Push(a); // the operand survives the sum on the stack
            LoadAcc(Reg::rax, b);
            ProductToBus40(Reg::rsi, 0);
            AddSub(Reg::rax, Reg::rsi, op == AlmOp::Msu);
            SatAndSetAccAndFlag(b, Reg::rax);
            e.Pop(Reg::rsi);
            e.Mov(R(modes.x[0]), Reg::rsi, 16);
            if (op == AlmOp::Sqra) {
                e.Mov(R(modes.y[0]), Reg::rsi, 16);
            }
            DoMultiplication(0, true, true);
            break;
        }
        case AlmOp::Sqr: {
            e.Mov(R(modes.x[0]), a, 16);
            e.Mov(R(modes.y[0]), a, 16);
            DoMultiplication(0, true, true);
            break;
        }
        default: {
            const bool sub = !(op == AlmOp::Add || op == AlmOp::Addl || op == AlmOp::Addh);
            LoadAcc(Reg::rax, b);
            AddSub(Reg::rax, a, sub);
            if (op == AlmOp::Cmp || op == AlmOp::Cmpu) {
                SetAccFlag(Reg::rax);
            } else {
                SatAndSetAccAndFlag(b, Reg::rax);
            }
            break;
        }
        }
    }

    bool Or(RegName a, RegName b, RegName c) {
        LoadAcc(Reg::rax, a);
        LoadAcc(Reg::rsi, b);
        e.Op(Arith::or_, Reg::rax, Reg::rsi, 64);
        SetAccAndFlag(c, Reg::rax);
        return true;
    }

    // ConditionPass(cond): jumps to `skip` if the condition doesn't hold. Returns false for a
    // condition it can't test.
    bool JumpUnlessCondition(Cond cond, Label& skip) {
        if (cond.GetName() != CondValue::True) {
            // It comes first in the instruction, when nothing is kept in registers. The flags are
            // computed on both ways past the condition (EndConditional).
            ComputeFlags();
        }
        const auto require = [&](const u16& flag, u16 value) {
            e.OpImm(Arith::cmp, R(flag), value, 16);
            e.J(CC::ne, skip);
        };
        switch (cond.GetName()) {
        case CondValue::True:
            return true;
        case CondValue::Eq:
            require(modes.fz, 1);
            return true;
        case CondValue::Neq:
            require(modes.fz, 0);
            return true;
        case CondValue::Gt:
            require(modes.fz, 0);
            require(modes.fm, 0);
            return true;
        case CondValue::Ge:
            require(modes.fm, 0);
            return true;
        case CondValue::Lt:
            require(modes.fm, 1);
            return true;
        case CondValue::Le: {
            Label pass;
            e.OpImm(Arith::cmp, R(modes.fm), 1, 16);
            e.J(CC::e, pass);
            require(modes.fz, 1);
            e.Bind(pass);
            return true;
        }
        case CondValue::Nn:
            require(modes.fn, 0);
            return true;
        case CondValue::C:
            require(modes.fc0, 1);
            return true;
        case CondValue::V:
            require(modes.fv, 1);
            return true;
        case CondValue::E:
            require(modes.fe, 1);
            return true;
        case CondValue::L: {
            Label pass;
            e.OpImm(Arith::cmp, R(modes.flm), 1, 16);
            e.J(CC::e, pass);
            require(modes.fvl, 1);
            e.Bind(pass);
            return true;
        }
        case CondValue::Nr:
            require(modes.fr, 0);
            return true;
        case CondValue::Niu0:
            require(modes.iu[0], 0);
            return true;
        case CondValue::Iu0:
            require(modes.iu[0], 1);
            return true;
        case CondValue::Iu1:
            require(modes.iu[1], 1);
            return true;
        default:
            return false;
        }
    }

    bool Moda(ModaOp op, RegName a, Cond cond) {
        switch (op) {
        case ModaOp::Shr:
        case ModaOp::Shr4:
        case ModaOp::Shl:
        case ModaOp::Shl4:
        case ModaOp::Clr:
        case ModaOp::Not:
        case ModaOp::Rnd:
        case ModaOp::Clrr:
        case ModaOp::Inc:
        case ModaOp::Dec:
        case ModaOp::Copy:
        case ModaOp::Neg:
        case ModaOp::Ror:
        case ModaOp::Rol:
        case ModaOp::Pacr:
            break;
        default:
            return Refuse();
        }
        Label skip;
        if (!JumpUnlessCondition(cond, skip)) {
            return Refuse();
        }
        switch (op) {
        case ModaOp::Shr:
            LoadAcc(Reg::rax, a);
            ShiftBus40Constant(Reg::rax, 0xFFFF, a);
            break;
        case ModaOp::Shr4:
            LoadAcc(Reg::rax, a);
            ShiftBus40Constant(Reg::rax, 0xFFFC, a);
            break;
        case ModaOp::Shl:
            LoadAcc(Reg::rax, a);
            ShiftBus40Constant(Reg::rax, 1, a);
            break;
        case ModaOp::Shl4:
            LoadAcc(Reg::rax, a);
            ShiftBus40Constant(Reg::rax, 4, a);
            break;
        case ModaOp::Clr:
            e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
            SatAndSetAccAndFlag(a, Reg::rax);
            break;
        case ModaOp::Not:
            LoadAcc(Reg::rax, a);
            e.Not(Reg::rax, 64);
            SetAccAndFlag(a, Reg::rax);
            break;
        case ModaOp::Rnd:
            LoadAcc(Reg::rax, a);
            e.MovImm(Reg::rsi, 0x8000);
            AddSub(Reg::rax, Reg::rsi, false);
            SatAndSetAccAndFlag(a, Reg::rax);
            break;
        case ModaOp::Clrr:
            e.MovImm(Reg::rax, 0x8000);
            SatAndSetAccAndFlag(a, Reg::rax);
            break;
        case ModaOp::Inc:
        case ModaOp::Dec:
            LoadAcc(Reg::rax, a);
            e.MovImm(Reg::rsi, 1);
            AddSub(Reg::rax, Reg::rsi, op == ModaOp::Dec);
            SatAndSetAccAndFlag(a, Reg::rax);
            break;
        case ModaOp::Copy:
            LoadAcc(Reg::rax, a == RegName::a0 ? RegName::a1 : RegName::a0);
            SatAndSetAccAndFlag(a, Reg::rax);
            break;
        case ModaOp::Neg: {
            // fc0 = value != 0; fv = value == 0xFFFF'FF80'0000'0000 (and fvl); value = -value
            LoadAcc(Reg::rax, a);
            e.Op(Arith::xor_, Reg::rcx, Reg::rcx, 32);
            e.Test(Reg::rax, Reg::rax, 64);
            e.Set(CC::ne, Reg::rcx);
            e.Mov(R(modes.fc0), Reg::rcx, 16);
            e.MovImm(Reg::rdx, 0xFFFF'FF80'0000'0000);
            e.Op(Arith::xor_, Reg::rcx, Reg::rcx, 32);
            e.Op(Arith::cmp, Reg::rax, Reg::rdx, 64);
            e.Set(CC::e, Reg::rcx);
            e.Mov(R(modes.fv), Reg::rcx, 16);
            e.Op(Arith::or_, R(modes.fvl), Reg::rcx, 16);
            e.Neg(Reg::rax, 64); // ~value + 1
            e.ShiftImm(Shift::shl, Reg::rax, 24, 64);
            e.ShiftImm(Shift::sar, Reg::rax, 24, 64);
            SatAndSetAccAndFlag(a, Reg::rax);
            break;
        }
        case ModaOp::Ror: {
            // value (40 bits) >> 1 with the old fc0 into bit 39; fc0 = the bit shifted out
            LoadAcc(Reg::rax, a);
            e.MovImm(Reg::rcx, 0xFF'FFFF'FFFF);
            e.Op(Arith::and_, Reg::rax, Reg::rcx, 64);
            e.Movzx(Reg::rsi, R(modes.fc0), 16);
            e.Mov(Reg::rcx, Reg::rax, 32);
            e.OpImm(Arith::and_, Reg::rcx, 1, 32);
            e.Mov(R(modes.fc0), Reg::rcx, 16);
            e.ShiftImm(Shift::shr, Reg::rax, 1, 64);
            e.ShiftImm(Shift::shl, Reg::rsi, 39, 64);
            e.Op(Arith::or_, Reg::rax, Reg::rsi, 64);
            e.ShiftImm(Shift::shl, Reg::rax, 24, 64);
            e.ShiftImm(Shift::sar, Reg::rax, 24, 64);
            SetAccAndFlag(a, Reg::rax);
            break;
        }
        case ModaOp::Rol: {
            // value << 1 with the old fc0 into bit 0; fc0 = bit 39
            LoadAcc(Reg::rax, a);
            e.Movzx(Reg::rsi, R(modes.fc0), 16);
            e.Mov(Reg::rcx, Reg::rax, 64);
            e.ShiftImm(Shift::shr, Reg::rcx, 39, 64);
            e.OpImm(Arith::and_, Reg::rcx, 1, 32);
            e.Mov(R(modes.fc0), Reg::rcx, 16);
            e.ShiftImm(Shift::shl, Reg::rax, 1, 64);
            e.Op(Arith::or_, Reg::rax, Reg::rsi, 64);
            e.ShiftImm(Shift::shl, Reg::rax, 24, 64);
            e.ShiftImm(Shift::sar, Reg::rax, 24, 64);
            SetAccAndFlag(a, Reg::rax);
            break;
        }
        case ModaOp::Pacr:
            ProductToBus40(Reg::rax, 0);
            e.MovImm(Reg::rsi, 0x8000);
            AddSub(Reg::rax, Reg::rsi, false);
            SatAndSetAccAndFlag(a, Reg::rax);
            break;
        default:
            UNREACHABLE();
        }
        EndConditional(skip);
        return true;
    }

    // ShiftBus40 by a constant amount (the 16-bit two's complement sv), value in rax.
    void ShiftBus40Constant(Reg value, u16 sv, RegName dest) {
        ASSERT(value == Reg::rax);
        const bool logic = Mode(modes.s) != 0;
        // value &= 0xFF'FFFF'FFFF; original_sign (bit 39) in rsi
        e.MovImm(Reg::rcx, 0xFF'FFFF'FFFF);
        e.Op(Arith::and_, Reg::rax, Reg::rcx, 64);
        e.Mov(Reg::rsi, Reg::rax, 64);
        e.ShiftImm(Shift::shr, Reg::rsi, 39, 64);
        // rdi = fv as this shift sets it (when !logic)
        if ((sv >> 15) == 0) {
            if (sv >= 40) {
                if (!logic) {
                    e.Op(Arith::xor_, Reg::rdi, Reg::rdi, 32);
                    e.Test(Reg::rax, Reg::rax, 64);
                    e.Set(CC::ne, Reg::rdi);
                    e.Mov(R(modes.fv), Reg::rdi, 16);
                    e.Op(Arith::or_, R(modes.fvl), Reg::rdi, 16);
                }
                e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
                e.MovImm(R(modes.fc0), 0, 16);
            } else {
                if (!logic) {
                    // fv = SignExtend<40>(value) != SignExtend(value, 40 - sv)
                    e.Mov(Reg::rcx, Reg::rax, 64);
                    e.ShiftImm(Shift::shl, Reg::rcx, 24, 64);
                    e.ShiftImm(Shift::sar, Reg::rcx, 24, 64);
                    e.Mov(Reg::rdx, Reg::rax, 64);
                    const u8 keep = static_cast<u8>(64 - (40 - sv));
                    if (keep != 0) {
                        e.ShiftImm(Shift::shl, Reg::rdx, keep, 64);
                        e.ShiftImm(Shift::sar, Reg::rdx, keep, 64);
                    }
                    e.Op(Arith::xor_, Reg::rdi, Reg::rdi, 32);
                    e.Op(Arith::cmp, Reg::rcx, Reg::rdx, 64);
                    e.Set(CC::ne, Reg::rdi);
                    e.Mov(R(modes.fv), Reg::rdi, 16);
                    e.Op(Arith::or_, R(modes.fvl), Reg::rdi, 16);
                }
                if (sv != 0) {
                    e.ShiftImm(Shift::shl, Reg::rax, static_cast<u8>(sv), 64);
                }
                // fc0 = bit 40
                e.Mov(Reg::rcx, Reg::rax, 64);
                e.ShiftImm(Shift::shr, Reg::rcx, 40, 64);
                e.OpImm(Arith::and_, Reg::rcx, 1, 32);
                e.Mov(R(modes.fc0), Reg::rcx, 16);
            }
        } else {
            const u16 nsv = static_cast<u16>(~sv + 1);
            if (nsv >= 40) {
                if (!logic) {
                    // fc0 = bit 39; value = fc0 ? 0xFF'FFFF'FFFF : 0
                    e.Mov(R(modes.fc0), Reg::rsi, 16);
                    e.Mov(Reg::rax, Reg::rsi, 64);
                    e.Neg(Reg::rax, 64);
                    e.MovImm(Reg::rcx, 0xFF'FFFF'FFFF);
                    e.Op(Arith::and_, Reg::rax, Reg::rcx, 64);
                } else {
                    e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
                    e.MovImm(R(modes.fc0), 0, 16);
                }
            } else {
                // fc0 = bit nsv - 1
                e.Mov(Reg::rcx, Reg::rax, 64);
                if (nsv - 1 != 0) {
                    e.ShiftImm(Shift::shr, Reg::rcx, static_cast<u8>(nsv - 1), 64);
                }
                e.OpImm(Arith::and_, Reg::rcx, 1, 32);
                e.Mov(R(modes.fc0), Reg::rcx, 16);
                e.ShiftImm(Shift::shr, Reg::rax, static_cast<u8>(nsv), 64);
                if (!logic) { // SignExtend(value, 40 - nsv)
                    const u8 keep = static_cast<u8>(64 - (40 - nsv));
                    e.ShiftImm(Shift::shl, Reg::rax, keep, 64);
                    e.ShiftImm(Shift::sar, Reg::rax, keep, 64);
                }
            }
            if (!logic) {
                e.MovImm(R(modes.fv), 0, 16);
                e.Op(Arith::xor_, Reg::rdi, Reg::rdi, 32);
            }
        }
        ShiftBus40Finish(logic);
        StoreAcc(dest, Reg::rax);
    }

    // The end of ShiftBus40, with the value in rax, the original sign in rsi and (unless logic)
    // this shift's fv in rdi.
    void ShiftBus40Finish(bool logic) {
        // value = SignExtend<40>(value)
        e.ShiftImm(Shift::shl, Reg::rax, 24, 64);
        e.ShiftImm(Shift::sar, Reg::rax, 24, 64);
        SetAccFlag(Reg::rax);
        if (!logic && !Mode(modes.sata)) {
            // if fv || SignExtend<32>(value) != value
            Label done;
            e.Movsx(Reg::r8, Reg::rax, 32, 64);
            e.Op(Arith::cmp, Reg::r8, Reg::rax, 64);
            e.Set(CC::ne, Reg::r8);
            e.Movzx(Reg::r8, Reg::r8, 8);
            e.Op(Arith::or_, Reg::r8, Reg::rdi, 32);
            e.J(CC::e, done);
            e.MovImm(R(modes.flm), 1, 16);
            e.MovImm(Reg::rax, 0x7FFF'FFFF);
            e.MovImm(Reg::rcx, 0xFFFF'FFFF'8000'0000);
            e.Test(Reg::rsi, Reg::rsi, 32);
            e.Cmov(CC::ne, Reg::rax, Reg::rcx, 64);
            e.Bind(done);
        }
    }

    // ShiftBus40 by regs.sv, which the firmware changes as data, so the code handles any amount.
    // The value is in rax.
    void ShiftBus40Variable(Reg value, RegName dest) {
        ASSERT(value == Reg::rax);
        const bool logic = Mode(modes.s) != 0;
        Label right, left_big, left_done, right_big, right_done, finish;
        e.MovImm(Reg::rcx, 0xFF'FFFF'FFFF);
        e.Op(Arith::and_, Reg::rax, Reg::rcx, 64);
        e.Mov(Reg::rsi, Reg::rax, 64);
        e.ShiftImm(Shift::shr, Reg::rsi, 39, 64);  // original sign
        e.Op(Arith::xor_, Reg::rdi, Reg::rdi, 32); // fv
        e.Movzx(Reg::r9, R(modes.sv), 16);
        e.TestImm(Reg::r9, 0x8000, 32);
        e.J(CC::ne, right);
        // left shift by r9
        e.OpImm(Arith::cmp, Reg::r9, 40, 32);
        e.J(CC::ae, left_big);
        if (!logic) {
            // fv = SignExtend<40>(value) != SignExtend(value, 40 - sv): the bits from 39 - sv up
            // aren't all equal. (value << (24 + sv)) >> (24 + sv) (arithmetic) against
            // (value << 24) >> 24.
            e.Mov(Reg::rdx, Reg::rax, 64);
            e.ShiftImm(Shift::shl, Reg::rdx, 24, 64);
            e.ShiftImm(Shift::sar, Reg::rdx, 24, 64);
            e.Lea(Reg::rcx, Ptr(Reg::r9, 24), 32);
            e.Mov(Reg::r10, Reg::rax, 64);
            e.ShiftCl(Shift::shl, Reg::r10, 64);
            e.ShiftCl(Shift::sar, Reg::r10, 64);
            e.Op(Arith::cmp, Reg::rdx, Reg::r10, 64);
            e.Set(CC::ne, Reg::rdi);
            e.Mov(R(modes.fv), Reg::rdi, 16);
            e.Op(Arith::or_, R(modes.fvl), Reg::rdi, 16);
        }
        e.Mov(Reg::rcx, Reg::r9, 32);
        e.ShiftCl(Shift::shl, Reg::rax, 64);
        e.Mov(Reg::rcx, Reg::rax, 64);
        e.ShiftImm(Shift::shr, Reg::rcx, 40, 64);
        e.OpImm(Arith::and_, Reg::rcx, 1, 32);
        e.Mov(R(modes.fc0), Reg::rcx, 16);
        e.Jmp(finish);
        e.Bind(left_big);
        if (!logic) {
            e.Test(Reg::rax, Reg::rax, 64);
            e.Set(CC::ne, Reg::rdi);
            e.Mov(R(modes.fv), Reg::rdi, 16);
            e.Op(Arith::or_, R(modes.fvl), Reg::rdi, 16);
        }
        e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
        e.MovImm(R(modes.fc0), 0, 16);
        e.Jmp(finish);
        // right shift by nsv = -sv (16 bits)
        e.Bind(right);
        e.Neg(Reg::r9, 32);
        e.OpImm(Arith::and_, Reg::r9, 0xFFFF, 32);
        e.OpImm(Arith::cmp, Reg::r9, 40, 32);
        e.J(CC::ae, right_big);
        e.Lea(Reg::rcx, Ptr(Reg::r9, -1), 32);
        e.Mov(Reg::rdx, Reg::rax, 64);
        e.ShiftCl(Shift::shr, Reg::rdx, 64);
        e.OpImm(Arith::and_, Reg::rdx, 1, 32);
        e.Mov(R(modes.fc0), Reg::rdx, 16);
        e.Mov(Reg::rcx, Reg::r9, 32);
        e.ShiftCl(Shift::shr, Reg::rax, 64);
        if (!logic) {
            // SignExtend(value, 40 - nsv): shift left and back by 24 + nsv
            e.Lea(Reg::rcx, Ptr(Reg::r9, 24), 32);
            e.ShiftCl(Shift::shl, Reg::rax, 64);
            e.ShiftCl(Shift::sar, Reg::rax, 64);
        }
        e.Jmp(right_done);
        e.Bind(right_big);
        if (!logic) {
            e.Mov(R(modes.fc0), Reg::rsi, 16);
            e.Mov(Reg::rax, Reg::rsi, 64);
            e.Neg(Reg::rax, 64);
            e.MovImm(Reg::rcx, 0xFF'FFFF'FFFF);
            e.Op(Arith::and_, Reg::rax, Reg::rcx, 64);
        } else {
            e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
            e.MovImm(R(modes.fc0), 0, 16);
        }
        e.Bind(right_done);
        if (!logic) {
            e.MovImm(R(modes.fv), 0, 16);
            e.Op(Arith::xor_, Reg::rdi, Reg::rdi, 32);
        }
        e.Bind(finish);
        ShiftBus40Finish(logic);
        StoreAcc(dest, Reg::rax);
    }

    // ------------------------------------------------------------------ Addresses and memory

    // RnAndModify: the unit register's value into `old_value` and its stepped value into
    // `new_value`, both zero-extended 16 bits. The register itself isn't written: the caller
    // stores `new_value` once the instruction can't stop. The modes are baked in. Clobbers rdx.
    void RnAndModifyRaw(Reg old_value, Reg new_value, unsigned unit, StepValue step, bool dmod) {
        e.Movzx(old_value, R(modes.r[unit]), 16);
        const bool ep = (unit == 3 && Mode(modes.epi)) || (unit == 7 && Mode(modes.epj));
        if (ep && step != StepValue::Increase2Mode1 && step != StepValue::Decrease2Mode1 &&
            step != StepValue::Increase2Mode2 && step != StepValue::Decrease2Mode2) {
            e.Op(Arith::xor_, new_value, new_value, 32);
        } else {
            StepAddress(new_value, old_value, unit, step, dmod);
        }
    }

    // RnAddress on the value in `value`: bit-reversed if the unit reverses.
    void RnAddress(Reg value, unsigned unit) {
        if (Mode(modes.br[unit]) && !Mode(modes.m[unit])) {
            BitReverse16(value);
        }
    }

    // RnAddressAndModify: as RnAndModifyRaw, with the address the old value gives (RnAddress) in
    // `address`.
    void RnAndModify(Reg address, Reg new_value, unsigned unit, StepValue step, bool dmod) {
        RnAndModifyRaw(address, new_value, unit, step, dmod);
        RnAddress(address, unit);
    }

    // BitReverse on the zero-extended 16 bits in `value`. Clobbers rdx.
    void BitReverse16(Reg value) {
        const struct {
            u8 shift;
            s32 mask;
        } stages[] = {{1, 0x5555}, {2, 0x3333}, {4, 0x0F0F}};
        for (const auto& s : stages) {
            // value = ((value >> s) & mask) | ((value & mask) << s)
            e.Mov(Reg::rdx, value, 32);
            e.ShiftImm(Shift::shr, Reg::rdx, s.shift, 32);
            e.OpImm(Arith::and_, Reg::rdx, s.mask, 32);
            e.OpImm(Arith::and_, value, s.mask, 32);
            e.ShiftImm(Shift::shl, value, s.shift, 32);
            e.Op(Arith::or_, value, Reg::rdx, 32);
        }
        e.ShiftImm(Shift::rol, value, 8, 16); // swap the bytes
        e.Movzx(value, value, 16);
    }

    // StepAddress(unit, address, step, dmod) into dst (16 bits, zero-extended). The modes are
    // baked in.
    void StepAddress(Reg dst, Reg address, unsigned unit, StepValue step, bool dmod) {
        const bool br = Mode(modes.br[unit]) != 0;
        const bool m = Mode(modes.m[unit]) != 0;
        if ((dmod || br || !m) && step != StepValue::PlusStep) {
            static constexpr u16 plain_steps[8] = {0, 1, 0xFFFF, 0, 2, 0xFFFE, 2, 0xFFFE};
            const u16 s = plain_steps[static_cast<unsigned>(step)];
            e.Lea(dst, Ptr(address, static_cast<s16>(s)), 32);
            e.Movzx(dst, dst, 16);
            return;
        }

        u16 s;
        const bool legacy = Mode(modes.cmd) != 0;
        bool step2_mode1 = false;
        bool step2_mode2 = false;
        switch (step) {
        case StepValue::Zero:
            s = 0;
            break;
        case StepValue::Increase:
            s = 1;
            break;
        case StepValue::Decrease:
            s = 0xFFFF;
            break;
        case StepValue::Increase2Mode1:
            s = 2;
            step2_mode1 = !legacy;
            break;
        case StepValue::Decrease2Mode1:
            s = 0xFFFE;
            step2_mode1 = !legacy;
            break;
        case StepValue::Increase2Mode2:
            s = 2;
            step2_mode2 = !legacy;
            break;
        case StepValue::Decrease2Mode2:
            s = 0xFFFE;
            step2_mode2 = !legacy;
            break;
        case StepValue::PlusStep: {
            if (br && !m) {
                s = unit < 4 ? Mode(modes.stepi0) : Mode(modes.stepj0);
            } else {
                s = unit < 4 ? Mode(modes.stepi) : Mode(modes.stepj);
                s = SignExtend<7>(s);
            }
            if (Mode(modes.stp16) == 1 && !legacy) {
                s = unit < 4 ? Mode(modes.stepi0) : Mode(modes.stepj0);
                if (m) {
                    s = SignExtend<9>(s);
                }
            }
            break;
        }
        default:
            UNREACHABLE();
        }

        if (s == 0) {
            e.Mov(dst, address, 32);
            return;
        }

        if (!dmod && !br && m) {
            const u16 mod = unit < 4 ? Mode(modes.modi) : Mode(modes.modj);
            if (mod == 0 || (mod == 1 && step2_mode2)) {
                e.Mov(dst, address, 32);
                return;
            }
            unsigned iteration = 1;
            if (step2_mode1) {
                iteration = 2;
                s = SignExtend<15, u16>(s >> 1);
            }
            e.Mov(dst, address, 32);
            for (unsigned i = 0; i < iteration; ++i) {
                if (legacy || step2_mode2) {
                    const bool negative = (s >> 15) != 0;
                    u16 mm = mod;
                    if (negative) {
                        mm |= static_cast<u16>(~s);
                    } else {
                        mm |= s;
                    }
                    const u16 mask = static_cast<u16>((1u << std20::log2p1(mm)) - 1);
                    // next = special case ? constant : (address + s) & mask
                    Label plain, merged;
                    const bool special_allowed = !step2_mode2 || mod != mask;
                    e.Mov(Reg::rdx, dst, 32);
                    e.OpImm(Arith::and_, Reg::rdx, mask, 32);
                    if (special_allowed) {
                        e.OpImm(Arith::cmp, Reg::rdx, negative ? 0 : mod, 32);
                        e.J(CC::ne, plain);
                        e.MovImm(Reg::rdx, negative ? mod : 0);
                        e.Jmp(merged);
                    }
                    e.Bind(plain);
                    e.Lea(Reg::rdx, Ptr(dst, static_cast<s16>(s)), 32);
                    e.OpImm(Arith::and_, Reg::rdx, mask, 32);
                    e.Bind(merged);
                    e.OpImm(Arith::and_, dst, static_cast<u16>(~mask), 32);
                    e.Op(Arith::or_, dst, Reg::rdx, 32);
                } else {
                    const u16 mask = static_cast<u16>((1u << std20::log2p1(mod)) - 1);
                    if (s < 0x8000) {
                        // next = (address + s) & mask; if next == ((mod + 1) & mask) next = 0
                        Label keep;
                        e.Lea(Reg::rdx, Ptr(dst, s), 32);
                        e.OpImm(Arith::and_, Reg::rdx, mask, 32);
                        e.OpImm(Arith::cmp, Reg::rdx, (mod + 1) & mask, 32);
                        e.J(CC::ne, keep);
                        e.Op(Arith::xor_, Reg::rdx, Reg::rdx, 32);
                        e.Bind(keep);
                    } else {
                        // next = address & mask; if next == 0 next = mod + 1; next = (next + s) &
                        // mask
                        Label nonzero;
                        e.Mov(Reg::rdx, dst, 32);
                        e.OpImm(Arith::and_, Reg::rdx, mask, 32);
                        e.J(CC::ne, nonzero);
                        e.MovImm(Reg::rdx, static_cast<u16>(mod + 1));
                        e.Bind(nonzero);
                        e.Lea(Reg::rdx, Ptr(Reg::rdx, static_cast<s16>(s)), 32);
                        e.OpImm(Arith::and_, Reg::rdx, mask, 32);
                    }
                    e.OpImm(Arith::and_, dst, static_cast<u16>(~mask), 32);
                    e.Op(Arith::or_, dst, Reg::rdx, 32);
                }
            }
            e.Movzx(dst, dst, 16);
            return;
        }

        e.Lea(dst, Ptr(address, static_cast<s16>(s)), 32);
        e.Movzx(dst, dst, 16);
    }

    bool OffsetSupported(unsigned unit, OffsetValue offset, bool dmod) {
        if (offset != OffsetValue::MinusOne) {
            return true;
        }
        // The interpreter throws for MinusOne with modulo arithmetic.
        const bool emod = Mode(modes.m[unit]) && !Mode(modes.br[unit]) && !dmod;
        return !emod;
    }

    // OffsetAddress(unit, address, offset, dmod) into dst (16 bits, zero-extended).
    void OffsetAddress(Reg dst, Reg address, unsigned unit, OffsetValue offset, bool dmod) {
        if (offset == OffsetValue::Zero) {
            e.Mov(dst, address, 32);
            return;
        }
        if (offset == OffsetValue::MinusOneDmod) {
            e.Lea(dst, Ptr(address, -1), 32);
            e.Movzx(dst, dst, 16);
            return;
        }
        const bool emod = Mode(modes.m[unit]) && !Mode(modes.br[unit]) && !dmod;
        if (!emod) {
            e.Lea(dst, Ptr(address, offset == OffsetValue::PlusOne ? 1 : -1), 32);
            e.Movzx(dst, dst, 16);
            return;
        }
        ASSERT(offset == OffsetValue::PlusOne);
        const u16 mod = unit < 4 ? Mode(modes.modi) : Mode(modes.modj);
        u16 mask = 1;
        for (unsigned i = 0; i < 9; ++i) {
            mask |= mod >> i;
        }
        // (address & mask) == mod ? address & ~mask : address + 1
        Label plain, done;
        e.Mov(dst, address, 32);
        e.OpImm(Arith::and_, dst, mask, 32);
        e.OpImm(Arith::cmp, dst, mod, 32);
        e.J(CC::ne, plain);
        e.Mov(dst, address, 32);
        e.OpImm(Arith::and_, dst, static_cast<u16>(~mask), 32);
        e.Jmp(done);
        e.Bind(plain);
        e.Lea(dst, Ptr(address, 1), 32);
        e.Movzx(dst, dst, 16);
        e.Bind(done);
    }

    // The address of a [page:imm8] operand into dst.
    void PageAddress(Reg dst, MemImm8 a) {
        e.Movzx(dst, R(modes.page), 16);
        e.ShiftImm(Shift::shl, dst, 8, 32);
        e.OpImm(Arith::add, dst, a.Unsigned16(), 32);
        e.Movzx(dst, dst, 16);
    }

    // Stops the block before this instruction if the data address in `address` (16 bits,
    // zero-extended) is an MMIO register, or one the memory interface would refuse.
    void CheckData(Reg address) {
        const u16 base = MiuMode(miu.mmio_base);
        const u32 end = std::min<u32>(u32{base} + MemoryInterfaceUnit::MMIOSize, 0x10000);
        if (end > base) {
            Label outside;
            e.OpImm(Arith::cmp, address, base, 32);
            e.J(CC::b, outside);
            e.OpImm(Arith::cmp, address, static_cast<s32>(end), 32);
            e.J(CC::b, *bail);
            e.Bind(outside);
        }
    }

    // Calls emit(base) with the register that holds the data page the address in `address` is
    // in (ConvertDataAddress): the page mode and, in page mode 1, the size of the X page are baked
    // in; the pages themselves are read on entry to the block (XPage, YPage).
    template <typename F>
    void ForDataPage(Reg address, F&& emit) {
        if (MiuMode(miu.page_mode) == 0) {
            emit(XPage);
            return;
        }
        const u32 x_limit = u32{MiuMode(miu.x_size[0])} * MemoryInterfaceUnit::XYSizeResolution;
        if (x_limit >= 0xFFFF) {
            emit(XPage);
            return;
        }
        Label y, done;
        e.OpImm(Arith::cmp, address, static_cast<s32>(x_limit), 32);
        e.J(CC::a, y);
        emit(XPage);
        e.Jmp(done);
        e.Bind(y);
        emit(YPage);
        e.Bind(done);
    }

    void LoadData(Reg dst, Reg address) {
        ForDataPage(address, [&](Reg base) { e.Movzx(dst, Ptr(base, address, 2), 16); });
    }

    void StoreData(Reg address, Reg value) {
        ForDataPage(address, [&](Reg base) { e.Mov(Ptr(base, address, 2), value, 16); });
    }

    Emitter& e;
    const RegisterState& modes;
    const MemoryInterfaceUnit& miu;
    const MemoryInterfaceUnit* live_miu;
    std::vector<Guard> guards;
    Label* bail = nullptr;
    Refusal refusal = Refusal::None;
    u32 loop_end;
    u32 address = 0;
    u32 next = 0;
    bool sets_pc = false;
    Flags flags = Flags::Computed;
    Jit::Handler handler;
    u16 opcode = 0;
    u16 expansion_word = 0;
    u32 index = 0; // the instruction's position in the block
    Label* epilogue = nullptr;
};

} // namespace

struct Jit::Impl {
    Impl(RegisterState& regs, MemoryInterface& mem)
        : regs(regs), mem(mem), code(CodeSize), table(ProgramWords) {
        const char* setting = std::getenv("TEAKRA_JIT");
        enabled = code.Usable() && !(setting && std::string(setting) == "0");
        stats = std::getenv("TEAKRA_JIT_STATS") != nullptr;
        if (const char* dir = std::getenv("TEAKRA_JIT_DUMP")) {
            dump = dir;
        }
        trace = std::getenv("TEAKRA_JIT_TRACE") != nullptr;
    }

    ~Impl() {
        if (stats) {
            std::fprintf(
                stderr,
                "teakra jit: %llu instructions run, %llu blocks, %llu refused, %llu runs, "
                "%llu calls, %llu validations\n",
                static_cast<unsigned long long>(run), static_cast<unsigned long long>(blocks_made),
                static_cast<unsigned long long>(refused.size()),
                static_cast<unsigned long long>(runs), static_cast<unsigned long long>(calls),
                static_cast<unsigned long long>(validations));
            std::vector<std::pair<std::string, u64>> sorted(refused.begin(), refused.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            for (std::size_t i = 0; i < sorted.size() && i < 40; ++i) {
                std::fprintf(stderr, "  %8llu  %s\n",
                             static_cast<unsigned long long>(sorted[i].second),
                             sorted[i].first.c_str());
            }
            std::vector<std::pair<u32, std::pair<u64, u64>>> blocks(per_block.begin(),
                                                                    per_block.end());
            std::sort(blocks.begin(), blocks.end(),
                      [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
            std::fprintf(stderr, "busiest blocks (calls, instructions per call, length):\n");
            for (std::size_t i = 0; i < blocks.size() && i < 25; ++i) {
                const Block* b = table[blocks[i].first].get();
                std::fprintf(stderr, "  %05x %10llu %6.1f %3u\n", blocks[i].first,
                             static_cast<unsigned long long>(blocks[i].second.first),
                             double(blocks[i].second.second) / double(blocks[i].second.first),
                             b ? b->length : 0);
            }
            std::vector<std::pair<u64, u64>> failures(guard_failures.begin(), guard_failures.end());
            std::sort(failures.begin(), failures.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::fprintf(stderr, "most guard failures (address, field offset, count):\n");
            for (std::size_t i = 0; i < failures.size() && i < 10; ++i) {
                std::fprintf(stderr, "  %05llx %8x %10llu\n",
                             static_cast<unsigned long long>(failures[i].first >> 32),
                             static_cast<unsigned>(failures[i].first & 0xFFFFFFFF),
                             static_cast<unsigned long long>(failures[i].second));
            }
            {
                std::vector<std::pair<u16, u64>> ops(interpreted_opcodes.begin(),
                                                     interpreted_opcodes.end());
                std::sort(ops.begin(), ops.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });
                u64 total = 0;
                for (const auto& o : ops) {
                    total += o.second;
                }
                std::fprintf(stderr,
                             "interpreted between blocks: %llu; after limit %llu, no block "
                             "%llu, budget %llu, guard %llu, refused %llu, bailed %llu, empty "
                             "%llu; by opcode:\n",
                             static_cast<unsigned long long>(total),
                             static_cast<unsigned long long>(stops[0]),
                             static_cast<unsigned long long>(stops[1]),
                             static_cast<unsigned long long>(stops[2]),
                             static_cast<unsigned long long>(stops[3]),
                             static_cast<unsigned long long>(stops[4]),
                             static_cast<unsigned long long>(stops[5]),
                             static_cast<unsigned long long>(stops[6]));
                for (std::size_t i = 0; i < ops.size() && i < 40; ++i) {
                    std::fprintf(stderr, "  %04x %10llu %s\n", ops[i].first,
                                 static_cast<unsigned long long>(ops[i].second),
                                 Decode(ops[i].first).GetName());
                }
            }
            {
                std::vector<std::pair<u32, u64>> ops(interpreted_by_stop.begin(),
                                                     interpreted_by_stop.end());
                std::sort(ops.begin(), ops.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });
                static const char* const names[] = {"limit",   "no block", "budget", "guard",
                                                    "refused", "bailed",   "empty"};
                std::fprintf(stderr, "interpreted by opcode and stop:\n");
                for (std::size_t i = 0; i < ops.size() && i < 60; ++i) {
                    std::fprintf(stderr, "  %04x %10llu %-8s %s\n", ops[i].first >> 8,
                                 static_cast<unsigned long long>(ops[i].second),
                                 names[ops[i].first & 0xFF], Decode(ops[i].first >> 8).GetName());
                }
            }
            std::vector<std::pair<u32, u64>> bails(bails_at.begin(), bails_at.end());
            std::sort(bails.begin(), bails.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::fprintf(stderr, "most bails (address, count):\n");
            for (std::size_t i = 0; i < bails.size() && i < 10; ++i) {
                std::fprintf(stderr, "  %05x %10llu\n", bails[i].first,
                             static_cast<unsigned long long>(bails[i].second));
            }
        }
    }

    u64 Run(u64 limit) {
        u64 done = 0;
        stop = Stop::Limit;
        if (stats) {
            ++runs;
        }
        if (regs.prpage != 0 || (regs.lp && static_cast<u32>(regs.bcn) - 1 >= 4)) {
            // Blocks come from program page 0, which is the one the firmware uses, and are for a
            // valid block-repeat stack.
            stop = Stop::NoBlock;
            return 0;
        }
        while (done < limit) {
            const u32 pc = regs.pc;
            if (pc >= ProgramWords) {
                stop = Stop::NoBlock;
                break;
            }
            const u32 loop_end = regs.lp ? regs.bkrep_stack[regs.bcn - 1].end + 1 : NoLoopEnd;
            Block* block = Find(pc, loop_end);
            if (!block || !block->code) {
                stop = Stop::NoBlock;
                break;
            }
            if (block->length > limit - done) {
                stop = Stop::Budget;
                break;
            }
            context.regs = &regs;
            context.interpreter = interpreter;
            context.memory = mem.Memory();
            context.miu = &mem.Unit();
            context.limit = limit - done;
            context.exit = Exit::Done;
            u64 ran = block->code(&context);
            ++calls;
            if (trace) {
                std::fprintf(stderr,
                             "block %05x (length %u, loop end %05x): ran %llu, exit %u, pc %05x\n",
                             pc, block->length, loop_end, static_cast<unsigned long long>(ran),
                             context.exit, regs.pc);
            }
            if (stats) {
                auto& s = per_block[pc];
                ++s.first;
                s.second += ran;
                if (context.exit == Exit::Bailed) {
                    ++bails_at[regs.pc];
                }
            }
            if (context.exit == Exit::GuardFailed) {
                if (stats) {
                    for (const Guard& g : block->guards) {
                        const u8* field = g.absolute
                                              ? static_cast<const u8*>(g.absolute)
                                              : reinterpret_cast<const u8*>(&regs) + g.offset;
                        u32 value = 0;
                        std::memcpy(&value, field, g.size);
                        if (value != g.value) {
                            ++guard_failures[(u64{pc} << 32) | static_cast<u32>(g.offset) |
                                             (g.absolute ? 0x80000000u : 0)];
                            break;
                        }
                    }
                }
                // Another block from here that suits the modes as they are now, or a new one. Find
                // stopped checking blocks against program memory at this one, so the rest are
                // checked here, and stale ones dropped as there.
                Block* other = nullptr;
                const u64 current = Generation();
                std::unique_ptr<Block>* slot = &block->next;
                while (*slot) {
                    Block* b = slot->get();
                    if (b->checked != current) {
                        ++validations;
                        if (!SourceMatches(*b)) {
                            *slot = std::move(b->next);
                            continue;
                        }
                        b->checked = current;
                    }
                    slot = &b->next;
                    if (b->loop_end != loop_end || !b->code || b->length > limit - done) {
                        continue;
                    }
                    context.exit = Exit::Done;
                    ran = b->code(&context);
                    if (context.exit != Exit::GuardFailed) {
                        other = b;
                        break;
                    }
                }
                if (!other) {
                    other = Compile(pc, loop_end);
                    if (!other || !other->code || other->length > limit - done) {
                        stop = Stop::Guard;
                        break;
                    }
                    context.exit = Exit::Done;
                    ran = other->code(&context);
                    if (context.exit == Exit::GuardFailed) {
                        stop = Stop::Guard;
                        break;
                    }
                } else {
                    MoveToFront(other);
                }
            }
            if (context.exit == Exit::Refused) {
                stop = Stop::Refused;
                break;
            }
            done += ran;
            if (context.exit == Exit::Bailed || ran == 0) {
                stop = context.exit == Exit::Bailed ? Stop::Bailed : Stop::Empty;
                break;
            }
        }
        run += done;
        return done;
    }

    // Why the last Run stopped, for the statistics.
    enum class Stop { Limit, NoBlock, Budget, Guard, Refused, Bailed, Empty, Count };
    Stop stop = Stop::Limit;
    u64 stops[static_cast<int>(Stop::Count)] = {};

    // The generation: it changes whenever program memory has changed, other than by a reset.
    u64 Generation() const {
        return mem.ProgramWrites();
    }

    // The block to run from pc in the loop that ends at loop_end, translating one if needed.
    Block* Find(u32 pc, u32 loop_end) {
        const u64 current = Generation();
        std::unique_ptr<Block>* slot = &table[pc];
        while (*slot) {
            Block* block = slot->get();
            if (block->checked != current) {
                ++validations;
                if (!SourceMatches(*block)) {
                    // Stale: drop it and look further.
                    *slot = std::move(block->next);
                    continue;
                }
                block->checked = current;
            }
            if (block->loop_end == loop_end) {
                return block;
            }
            slot = &block->next;
        }
        return Compile(pc, loop_end);
    }

    // Puts a block first among the blocks from its address, where Find looks first.
    void MoveToFront(Block* block) {
        std::unique_ptr<Block>* slot = &table[block->start];
        while (slot->get() != block) {
            slot = &(*slot)->next;
        }
        std::unique_ptr<Block> taken = std::move(*slot);
        *slot = std::move(taken->next);
        taken->next = std::move(table[block->start]);
        table[block->start] = std::move(taken);
    }

    bool SourceMatches(const Block& block) const {
        const u8* program = mem.ProgramMemory();
        for (std::size_t i = 0; i < block.source.size(); ++i) {
            if (SharedMemory::ReadWord(program, block.start + static_cast<u32>(i)) !=
                block.source[i]) {
                return false;
            }
        }
        return true;
    }

    const Matcher<Translator>& Decode(u16 opcode) {
        auto it = matchers.find(opcode);
        if (it == matchers.end()) {
            it = matchers.emplace(opcode, ::Decode<Translator>(opcode)).first;
        }
        return it->second;
    }

    // Translates a block from pc for the modes as they are now, and puts it first among the
    // blocks from pc. Returns it, or nullptr if there's no room or too many blocks from pc.
    Block* Compile(u32 pc, u32 loop_end) {
        std::size_t variants = 0;
        for (const Block* b = table[pc].get(); b; b = b->next.get()) {
            ++variants;
        }
        if (variants >= MaxVariants) {
            return nullptr;
        }

        auto block = std::make_unique<Block>();
        block->start = pc;
        block->loop_end = loop_end;
        block->checked = Generation();

        const RegisterState modes = regs;
        const MemoryInterfaceUnit miu = mem.Unit();
        const u8* program = mem.ProgramMemory();

        Emitter e;
        Translator t(e, modes, miu, &mem.Unit(), loop_end, handler);
        Label guards_label, top, epilogue;
        t.SetEpilogue(&epilogue);

        // Prologue: save the registers the block uses that callers expect kept, and load the
        // fixed ones.
        const Reg saved[] = {Reg::rbx, Reg::rbp, Reg::r12, Reg::r13,
                             Reg::r14, Reg::r15, Reg::rsi, Reg::rdi};
        for (Reg r : saved) {
            e.Push(r);
        }
        // Calls to handlers need the stack 16-byte aligned, and Win64 32 bytes above the return
        // address: eight pushes after the return address leave it 8 off.
        e.OpImm(Arith::sub, Reg::rsp, 40, 64);
#ifdef _WIN32
        e.Mov(ContextReg, Reg::rcx, 64);
#else
        e.Mov(ContextReg, Reg::rdi, 64);
#endif
        e.Mov(RegsBase, Ptr(ContextReg, offsetof(Context, regs)), 64);
        e.Mov(MemoryBase, Ptr(ContextReg, offsetof(Context, memory)), 64);
        e.Op(Arith::xor_, Executed, Executed, 32);
        // In a loop, the code may jump back to the top with flags pending (Translator::Flags).
        using Flags = Translator::Flags;
        const Flags top_flags = loop_end != NoLoopEnd ? Flags::Either : Flags::Computed;
        t.SetFlagState(top_flags);
        if (top_flags == Flags::Either) {
            e.MovImm(Ptr(ContextReg, offsetof(Context, flags_pending)), 0, 32);
        }
        e.Jmp(guards_label);
        e.Bind(top);

        struct Bail {
            std::unique_ptr<Label> label;
            u32 index;
            u32 address;
            Flags flags; // before the instruction
        };
        std::vector<Bail> bails;

        u32 address = pc;
        u32 count = 0;
        bool loop_back = false;
        bool jumped = false; // the last instruction set regs.pc itself
        while (count < MaxBlockInstructions && address < ProgramWords) {
            const u16 opcode = SharedMemory::ReadWord(program, address);
            const Matcher<Translator>& matcher = Decode(opcode);
            // Instructions that change the flow through a register operand stay with the
            // interpreter; jumps (the translator knows which) end the block.
            if (matcher.UsesFlowRegister(opcode)) {
                break;
            }
            const bool flow = ChangesFlow(matcher.GetName());
            const bool expanded = matcher.NeedExpansion();
            const u16 expansion = expanded ? SharedMemory::ReadWord(program, address + 1) : 0;
            const u32 next = address + (expanded ? 2 : 1);

            auto bail = std::make_unique<Label>();
            const std::size_t code_size = e.Size();
            const std::size_t guard_count = t.Guards().size();
            const Flags flags_before = t.FlagState();
            t.Begin(bail.get(), address, next, opcode, expansion, count);
            if (!matcher.call(t, opcode, expansion)) {
                if (stats) {
                    ++refused[matcher.GetName()];
                }
                t.SetFlagState(flags_before);
                if (count == 0 && t.GetRefusal() == Refusal::Mode) {
                    // Keep the guards and stop at once: under other modes it may translate.
                    e.code.resize(code_size);
                    e.Jmp(*bail);
                    bails.push_back({std::move(bail), count, address, flags_before});
                    block->source.push_back(opcode);
                    if (expanded) {
                        block->source.push_back(expansion);
                    }
                    count = 0;
                    break;
                }
                e.code.resize(code_size);
                t.TruncateGuards(guard_count);
                break;
            }
            bails.push_back({std::move(bail), count, address, flags_before});
            block->source.push_back(opcode);
            if (expanded) {
                block->source.push_back(expansion);
            }
            ++count;
            address = next;
            if (flow || t.SetsPc()) {
                ASSERT(t.SetsPc());
                jumped = true;
                break;
            }
            if (next == loop_end) {
                loop_back = true;
                break;
            }
        }

        if (count == 0 && bails.empty()) {
            // The first instruction can't be translated whatever the modes.
            block->source.clear();
            block->source.push_back(SharedMemory::ReadWord(program, pc));
            block->length = 0;
            return Insert(std::move(block));
        }
        block->length = count;

        // The end of a pass.
        const auto exit_with = [&](u32 exit) {
            if (exit != Exit::Done) {
                e.MovImm(Ptr(ContextReg, offsetof(Context, exit)), static_cast<s32>(exit), 32);
            }
            e.Jmp(epilogue);
        };
        if (count > 0) {
            e.OpImm(Arith::add, Executed, static_cast<s32>(count), 64);
            const Flags end_flags = t.FlagState();
            if (loop_back) {
                // Into the next pass with the flags as they are, which the top reads from
                // flags_pending; out of the block with them computed.
                if (end_flags == Flags::Pending || end_flags == Flags::Computed) {
                    e.MovImm(Ptr(ContextReg, offsetof(Context, flags_pending)),
                             end_flags == Flags::Pending ? 1 : 0, 32);
                }
                // The block ends where the innermost loop does: its last instruction was run, so
                // do what the interpreter does after it (RunOrdinary).
                // rcx = &regs.bkrep_stack[bcn - 1]
                const RegisterState& m = modes;
                const s32 stack = static_cast<s32>(reinterpret_cast<const u8*>(&m.bkrep_stack[0]) -
                                                   reinterpret_cast<const u8*>(&m));
                const s32 frame_size = static_cast<s32>(sizeof(RegisterState::BlockRepeatFrame));
                const s32 start_off =
                    static_cast<s32>(offsetof(RegisterState::BlockRepeatFrame, start));
                const s32 lc_off = static_cast<s32>(offsetof(RegisterState::BlockRepeatFrame, lc));
                const s32 bcn = static_cast<s32>(reinterpret_cast<const u8*>(&m.bcn) -
                                                 reinterpret_cast<const u8*>(&m));
                const s32 lp = static_cast<s32>(reinterpret_cast<const u8*>(&m.lp) -
                                                reinterpret_cast<const u8*>(&m));
                const s32 pc_off = static_cast<s32>(reinterpret_cast<const u8*>(&m.pc) -
                                                    reinterpret_cast<const u8*>(&m));
                Label finished, to_start;
                e.Movzx(Reg::rax, Ptr(RegsBase, bcn), 16);
                e.OpImm(Arith::sub, Reg::rax, 1, 32);
                e.MovImm(Reg::rcx, static_cast<u64>(frame_size));
                e.Imul(Reg::rax, Reg::rcx, 64);
                e.Lea(Reg::rcx, Ptr(RegsBase, Reg::rax, 1, stack), 64);
                e.OpImm(Arith::cmp, Ptr(Reg::rcx, lc_off), 0, 16);
                e.J(CC::e, finished);
                e.OpImm(Arith::sub, Ptr(Reg::rcx, lc_off), 1, 16);
                e.Mov(Reg::rax, Ptr(Reg::rcx, start_off), 32);
                e.Mov(Ptr(RegsBase, pc_off), Reg::rax, 32);
                e.OpImm(Arith::cmp, Reg::rax, static_cast<s32>(pc), 32);
                e.J(CC::ne, to_start);
                // Another pass, if it fits in the limit.
                e.Lea(Reg::rax, Ptr(Executed, static_cast<s32>(count)), 64);
                e.Op(Arith::cmp, Reg::rax, Ptr(ContextReg, offsetof(Context, limit)), 64);
                e.J(CC::be, top);
                e.Bind(to_start);
                t.EmitComputeFlags(end_flags == Flags::Computed ? Flags::Computed : Flags::Either);
                exit_with(Exit::Done);
                e.Bind(finished);
                t.EmitComputeFlags(end_flags == Flags::Computed ? Flags::Computed : Flags::Either);
                // --bcn; lp = bcn != 0
                e.OpImm(Arith::sub, Ptr(RegsBase, bcn), 1, 16);
                e.Op(Arith::xor_, Reg::rax, Reg::rax, 32);
                e.OpImm(Arith::cmp, Ptr(RegsBase, bcn), 0, 16);
                e.Set(CC::ne, Reg::rax);
                e.Mov(Ptr(RegsBase, lp), Reg::rax, 16);
                e.MovImm(Ptr(RegsBase, pc_off), static_cast<s32>(address), 32);
                exit_with(Exit::Done);
            } else if (jumped) {
                t.EmitComputeFlags(end_flags);
                exit_with(Exit::Done);
            } else {
                const s32 pc_off = static_cast<s32>(reinterpret_cast<const u8*>(&modes.pc) -
                                                    reinterpret_cast<const u8*>(&modes));
                t.EmitComputeFlags(end_flags);
                e.MovImm(Ptr(RegsBase, pc_off), static_cast<s32>(address), 32);
                exit_with(Exit::Done);
            }
        }

        // Stops before instruction `index`: the ones before it have run.
        const s32 pc_off = static_cast<s32>(reinterpret_cast<const u8*>(&modes.pc) -
                                            reinterpret_cast<const u8*>(&modes));
        // Exits with pending or possibly pending flags share the code that computes them.
        Label bail_pending, bail_either;
        for (Bail& b : bails) {
            if (!b.label->Used()) {
                continue;
            }
            e.Bind(*b.label);
            e.OpImm(Arith::add, Executed, static_cast<s32>(b.index), 64);
            e.MovImm(Ptr(RegsBase, pc_off), static_cast<s32>(b.address), 32);
            switch (b.flags) {
            case Flags::Computed:
                exit_with(Exit::Bailed);
                break;
            case Flags::Pending:
                e.Jmp(bail_pending);
                break;
            case Flags::Either:
                e.Jmp(bail_either);
                break;
            }
        }
        if (bail_pending.Used()) {
            e.Bind(bail_pending);
            t.EmitComputeFlags(Flags::Pending);
            exit_with(Exit::Bailed);
        }
        if (bail_either.Used()) {
            e.Bind(bail_either);
            t.EmitComputeFlags(Flags::Either);
            exit_with(Exit::Bailed);
        }

        // The mode checks, run once when the block is entered: register fields next to each other
        // are compared together, and the memory interface's fields through Context::miu.
        e.Bind(guards_label);
        Label guard_failed;
        {
            std::vector<Guard> fields, miu_fields;
            for (const Guard& g : t.Guards()) {
                (g.absolute ? miu_fields : fields).push_back(g);
            }
            std::sort(fields.begin(), fields.end(),
                      [](const Guard& a, const Guard& b) { return a.offset < b.offset; });
            const auto compare = [&](Reg base, s32 offset, u64 value, u32 bytes) {
                switch (bytes) {
                case 2:
                    e.OpImm(Arith::cmp, Ptr(base, offset), static_cast<s16>(value), 16);
                    break;
                case 4:
                    e.OpImm(Arith::cmp, Ptr(base, offset), static_cast<s32>(value), 32);
                    break;
                default:
                    ASSERT(bytes == 8);
                    e.MovImm(Reg::rcx, value);
                    e.Op(Arith::cmp, Ptr(base, offset), Reg::rcx, 64);
                    break;
                }
                e.J(CC::ne, guard_failed);
            };
            for (std::size_t i = 0; i < fields.size();) {
                // A run of fields that follow each other, of up to 8 bytes.
                const s32 start = fields[i].offset;
                u64 value = 0;
                u32 bytes = 0;
                while (i < fields.size() && fields[i].offset == start + static_cast<s32>(bytes) &&
                       bytes + fields[i].size <= 8) {
                    value |= u64{fields[i].value} << (8 * bytes);
                    bytes += fields[i].size;
                    ++i;
                }
                if (bytes == 6) {
                    compare(RegsBase, start, value & 0xFFFF'FFFF, 4);
                    compare(RegsBase, start + 4, value >> 32, 2);
                } else {
                    compare(RegsBase, start, value, bytes);
                }
            }
            if (!miu_fields.empty()) {
                e.Mov(Reg::rax, Ptr(ContextReg, offsetof(Context, miu)), 64);
                for (const Guard& g : miu_fields) {
                    compare(Reg::rax, g.offset, g.value, g.size);
                }
            }
        }
        // The data pages (ConvertDataAddress), which the firmware switches: the bases of the X and
        // Y pages, or of the Z page twice. A page of 2 or more, which the interpreter asserts for,
        // leaves the block to the interpreter.
        Label refused;
        const s32 x_page = static_cast<s32>(offsetof(MemoryInterfaceUnit, x_page));
        const s32 y_page = static_cast<s32>(offsetof(MemoryInterfaceUnit, y_page));
        const s32 z_page = static_cast<s32>(offsetof(MemoryInterfaceUnit, z_page));
        const s32 data = static_cast<s32>(MemoryInterfaceUnit::DataMemoryOffset * 2);
        e.Mov(Reg::rax, Ptr(ContextReg, offsetof(Context, miu)), 64);
        if (miu.page_mode == 0) {
            e.Movzx(Reg::rcx, Ptr(Reg::rax, z_page), 16);
            e.OpImm(Arith::cmp, Reg::rcx, 2, 32);
            e.J(CC::ae, refused);
            e.ShiftImm(Shift::shl, Reg::rcx, 17, 32); // a page is 0x10000 words, 0x20000 bytes
            e.Lea(XPage, Ptr(MemoryBase, Reg::rcx, 1, data), 64);
            e.Mov(YPage, XPage, 64);
        } else {
            e.Movzx(Reg::rcx, Ptr(Reg::rax, x_page), 16);
            e.Movzx(Reg::rdx, Ptr(Reg::rax, y_page), 16);
            e.OpImm(Arith::cmp, Reg::rcx, 2, 32);
            e.J(CC::ae, refused);
            e.OpImm(Arith::cmp, Reg::rdx, 2, 32);
            e.J(CC::ae, refused);
            e.ShiftImm(Shift::shl, Reg::rcx, 17, 32);
            e.ShiftImm(Shift::shl, Reg::rdx, 17, 32);
            e.Lea(XPage, Ptr(MemoryBase, Reg::rcx, 1, data), 64);
            e.Lea(YPage, Ptr(MemoryBase, Reg::rdx, 1, data), 64);
        }
        e.Jmp(top);
        e.Bind(refused);
        e.MovImm(Ptr(ContextReg, offsetof(Context, exit)), static_cast<s32>(Exit::Refused), 32);
        e.Jmp(epilogue);
        e.Bind(guard_failed);
        e.MovImm(Ptr(ContextReg, offsetof(Context, exit)), static_cast<s32>(Exit::GuardFailed), 32);

        e.Bind(epilogue);
        e.Mov(Reg::rax, Executed, 64);
        e.OpImm(Arith::add, Reg::rsp, 40, 64);
        for (auto it = std::rbegin(saved); it != std::rend(saved); ++it) {
            e.Pop(*it);
        }
        e.Ret();

        const u8* placed = code.Add(e.code);
        if (!placed) {
            // Full: start again with no blocks.
            Reset();
            placed = code.Add(e.code);
            if (!placed) {
                return nullptr;
            }
        }
        block->code = reinterpret_cast<BlockCode>(const_cast<u8*>(placed));
        if (stats) {
            block->guards = t.Guards();
        }
        if (!dump.empty()) {
            Dump(*block, e.code, t.Guards());
        }
        ++blocks_made;
        return Insert(std::move(block));
    }

    // With TEAKRA_JIT_DUMP set to a directory: <pc>-<n>.bin holds a block's code and <pc>-<n>.txt
    // its instructions and guards.
    void Dump(const Block& block, const std::vector<u8>& bytes, const std::vector<Guard>& guards) {
        char name[64];
        std::snprintf(name, sizeof name, "/%05x-%llu", block.start,
                      static_cast<unsigned long long>(blocks_made));
        if (FILE* f = std::fopen((dump + name + ".bin").c_str(), "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((dump + name + ".txt").c_str(), "w")) {
            std::fprintf(f, "start %05x loop_end %05x length %u bytes %zu\n", block.start,
                         block.loop_end, block.length, bytes.size());
            for (const Guard& g : guards) {
                std::fprintf(f, "guard %s+%x size %u == %x\n", g.absolute ? "abs" : "regs",
                             static_cast<unsigned>(g.offset), static_cast<unsigned>(g.size),
                             static_cast<unsigned>(g.value));
            }
            u32 address = block.start;
            for (std::size_t i = 0; i < block.source.size();) {
                const u16 opcode = block.source[i];
                const bool expanded = Disassembler::NeedExpansion(opcode);
                const u16 expansion =
                    expanded && i + 1 < block.source.size() ? block.source[i + 1] : 0;
                std::fprintf(f, "%05x  %04x  %s\n", address, opcode,
                             Disassembler::Do(opcode, expansion).c_str());
                const u32 size = expanded ? 2 : 1;
                address += size;
                i += size;
            }
            std::fclose(f);
        }
    }

    Block* Insert(std::unique_ptr<Block> block) {
        const u32 pc = block->start;
        block->next = std::move(table[pc]);
        table[pc] = std::move(block);
        return table[pc].get();
    }

    void Reset() {
        for (auto& slot : table) {
            slot.reset();
        }
        code.Clear();
    }

    RegisterState& regs;
    MemoryInterface& mem;
    CodeMemory code;
    std::vector<std::unique_ptr<Block>> table;
    std::unordered_map<u16, Matcher<Translator>> matchers;
    Context context{};
    void* interpreter = nullptr;
    Jit::Handler handler = nullptr;
    bool enabled = false;
    bool stats = false;
    bool trace = false;
    std::string dump;
    u64 run = 0;
    u64 blocks_made = 0;
    u64 runs = 0;
    u64 calls = 0;
    u64 validations = 0;
    std::unordered_map<u32, std::pair<u64, u64>> per_block;
    std::unordered_map<u32, u64> bails_at;
    std::unordered_map<u64, u64> guard_failures;
    std::unordered_map<u16, u64> interpreted_opcodes;
    std::unordered_map<u32, u64> interpreted_by_stop;
    std::unordered_map<std::string, u64> refused;
};

Jit::Jit(RegisterState& regs, MemoryInterface& mem) : impl(new Impl(regs, mem)) {}
Jit::~Jit() = default;

bool Jit::Enabled() const {
    return impl->enabled;
}

u64 Jit::Run(u64 limit) {
    return impl->Run(limit);
}

void Jit::Reset() {
    impl->Reset();
}

bool Jit::Stats() const {
    return impl->stats;
}

bool Jit::StoppedForBudget() const {
    return impl->stop == Impl::Stop::Budget;
}

void Jit::Interpreted(u32 pc, u64 count, bool no_budget) {
    if (no_budget) {
        impl->stop = Impl::Stop::Budget;
    }
    const u16 opcode = SharedMemory::ReadWord(impl->mem.ProgramMemory(), pc);
    impl->interpreted_opcodes[opcode] += count;
    impl->interpreted_by_stop[(u32{opcode} << 8) | static_cast<u32>(impl->stop)] += count;
    impl->stops[static_cast<int>(impl->stop)] += count;
}

void Jit::SetInterpreter(void* interpreter, Handler handler) {
    impl->interpreter = interpreter;
    impl->handler = handler;
}

u64 Jit::Instructions() const {
    return impl->run;
}

#else // !TEAKRA_JIT_X64

struct Jit::Impl {};

Jit::Jit(RegisterState&, MemoryInterface&) {}
Jit::~Jit() = default;

bool Jit::Enabled() const {
    return false;
}

u64 Jit::Run(u64) {
    return 0;
}

void Jit::Reset() {}
bool Jit::Stats() const {
    return false;
}
bool Jit::StoppedForBudget() const {
    return false;
}
u64 Jit::Instructions() const {
    return 0;
}
void Jit::Interpreted(u32, u64, bool) {}
void Jit::SetInterpreter(void*, Handler) {}

#endif

} // namespace Teakra
