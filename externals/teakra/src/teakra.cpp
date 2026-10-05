#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "ahbm.h"
#include "apbp.h"
#include "btdmp.h"
#include "core_timing.h"
#include "dma.h"
#include "icu.h"
#include "memory_interface.h"
#include "mmio.h"
#include "processor.h"
#include "register.h"
#include "shared_memory.h"
#include "state.h"
#include "teakra/teakra.h"
#include "timer.h"

namespace Teakra {

struct Teakra::Impl {
    CoreTiming core_timing;
    SharedMemory shared_memory;
    MemoryInterfaceUnit miu;
    ICU icu;
    Apbp apbp_from_cpu, apbp_from_dsp;
    std::array<Timer, 2> timer{{{core_timing}, {core_timing}}};
    Ahbm ahbm;
    Dma dma{shared_memory, ahbm};
    std::array<Btdmp, 2> btdmp{{{core_timing}, {core_timing}}};
    MMIORegion mmio{miu, icu, apbp_from_cpu, apbp_from_dsp, timer, dma, ahbm, btdmp, core_timing};
    MemoryInterface memory_interface{shared_memory, miu};
    Processor processor{core_timing, memory_interface};

    Impl(u8* dsp_memory) : shared_memory{dsp_memory} {
        memory_interface.SetMMIO(mmio);
        using namespace std::placeholders;
        icu.SetInterruptHandler(std::bind(&Processor::SignalInterrupt, &processor, _1),
                                std::bind(&Processor::SignalVectoredInterrupt, &processor, _1, _2));

        timer[0].SetInterruptHandler([this]() { icu.TriggerSingle(0xA); });
        timer[1].SetInterruptHandler([this]() { icu.TriggerSingle(0x9); });

        apbp_from_cpu.SetDataHandler(0, [this]() { icu.TriggerSingle(0xE); });
        apbp_from_cpu.SetDataHandler(1, [this]() { icu.TriggerSingle(0xE); });
        apbp_from_cpu.SetDataHandler(2, [this]() { icu.TriggerSingle(0xE); });
        apbp_from_cpu.SetSemaphoreHandler([this]() { icu.TriggerSingle(0xE); });

        btdmp[0].SetInterruptHandler([this]() { icu.TriggerSingle(0xB); });
        btdmp[1].SetInterruptHandler([this]() { icu.TriggerSingle(0xB); });

        dma.SetInterruptHandler([this]() { icu.TriggerSingle(0xF); });
    }

    template <typename Archive>
    void Serialize(Archive& ar) {
        core_timing.Serialize(ar);
        miu.Serialize(ar);
        icu.Serialize(ar);
        apbp_from_cpu.Serialize(ar);
        apbp_from_dsp.Serialize(ar);
        for (Timer& t : timer) {
            t.Serialize(ar);
        }
        ahbm.Serialize(ar);
        dma.Serialize(ar);
        for (Btdmp& b : btdmp) {
            b.Serialize(ar);
        }
        mmio.Serialize(ar);
        processor.Serialize(ar);
    }

