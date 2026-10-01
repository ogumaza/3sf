#include "memory_interface.h"
#include "mmio.h"
#include "shared_memory.h"

namespace Teakra {
MemoryInterface::MemoryInterface(SharedMemory& shared_memory,
                                 MemoryInterfaceUnit& memory_interface_unit)
    : shared_memory(shared_memory), memory_interface_unit(memory_interface_unit) {}

void MemoryInterface::SetMMIO(MMIORegion& mmio) {
    this->mmio = &mmio;
}

void MemoryInterface::ProgramWrite(u32 address, u16 value) {
    shared_memory.WriteWord(address, value);
    ++shared_memory.program_writes;
}
u16 MemoryInterface::DataReadMMIO(u16 address) {
    ASSERT(mmio != nullptr);
    return mmio->Read(memory_interface_unit.ToMMIO(address));
}
void MemoryInterface::DataWriteMMIO(u16 address, u16 value) {
    ASSERT(mmio != nullptr);
    mmio->Write(memory_interface_unit.ToMMIO(address), value);
}
u16 MemoryInterface::DataReadA32(u32 address) const {
    u32 converted = (address & ((MemoryInterfaceUnit::DataMemoryBankSize*2)-1))
        + MemoryInterfaceUnit::DataMemoryOffset;
    return shared_memory.ReadWord(converted);
}
void MemoryInterface::DataWriteA32(u32 address, u16 value) {
    u32 converted = (address & ((MemoryInterfaceUnit::DataMemoryBankSize*2)-1))
        + MemoryInterfaceUnit::DataMemoryOffset;
    shared_memory.WriteWord(converted, value);
}
u16 MemoryInterface::MMIORead(u16 address) {
    ASSERT(mmio != nullptr);
    // according to GBATek ("DSi Teak I/O Ports (on ARM9 Side)"), these are mirrored
    return mmio->Read(address & (MemoryInterfaceUnit::MMIOSize - 1));
}
void MemoryInterface::MMIOWrite(u16 address, u16 value) {
    ASSERT(mmio != nullptr);
    mmio->Write(address & (MemoryInterfaceUnit::MMIOSize - 1), value);
}

} // namespace Teakra
