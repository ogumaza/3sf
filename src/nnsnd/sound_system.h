// SPDX-License-Identifier: MIT

// Model of nn::snd's global state: 24 voices, the DSP link, the DSP configuration, and the per-frame receive/send
// sequence (code.bin 0x185f50 / 0x186070 / 0x18b40c / 0x18b2d4).

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <span>

#include "nnsnd/dsp_link.h"
#include "nnsnd/voice.h"

namespace threesf::nnsnd
{

// The DSP settings nn::snd writes when it starts (0x148470 and 0x1345fc) that a player can change.
struct DspDefaults
{
    uint16_t clipping_mode = 1;
    uint16_t output_format = 1; // stereo (system setting)
};

class SoundSystem
{
public:
    // Owner callback supplied at allocation. AllocVoice calls it after freeing a voice to make room for another.
    using DropCallback = std::function<void(Voice*)>;

    explicit SoundSystem(dsp::TeakraDsp& dsp);

    bool Initialize(std::span<const uint8_t> component, const DspDefaults& defaults);

    // Allocates a free voice (nn::snd AllocVoice). When all 24 are in use, it frees the lowest-priority one to make
    // room and calls that voice's drop callback, or returns nullptr if that one outranks the request or has priority
    // 0x7fff. nw::snd uses at most 23 voices at once, so this only happens when some of them are stereo.
    Voice* AllocVoice(int priority, DropCallback on_drop = {});
    void FreeVoice(Voice* voice);

    // nn::snd Voice::SetPriority: re-sorts the voice in the drop-order list.
    void ChangePriority(Voice* voice, int priority);

    // Runs the DSP to the end of the current frame and processes the returned statuses.
    bool WaitForDspSync();

    // Writes all voice and DSP parameters and commits the frame.
    void SendParameterToDsp();

private:
    void WriteDspDefaults(const DspDefaults& defaults);
    void InsertByPriority(Voice* voice);

    DspLink link_;
    std::array<Voice, kNumSources> voices_;
    std::array<bool, kNumSources> allocated_{};
    std::array<DropCallback, kNumSources> drop_callbacks_;
    std::list<Voice*> priority_list_; // highest priority first
};

} // namespace threesf::nnsnd
