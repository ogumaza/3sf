#pragma once

// Translates runs of the DSP's ordinary instructions (see Interpreter::DecoderEntry) into x86-64
// code and runs it, with the same results as the interpreter.
//
// A block is a run of instructions from one address, translated for the mode registers it finds
// there (the product shift, saturation, address steps and modulo, the memory interface's pages and
// so on), which its code checks on entry. It ends before the first instruction it can't
// translate, at the end of the innermost block-repeat loop, whose jump back it takes itself, or
// after a limit. Its code touches only the registers and the data memory: before an instruction
// that would read or write an MMIO register, the block stops, and the interpreter runs that
// instruction. The interpreter only runs a block when none of the components can produce an event
// before it ends (CoreTiming::BudgetLeft), so a block's instructions only need counting.
//
// Program memory changes when a firmware is loaded, which Reset precedes, and otherwise rarely:
// the DSP writes it itself (movd, DMA that wraps around into it) and the host through
// GetDspMemory, which it reports with Teakra::NotifyProgramWrite. Each block keeps the words it was
// translated from and compares them with the memory the first time it runs after such a write.

#include <memory>
#include "common_types.h"

namespace Teakra {

class MemoryInterface;
struct RegisterState;

class Jit {
public:
    Jit(RegisterState& regs, MemoryInterface& mem);
    ~Jit();

    Jit(const Jit&) = delete;
    Jit& operator=(const Jit&) = delete;

    // True if the JIT is available: the host is x86-64, executable memory is allowed, and the
    // TEAKRA_JIT environment variable isn't 0.
    bool Enabled() const;

    // Runs translated code from regs.pc for at most `limit` instructions, while there is code for
    // the address it's at. Returns the number of instructions run, 0 if it had no block to run.
    u64 Run(u64 limit);

    // Forgets every block (a reset).
    void Reset();

    // True if the last Run stopped at a block that needs more instructions than its limit.
    bool StoppedForBudget() const;

    // The instructions that translated code has run.
    u64 Instructions() const;

    // With TEAKRA_JIT_STATS set, the instructions the interpreter runs between blocks are counted
    // (Interpreted: `count` from `pc`, and whether Run was called at all), and a summary is
    // printed at the end.
    bool Stats() const;
    void Interpreted(u32 pc, u64 count, bool no_budget);

    // Runs one instruction in the interpreter for a block that doesn't translate it. Exceptions
    // cannot unwind through generated code, so the handler saves them for Run's caller and returns
    // false to stop the block.
    using Handler = bool (*)(void* interpreter, u16 opcode, u16 expansion);
    void SetInterpreter(void* interpreter, Handler handler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Teakra
