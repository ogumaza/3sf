#pragma once
#include <array>
#include <memory>
#include <optional>
#include "common_types.h"

namespace Teakra {
struct SharedMemory {
    // We allocate our own memory if the user doesn't supply their own
    std::unique_ptr<std::array<u8, 0x80000>> own_memory;
    // Points to either own own memory or user-supplied memory
    u8* raw;
    // Counts the writes to program memory that the DSP makes itself (movd, and DMA that wraps
    // around into it), for the JIT (jit.h). Writes through `raw` aren't counted.
    u64 program_writes = 0;
    static constexpr u32 ProgramWords = 0x20000;

    SharedMemory(u8* mem = nullptr) : raw{mem} {
        if (mem == nullptr) {
            own_memory = std::make_unique<std::array<u8, 0x80000>>();
            raw = own_memory->data();
        }
    }

    // Word addresses wrap around at the end of the memory, so that none reaches outside it:
    // the firmware, which comes with the data a host plays, sets the addresses of DMA transfers
    // and the program page.
    static constexpr u32 WordAddressMask = 0x80000 / 2 - 1;

    // Reads a word of the memory whose bytes start at `bytes`.
    static TEAKRA_ALWAYS_INLINE u16 ReadWord(const u8* bytes, u32 word_address) {
        u32 byte_address = (word_address & WordAddressMask) * 2;
        u8 low = bytes[byte_address];
        u8 high = bytes[byte_address + 1];
        return low | ((u16)high << 8);
    }
    TEAKRA_ALWAYS_INLINE u16 ReadWord(u32 word_address) const {
        return ReadWord(raw, word_address);
    }
    // WriteWord for a write the DSP makes (DMA), counted in program_writes if it lands in program
    // memory.
    void WriteWordFromDsp(u32 word_address, u16 value) {
        if ((word_address & WordAddressMask) < ProgramWords) {
            ++program_writes;
        }
        WriteWord(word_address, value);
    }
    TEAKRA_ALWAYS_INLINE void WriteWord(u32 word_address, u16 value) {
        u8 low = value & 0xFF;
        u8 high = value >> 8;
        u32 byte_address = (word_address & WordAddressMask) * 2;
        raw[byte_address] = low;
        raw[byte_address + 1] = high;
    }
};
} // namespace Teakra
