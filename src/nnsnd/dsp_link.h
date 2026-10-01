// SPDX-License-Identifier: MIT

// Model of nn::snd's link to the DSP audio firmware (CTR SDK 5.2, as linked into Pokemon X).
//
// This is the ARM11 side of the DSP shared-memory protocol, as worked out from the game's code.bin:
//   0x148764 open: register the audio-pipe interrupt event, SetSemaphoreMask(0x2000)
//   0x1488e8 initialize: audio pipe "Initialize", SetSemaphore(0x4000), read the 15 struct
//            addresses, map both regions (region 1 = region 0 address | 0x10000), commit frame 4
//   0x18b40c receive: re-derive the frame counter and read region, read source statuses
//   0x18b2d4 send:    run aux-bus effects in place, write parameters, commit the frame counter,
//            signal the semaphore event (which makes dsp::DSP set semaphore 0x2000)

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "dsp/teakra_dsp.h"
#include "nnsnd/dsp_abi.h"

namespace threesf::nnsnd
{

class DspLink
{
public:
    explicit DspLink(dsp::TeakraDsp& dsp);

    // Loads the DSP component and performs nn::snd's initialization handshake. Returns false if the DSP didn't answer.
    bool Initialize(std::span<const uint8_t> component);

    // Runs the DSP until it raises the audio-pipe interrupt that ends a frame, then performs nn::snd's receive step.
    // Returns false if the DSP stopped producing frames.
    bool WaitForFrame();

    // Commits the frame being prepared (nn::snd 0x18b2d4 tail): write the frame counter into the write region, advance
    // it and signal the semaphore event.
    void Commit();

    // The structures in the region the ARM11 is preparing (frame & 1) and in the region the DSP last wrote (the read
    // region). They're host copies of DSP RAM, kept in step with it by Initialize, WaitForFrame and Commit, which are
    // the only places the DSP runs.
    SourceParams& SourceParamsOf(int voice);
    int16_t* AdpcmCoefs(int voice);
    MixerParams& Mixer();
    const SourceReport& SourceReportOf(int voice) const;

private:
    // Host copies of one of the firmware's structures, one per region. They're loaded from DSP RAM when first used,
    // written back before the DSP next runs, and reloaded after that.
    template <typename T>
    struct Mirror
    {
        std::array<T, 2> value{};
        std::array<bool, 2> loaded{};
    };

    uint8_t* Address(int structure, int region) const;
    uint16_t LoadWord(int structure, int region) const;
    void StoreWord(int structure, int region, uint16_t value);

    template <typename T>
    T& Get(Mirror<T>& mirror, int structure, int region) const
    {
        static_assert(std::is_trivially_copyable_v<T>);

        if (!mirror.loaded[region])
        {
            std::memcpy(&mirror.value[region], Address(structure, region), sizeof(T));
            mirror.loaded[region] = true;
        }

        return mirror.value[region];
    }

    template <typename T>
    void WriteBack(const Mirror<T>& mirror, int structure)
    {
        for (int region = 0; region < 2; region++)
        {
            if (mirror.loaded[region])
            {
                std::memcpy(Address(structure, region), &mirror.value[region], sizeof(T));
            }
        }
    }

    void BeforeDspRuns();
    void AfterDspRuns();

    dsp::TeakraDsp& dsp_;
    std::array<uint16_t, 15> struct_addr_{};
    uint16_t frame_ = 0;   // nn::snd +0x131a
    int read_region_ = 0;  // nn::snd +0x131c
    int write_region_ = 0; // nn::snd +0x131e
    bool audio_irq_ = false;
    mutable Mirror<std::array<SourceParams, kNumSources>> sources_;
    mutable Mirror<AdpcmCoefficientTable> adpcm_coefs_;
    mutable Mirror<MixerParams> mixer_;
    mutable Mirror<std::array<SourceReport, kNumSources>> reports_;
};

} // namespace threesf::nnsnd
