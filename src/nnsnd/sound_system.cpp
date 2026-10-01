// SPDX-License-Identifier: MIT

#include "nnsnd/sound_system.h"

#include <algorithm>
#include <cstdint>
#include <span>

namespace threesf::nnsnd
{

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
        if (lowest->Priority() == 0x7fff || lowest->Priority() > priority)
        {
            return nullptr;
        }

        DropCallback dropped = std::move(drop_callbacks_[lowest->Id()]);
        FreeVoice(lowest);
        if (dropped)
        {
            dropped(lowest);
        }
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

    // 0x18d39c: playing voices with wave buffers get their sync count and enable written, in priority order.
    for (Voice* v : priority_list_)
    {
        if (v->State() != VoiceState::kPlay || !v->WaveBufferHead())
        {
            continue;
        }

        v->ApplySyncCount(link_);
        if (v->NeedsEnable())
        {
            v->EnableOnDsp(link_);
        }
    }

    link_.Commit();
}

} // namespace threesf::nnsnd