    void Reset() {
        std::memset(shared_memory.raw, 0, DspMemorySize);
        miu.Reset();
        apbp_from_cpu.Reset();
        apbp_from_dsp.Reset();
        timer[0].Reset();
        timer[1].Reset();
        ahbm.Reset();
        dma.Reset();
        btdmp[0].Reset();
        btdmp[1].Reset();
        core_timing.Invalidate();
        processor.Reset();
    }
};

Teakra::Teakra(const UserConfig& config) : impl(std::make_unique<Impl>(config.dsp_memory)) {}
Teakra::~Teakra() = default;

void Teakra::Reset() {
    impl->Reset();
}

uint8_t* Teakra::GetDspMemory() {
    return impl->shared_memory.raw;
}

const uint8_t* Teakra::GetDspMemory() const {
    return impl->shared_memory.raw;
}

void Teakra::NotifyProgramWrite() {
    ++impl->shared_memory.program_writes;
}

std::uint64_t Teakra::JitInstructions() const {
    return impl->processor.JitInstructions();
}

// A saved state starts with these two words. The version changes with the layout.
static constexpr std::uint32_t StateMagic = 0x5453414B;
static constexpr std::uint32_t StateVersion = 1;

std::vector<std::uint8_t> Teakra::SaveState() {
    std::vector<std::uint8_t> state;
    StateWriter ar(state);
    std::uint32_t magic = StateMagic, version = StateVersion;
    ar(magic, version);
    impl->Serialize(ar);
    return state;
}

void Teakra::LoadState(const std::vector<std::uint8_t>& state) {
    StateReader ar(state.data(), state.size());
    std::uint32_t magic = 0, version = 0;
    ar(magic, version);
    if (magic != StateMagic || version != StateVersion) {
        throw std::invalid_argument("Teakra: not a saved state from this version");
    }

    impl->Serialize(ar);
    if (!ar.AtEnd()) {
        throw std::invalid_argument("Teakra: the saved state is longer than expected");
    }

    // The memory comes back from the host, so translated code is checked against it again.
    ++impl->shared_memory.program_writes;
}

RegisterState& Teakra::GetRegisterState() {
    return impl->processor.GetRegisterState();
}

const RegisterState& Teakra::GetRegisterState() const {
    return impl->processor.GetRegisterState();
}

void Teakra::Run(unsigned cycle) {
    impl->dma.AddBudget(cycle);
    impl->processor.Run(cycle);
}

bool Teakra::SendDataIsEmpty(std::uint8_t index) const {
    return !impl->apbp_from_cpu.IsDataReady(index);
}
void Teakra::SendData(std::uint8_t index, std::uint16_t value) {
    impl->apbp_from_cpu.SendData(index, value);
}
bool Teakra::RecvDataIsReady(std::uint8_t index) const {
    return impl->apbp_from_dsp.IsDataReady(index);
}
std::uint16_t Teakra::RecvData(std::uint8_t index) {
    return impl->apbp_from_dsp.RecvData(index);
}
std::uint16_t Teakra::PeekRecvData(std::uint8_t index) {
    return impl->apbp_from_dsp.PeekData(index);
}
void Teakra::SetRecvDataHandler(std::uint8_t index, std::function<void()> handler) {
    impl->apbp_from_dsp.SetDataHandler(index, std::move(handler));
}

void Teakra::SetSemaphore(std::uint16_t value) {
    impl->apbp_from_cpu.SetSemaphore(value);
}
void Teakra::SetSemaphoreHandler(std::function<void()> handler) {
    impl->apbp_from_dsp.SetSemaphoreHandler(std::move(handler));
}
std::uint16_t Teakra::GetSemaphore() const {
    return impl->apbp_from_dsp.GetSemaphore();
}
void Teakra::ClearSemaphore(std::uint16_t value) {
    impl->apbp_from_dsp.ClearSemaphore(value);
}
void Teakra::MaskSemaphore(std::uint16_t value) {
    impl->apbp_from_dsp.MaskSemaphore(value);
}
void Teakra::SetAHBMCallback(const AHBMCallback& callback) {
    impl->ahbm.SetExternalMemoryCallback(callback.read8, callback.write8, callback.read16,
                                         callback.write16, callback.read32, callback.write32);
}

std::uint16_t Teakra::AHBMGetUnitSize(std::uint16_t i) const {
    return impl->ahbm.GetUnitSize(i);
}
std::uint16_t Teakra::AHBMGetDirection(std::uint16_t i) const {
    return impl->ahbm.GetDirection(i);
}
std::uint16_t Teakra::AHBMGetDmaChannel(std::uint16_t i) const {
    return impl->ahbm.GetDmaChannel(i);
}

std::uint16_t Teakra::AHBMRead16(std::uint32_t addr) {
    return impl->ahbm.Read16(0, addr);
}
void Teakra::AHBMWrite16(std::uint32_t addr, std::uint16_t value) {
    impl->ahbm.Write16(0, addr, value);
}
std::uint16_t Teakra::AHBMRead32(std::uint32_t addr) {
    return impl->ahbm.Read32(0, addr);
}
void Teakra::AHBMWrite32(std::uint32_t addr, std::uint32_t value) {
    impl->ahbm.Write32(0, addr, value);
}

void Teakra::SetAudioCallback(std::function<void(std::array<s16, 2>)> callback) {
    impl->btdmp[0].SetAudioCallback(std::move(callback));
}

std::uint16_t Teakra::ProgramRead(std::uint32_t address) const {
    return impl->memory_interface.ProgramRead(address);
}
void Teakra::ProgramWrite(std::uint32_t address, std::uint16_t value) {
    impl->memory_interface.ProgramWrite(address, value);
}
std::uint16_t Teakra::DataRead(std::uint16_t address, bool bypass_mmio) {
    return impl->memory_interface.DataRead(address, bypass_mmio);
}
void Teakra::DataWrite(std::uint16_t address, std::uint16_t value, bool bypass_mmio) {
    impl->memory_interface.DataWrite(address, value, bypass_mmio);
}
std::uint16_t Teakra::DataReadA32(std::uint32_t address) const {
    return impl->memory_interface.DataReadA32(address);
}
void Teakra::DataWriteA32(std::uint32_t address, std::uint16_t value) {
    impl->memory_interface.DataWriteA32(address, value);
}
std::uint16_t Teakra::MMIORead(std::uint16_t address) {
    return impl->memory_interface.MMIORead(address);
}
void Teakra::MMIOWrite(std::uint16_t address, std::uint16_t value) {
    impl->memory_interface.MMIOWrite(address, value);
}

std::uint16_t Teakra::DMAChan0GetSrcHigh() {
    u16 active_bak = impl->dma.GetActiveChannel();
    impl->dma.ActivateChannel(0);
    u16 ret = impl->dma.GetAddrSrcHigh();
    impl->dma.ActivateChannel(active_bak);
    return ret;
}
std::uint16_t Teakra::DMAChan0GetDstHigh() {
    u16 active_bak = impl->dma.GetActiveChannel();
    impl->dma.ActivateChannel(0);
    u16 ret = impl->dma.GetAddrDstHigh();
    impl->dma.ActivateChannel(active_bak);
    return ret;
}

} // namespace Teakra
