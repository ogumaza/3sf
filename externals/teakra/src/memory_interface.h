#pragma once

#include <array>
#include "common_types.h"
#include "crash.h"
#include "shared_memory.h"

namespace Teakra {

class MemoryInterfaceUnit {
public:
    u16 x_page = 0, y_page = 0, z_page = 0;
    static constexpr u16 XYSizeResolution = 0x400;
    std::array<u16, 2> x_size{{0x20, 0x20}};
    std::array<u16, 2> y_size{{0x1E, 0x1E}};
    u16 page_mode = 0;
    u16 mmio_base = 0x8000;

    static constexpr u16 MMIOSize = 0x0800;
    static constexpr u32 DataMemoryOffset = 0x20000;
    static constexpr u32 DataMemoryBankSize = 0x10000;

    void Reset() {
        *this = MemoryInterfaceUnit();
    }

    template <typename Archive>
    void Serialize(Archive& ar) {
        ar(x_page, y_page, z_page, x_size, y_size, page_mode, mmio_base);
    }

    TEAKRA_ALWAYS_INLINE bool InMMIO(u16 addr) const {
        return addr >= mmio_base && addr < mmio_base + MMIOSize;
    }
    u16 ToMMIO(u16 addr) const {
        ASSERT(z_page == 0);
        // according to GBATek ("DSi Teak I/O Ports (on ARM9 Side)"), these are mirrored
        return (addr - mmio_base) & (MMIOSize - 1);
    }

    TEAKRA_ALWAYS_INLINE u32 ConvertDataAddress(u16 addr) const {
        if (page_mode == 0) {
            ASSERT(z_page < 2);
            return DataMemoryOffset + addr + z_page * DataMemoryBankSize;
        } else {
            if (addr <= x_size[0] * XYSizeResolution) {
                ASSERT(x_page < 2);
                return DataMemoryOffset + addr + x_page * DataMemoryBankSize;
            } else {
                ASSERT(y_page < 2);
                return DataMemoryOffset + addr + y_page * DataMemoryBankSize;
            }
        }
    }
};

struct SharedMemory;
class MMIORegion;

class MemoryInterface {
public:
    MemoryInterface(SharedMemory& shared_memory, MemoryInterfaceUnit& memory_interface_unit);
    void SetMMIO(MMIORegion& mmio);
    // The common cases are inline: these run for almost every DSP instruction.
    TEAKRA_ALWAYS_INLINE u16 ProgramRead(u32 address) const {
        return shared_memory.ReadWord(address);
    }
    void ProgramWrite(u32 address, u16 value);
    // not const because it can be a FIFO register
    TEAKRA_ALWAYS_INLINE u16 DataRead(u16 address, bool bypass_mmio = false) {
        if (memory_interface_unit.InMMIO(address) && !bypass_mmio) {
            return DataReadMMIO(address);
        }
        return shared_memory.ReadWord(memory_interface_unit.ConvertDataAddress(address));
    }
    TEAKRA_ALWAYS_INLINE void DataWrite(u16 address, u16 value, bool bypass_mmio = false) {
        if (memory_interface_unit.InMMIO(address) && !bypass_mmio) {
            DataWriteMMIO(address, value);
            return;
        }
        shared_memory.WriteWord(memory_interface_unit.ConvertDataAddress(address), value);
    }
    // The DSP memory as bytes, for fetching instructions without going through this class
    // (see Interpreter::Run). It stays at the same address for the memory's lifetime.
    const u8* ProgramMemory() const {
        return shared_memory.raw;
    }
    // The same memory, writable, and the unit that maps data addresses to it, for the JIT (jit.h),
    // whose code reads and writes data memory itself.
    u8* Memory() {
        return shared_memory.raw;
    }
    const MemoryInterfaceUnit& Unit() const {
        return memory_interface_unit;
    }
    u64 ProgramWrites() const {
        return shared_memory.program_writes;
    }
    u16 DataReadA32(u32 address) const;
    void DataWriteA32(u32 address, u16 value);
    u16 MMIORead(u16 address);
    void MMIOWrite(u16 address, u16 value);

private:
    u16 DataReadMMIO(u16 address);
    void DataWriteMMIO(u16 address, u16 value);

    SharedMemory& shared_memory;
    MemoryInterfaceUnit& memory_interface_unit;
    MMIORegion* mmio;
};

} // namespace Teakra
