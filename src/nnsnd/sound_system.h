// SPDX-License-Identifier: MIT

// Model of nn::snd's global state: 24 voices, the DSP link, the DSP configuration, and the per-frame receive/send
// sequence (code.bin 0x185f50 / 0x186070 / 0x18b40c / 0x18b2d4).

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <span>
#include <utility>

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

// The owner of a voice: the place of the nw::snd voice that holds it, and whether that voice is stereo. AllocVoice
// records it. A voice without an owner has place -1.
struct DropOwner
{
    int place = -1;
    bool stereo = false;
};

class SoundSystem
{
public:
    // Called with a voice's owner after nn::snd has dropped the voice.
    using DropHandler = std::function<void(DropOwner owner, Voice* dropped)>;

    // Where a DSP voice is in its wave, for the ripper's length analysis. A voice plays while it's allocated, its state
    // is play and the DSP reports it enabled. Its wave loops when one of its queued buffers loops. The position counts
    // the samples into the buffer that the DSP plays. It falls each time the wave goes back to its loop start.
    struct VoicePlace
    {
        bool playing = false;
        bool loops = false;
        uint32_t position = 0;
    };

    explicit SoundSystem(dsp::TeakraDsp& dsp);

    bool Initialize(std::span<const uint8_t> component, const DspDefaults& defaults);

    // Allocates a free voice (nn::snd AllocVoice). When all 24 are in use, it drops the lowest-priority one to make
    // room. It returns nullptr instead if that one outranks the request or has priority 0x7fff. nw::snd uses at most 23
    // voices at once. A drop here happens only when some of them are stereo.
    Voice* AllocVoice(int priority, DropOwner owner = {});

    // Sets the handler of drops. The nw::snd engine's voice manager sets it once.
    void SetDropHandler(DropHandler handler)
    {
        drop_handler_ = std::move(handler);
    }

    void FreeVoice(Voice* voice);

    // nn::snd Voice::SetPriority: re-sorts the voice in the drop-order list.
    void ChangePriority(Voice* voice, int priority);

    // Runs the DSP to the end of the current frame and processes the returned statuses.
    bool WaitForDspSync();

    // Writes all voice and DSP parameters and commits the frame.
    void SendParameterToDsp();

    VoicePlace PlaceOf(int id) const;

private:
    void WriteDspDefaults(const DspDefaults& defaults);
    void InsertByPriority(Voice* voice);

    // Frees a voice and calls the drop handler with the voice's owner (0x18d444). nn::snd drops a voice to make room
    // for another, or because the voice doesn't fit the frame's DSP cycles.
    void DropVoice(Voice* voice);

    DspLink link_;
    std::array<Voice, kNumSources> voices_;
    std::array<bool, kNumSources> allocated_{};
    std::array<DropOwner, kNumSources> drop_owners_{};
    DropHandler drop_handler_;
    std::list<Voice*> priority_list_; // highest priority first
    int32_t voice_cycles_ = 0;        // the DSP cycles a frame that voices may take
};

} // namespace threesf::nnsnd
