// SPDX-License-Identifier: MIT

#include "nnsnd/dsp_link.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "common/bytes.h"

namespace threesf::nnsnd
{
namespace
{

// The structures nn::snd uses, by their place in the list of addresses the firmware sends through the audio pipe.
enum StructIndex
{
    kFrameCounter = 0,
    kSourceParams = 1,
    kSourceReports = 2,
    kAdpcmCoefficients = 3,
    kMixerParams = 4,
};

// nn::snd per-frame semaphore value (SetSemaphoreMask at code.bin 0x148838) and the value it sets directly during
// initialization (0x1488e8).
constexpr uint16_t kFrameSemaphore = 0x2000;
constexpr uint16_t kInitSemaphore = 0x4000;

// DSP cycles that pass between the frame interrupt and the commit: the sound thread's processing time, taken as a
// quarter of a frame.
constexpr uint64_t kArmLatencyCycles = dsp::kCyclesPerFrame / 4;

// Initialize gives up after about 0.75 s of DSP time without an answer from the firmware, and WaitForFrame after about
// 1.5 s without the end of a frame.
constexpr uint64_t kMaxInitWaitCycles = 100'000'000;
constexpr uint64_t kMaxFrameWaitCycles = 200'000'000;

} // namespace

DspLink::DspLink(dsp::TeakraDsp& dsp) : dsp_(dsp)
{
    const auto on_interrupt = [this](dsp::Interrupt type, dsp::Pipe pipe)
    {
        if (type == dsp::Interrupt::kPipe && pipe == dsp::Pipe::kAudio)
        {
            audio_irq_ = true;
        }
    };
    dsp_.SetInterruptHandler(on_interrupt);
}

bool DspLink::Initialize(std::span<const uint8_t> component)
{
    BeforeDspRuns();
    dsp_.LoadComponent(component);

    // Audio pipe command 0 = Initialize (2 = Wakeup). dsp::DSP zeroes bytes 2..3.
    constexpr uint8_t kInitializeCommand[4] = {0, 0, 0, 0};
    audio_irq_ = false;
    dsp_.WritePipe(dsp::Pipe::kAudio, kInitializeCommand);
    dsp_.SetSemaphore(kInitSemaphore);
    uint64_t waited = 0;
    while (!audio_irq_)
    {
        if (waited > kMaxInitWaitCycles)
        {
            AfterDspRuns();
            return false;
        }

        dsp_.Run(1024);
        waited += 1024;
    }

    AfterDspRuns();
    audio_irq_ = false;

    const auto count_bytes = dsp_.ReadPipe(dsp::Pipe::kAudio, 2);
    const uint16_t count = static_cast<uint16_t>(count_bytes[0] | (count_bytes[1] << 8));
    if (count != struct_addr_.size())
    {
        return false;
    }

    auto addrs = dsp_.ReadPipe(dsp::Pipe::kAudio, count * 2);
    for (uint16_t i = 0; i < count; i++)
    {
        struct_addr_[i] = static_cast<uint16_t>(addrs[i * 2] | (addrs[i * 2 + 1] << 8));
    }

    // Every structure used here has to lie inside DSP RAM in both regions (region 1 is the higher one).
    constexpr std::array<std::pair<int, std::size_t>, 5> kUsed = {{
        {kFrameCounter, sizeof(uint16_t)},
        {kSourceParams, sizeof(std::array<SourceParams, kNumSources>)},
        {kSourceReports, sizeof(std::array<SourceReport, kNumSources>)},
        {kAdpcmCoefficients, sizeof(AdpcmCoefficientTable)},
        {kMixerParams, sizeof(MixerParams)},
    }};
    for (const auto& [structure, size] : kUsed)
    {
        if (dsp::DataOffset(struct_addr_[structure] | 0x10000u) + size > dsp::kRamSize)
        {
            return false;
        }
    }

    dsp_.SetSemaphore(kInitSemaphore);
    frame_ = 4;
    StoreWord(kFrameCounter, 0, frame_);
    frame_++;
    dsp_.SetSemaphore(kFrameSemaphore); // SignalEvent(semaphore event)
    write_region_ = frame_ & 1;
    read_region_ = frame_ & 1;

    return true;
}

bool DspLink::WaitForFrame()
{
    BeforeDspRuns();
    uint64_t waited = 0;
    while (!audio_irq_)
    {
        if (waited >= kMaxFrameWaitCycles)
        {
            AfterDspRuns();
            return false;
        }

        dsp_.Run(256);
        waited += 256;
    }

    AfterDspRuns();
    audio_irq_ = false;

    // nn::snd 0x18b40c: the counter in the region not being written tells the DSP's progress.
    const uint16_t counter = LoadWord(kFrameCounter, (~frame_) & 1);
    if (counter != 0)
    {
        uint16_t next = static_cast<uint16_t>(counter + 1);
        if (counter == 0xFFFF)
        {
            next = 2;
        }
        frame_ = next;
        read_region_ = frame_ & 1;
    }

    return true;
}

void DspLink::Commit()
{
    // The game's sound thread spends time between the DSP's frame interrupt and this commit; the DSP keeps running
    // meanwhile. Committing in zero DSP time can land the semaphore while the firmware is still finishing the frame,
    // where it gets lost (the firmware then idles until a ~116M-cycle recovery). Model the ARM's processing time before
    // committing.
    BeforeDspRuns();

    for (uint64_t run = 0; run < kArmLatencyCycles; run += 256)
    {
        dsp_.Run(256);
    }

    AfterDspRuns();
    StoreWord(kFrameCounter, write_region_, frame_);
    frame_++;
    dsp_.SetSemaphore(kFrameSemaphore); // SignalEvent(semaphore event)
    write_region_ = frame_ & 1;
}

SourceParams& DspLink::SourceParamsOf(int voice)
{
    return Get(sources_, kSourceParams, frame_ & 1)[voice];
}

int16_t* DspLink::AdpcmCoefs(int voice)
{
    return Get(adpcm_coefs_, kAdpcmCoefficients, write_region_).source[voice];
}

MixerParams& DspLink::Mixer()
{
    return Get(mixer_, kMixerParams, frame_ & 1);
}

const SourceReport& DspLink::SourceReportOf(int voice) const
{
    return Get(reports_, kSourceReports, read_region_)[voice];
}

uint8_t* DspLink::Address(int structure, int region) const
{
    const uint32_t address = struct_addr_[structure] | (region ? 0x10000u : 0u);
    return dsp_.Ram() + dsp::DataOffset(address);
}

uint16_t DspLink::LoadWord(int structure, int region) const
{
    return LoadLe16(Address(structure, region));
}

void DspLink::StoreWord(int structure, int region, uint16_t value)
{
    StoreLe16(Address(structure, region), value);
}

// The DSP reads the ARM11's parameters, and writes its own results, while it runs: the parameters go back to DSP RAM
// before it starts, and every copy is stale once it has run.
void DspLink::BeforeDspRuns()
{
    WriteBack(sources_, kSourceParams);
    WriteBack(adpcm_coefs_, kAdpcmCoefficients);
    WriteBack(mixer_, kMixerParams);
}

void DspLink::AfterDspRuns()
{
    sources_.loaded = {};
    adpcm_coefs_.loaded = {};
    mixer_.loaded = {};
    reports_.loaded = {};
}

} // namespace threesf::nnsnd
