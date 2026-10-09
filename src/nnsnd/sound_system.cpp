// SPDX-License-Identifier: MIT

#include "nnsnd/sound_system.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace threesf::nnsnd
{
namespace
{

// A voice of this priority is never dropped to make room or for want of DSP cycles.
constexpr int kMaxPriority = 0x7fff;

// The DSP cycles that nn::snd lets a frame take (0x4c0eb4).
constexpr int32_t kFrameCycles = 622535;

// The DSP cycles the output takes a frame (0x18d548), by output mode and clipping mode. With headphones in (the shared
// page's flag), surround takes 199,600. The model never sets that flag.
int32_t OutputCycles(uint16_t output_mode, uint16_t clipping_mode)
{
    int32_t cycles = 52600;
    switch (output_mode)
    {
    case 0:
        cycles = 57100;
        break;
    case 1:
        cycles = 55600;
        break;
    case 2:
        cycles = 225600;
        break;
    default:
        break;
    }

    if (clipping_mode == 0)
    {
        cycles += 1500;
    }
    else if (clipping_mode == 1)
    {
        cycles += 10000;
    }

    return cycles;
}

} // namespace

SoundSystem::SoundSystem(dsp::TeakraDsp& dsp) : link_(dsp)
{
    for (int i = 0; i < kNumSources; i++)
    {
        voices_[i] = Voice(i);
    }
}

bool SoundSystem::Initialize(std::span<const uint8_t> component, const DspDefaults& defaults)
{
    if (!link_.Initialize(component))
    {
        return false;
    }

    WriteDspDefaults(defaults);

    // 0x148190: every voice's parameters are written once with all dirty bits set.
    for (auto& v : voices_)
    {
        v.MarkAllDirty();
        v.UpdateParameters(link_);
    }

    return true;
}

void SoundSystem::WriteDspDefaults(const DspDefaults& defaults)
{
    // 0x148470 / 0x14862c: master volume, aux return volumes, aux bus enables, front bypass, rear ratio, surround
    // depth, clipping mode, output buffer count, output format, sync mode.
    auto& c = link_.Mixer();
    c.master_volume = 1.0f;
    c.aux_return[0] = 1.0f;
    c.aux_return[1] = 1.0f;
    c.aux_enable[0] = 0;
    c.aux_enable[1] = 0;
    c.aux_bypass[0] = 0;
    c.aux_bypass[1] = 0;
    c.rear_ratio = 0x8000;
    c.surround_depth = 0x7fff;
    c.limiter = defaults.clipping_mode;
    c.output_buffers = 2;
    c.output_mode = defaults.output_format;
    c.dirty |= (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 15) | (1u << 16) | (1u << 24) | (1u << 25) |
               (1u << 26) | (1u << 27) | (1u << 29) | (1u << 31);
    c.sync_mode = 1; // MixerParams +0xbc: nn::snd writes the system sound mode here
    c.dirty2 |= 1u << 16;

    // The voices get what the output leaves of the frame's cycles (0x18b380). The aux buses' effects (0x18d2f4) take
    // none: the model has none. When its voice manager's flag at +0xe is set, nn::snd also takes off the cycles over
    // 655,300 that the DSP last reported. The game doesn't set the flag.
    voice_cycles_ = kFrameCycles - OutputCycles(defaults.output_format, defaults.clipping_mode);
}

Voice* SoundSystem::AllocVoice(int priority, DropCallback on_drop)
{
    // 0x18eb6c
    if (priority >= 0x8000)
    {
        return nullptr;
    }

    if (priority_list_.size() == kNumSources)
    {
        // All voices busy: drop the lowest-priority one unless it outranks the request, and tell its owner.
        Voice* lowest = priority_list_.back();
        if (lowest->Priority() == kMaxPriority || lowest->Priority() > priority)
        {
            return nullptr;
        }

        DropVoice(lowest);
    }

    for (int i = 0; i < kNumSources; i++)
    {
        if (allocated_[i])
        {
            continue;
        }

        allocated_[i] = true;
        drop_callbacks_[i] = std::move(on_drop);
        Voice& v = voices_[i];
        v.Reset();
        v.SetPriority(priority);
        InsertByPriority(&v);
        v.SetState(VoiceState::kPause); // VoiceImpl::SetState(Pause): enable = 0
        v.BumpSyncCounter();
        return &v;
    }

    return nullptr;
}

void SoundSystem::InsertByPriority(Voice* voice)
{
    // The list runs from the highest priority to the lowest; a voice goes in front of voices of equal priority, so the
    // tail is the oldest of the lowest priority (0x18ec58).
    auto it = std::find_if(priority_list_.begin(), priority_list_.end(),
                           [&](Voice* v) { return v->Priority() <= voice->Priority(); });
    priority_list_.insert(it, voice);
}

void SoundSystem::FreeVoice(Voice* voice)
{
    // 0x18ecfc
    if (!voice || !allocated_[voice->Id()])
    {
        return;
    }

    const int i = voice->Id();

    if (voice->State() != VoiceState::kStop)
    {
        voice->SetState(VoiceState::kStop);
    }

    allocated_[i] = false;
    drop_callbacks_[i] = nullptr;
    priority_list_.remove(voice);
}

void SoundSystem::DropVoice(Voice* voice)
{
    // The owner's callback runs once the voice is free.
    DropCallback dropped = std::move(drop_callbacks_[voice->Id()]);
    FreeVoice(voice);
    if (dropped)
    {
        dropped(voice);
    }
}

void SoundSystem::ChangePriority(Voice* voice, int priority)
{
    priority_list_.remove(voice);
    voice->SetPriority(priority);
    InsertByPriority(voice);
}

bool SoundSystem::WaitForDspSync()
{
    if (!link_.WaitForFrame())
    {
        return false;
    }

    for (auto& v : voices_)
    {
        v.ProcessStatus(link_);
    }

    return true;
}

void SoundSystem::SendParameterToDsp()
{
    // 0x18d368 / 0x18d514
    for (auto& v : voices_)
    {
        v.UpdateParameters(link_);
    }

    for (auto& v : voices_)
    {
        v.UpdateWaveBuffers(link_);
    }

    // 0x18d39c: in priority order, playing voices with wave buffers get their sync count and enable written while
    // their DSP cycles fit what the frame has left. A voice that doesn't fit is dropped unless its priority is the
    // highest. The walk goes over a copy of the order: a dropped voice's owner can free others, and the game's walk
    // passes over them as stopped. The arithmetic is the ARM's, in 32 bits with signed comparisons.
    int32_t cycles = voice_cycles_;
    const std::vector<Voice*> order(priority_list_.begin(), priority_list_.end());
    for (Voice* v : order)
    {
        if (v->State() != VoiceState::kPlay || !v->WaveBufferHead())
        {
            continue;
        }

        const auto cost = static_cast<int32_t>(v->DspCost());
        if (cost > cycles && v->Priority() != kMaxPriority)
        {
            DropVoice(v);
            continue;
        }

        v->ApplySyncCount(link_);
        if (v->NeedsEnable())
        {
            v->EnableOnDsp(link_);
        }

        cycles = static_cast<int32_t>(static_cast<uint32_t>(cycles) - static_cast<uint32_t>(cost));
    }

    // The game writes the DSP side of a stop straight into the frame being prepared (see Voice::SetState). So a voice
    // that the walk dropped, and the other channel that its owner freed, go silent in this frame.
    for (auto& v : voices_)
    {
        v.ApplyPendingState(link_);
    }

    link_.Commit();
}

SoundSystem::VoicePlace SoundSystem::PlaceOf(int id) const
{
    const Voice& voice = voices_[id];
    const SourceReport& report = link_.SourceReportOf(id);

    VoicePlace place;
    place.playing = allocated_[id] && voice.State() == VoiceState::kPlay && report.enabled != 0;
    place.position = report.position;
    for (const WaveBuffer* buffer = voice.WaveBufferHead(); buffer; buffer = buffer->next)
    {
        place.loops = place.loops || buffer->loop;
    }

    return place;
}

} // namespace threesf::nnsnd
