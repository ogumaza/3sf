// SPDX-License-Identifier: MIT

// Model of nn::snd::Voice and its internal VoiceImpl (CTR SDK 5.2 as linked into Pokemon X).
//
// Field layout and behaviour follow the game's code.bin:
//   0x186134 VoiceImpl::UpdateParameters (dirty bits: 1 gains, 2 pitch, 4 filter enable,
//            8 mono filter, 0x10 biquad, 0x20 interpolation, 0x8000 sync count)
//   0x18f678 VoiceImpl::UpdateWaveBuffers (embedded buffer + 4 queue slots, 5 in flight)
//   0x18f46c status processing, 0x18f5d4 AppendWaveBuffer, 0x18da98 SetState
//   0x1932f0/0x1930e4/0x193094/0x19324c/0x1914e0/0x191534/0x18b2c0 shared-memory writers

#pragma once

#include <array>
#include <cstdint>

#include "common/common_types.h"

namespace threesf::nnsnd
{

class DspLink;

enum class SampleFormat : uint8_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kAdpcm = 2
};

enum class InterpolationType : uint8_t
{
    kPolyphase = 0,
    kLinear = 1,
    kNone = 2
};

enum class VoiceState : uint8_t
{
    kPlay = 0,
    kStop = 1,
    kPause = 2
};

struct AdpcmContext
{
    uint16_t pred_scale = 0;
    int16_t yn1 = 0;
    int16_t yn2 = 0;
};

struct WaveBuffer
{
    enum class Status : uint8_t
    {
        kFree = 0,
        kWait = 1,
        kPlay = 2,
        kDone = 3
    };

    void Initialize() // 0x1860dc
    {
        *this = WaveBuffer{};
    }

    // Set by the user (nn::snd::WaveBuffer).
    PAddr buffer_address = 0; // physical address (nn::snd adds 0x0C000000 to the linear vaddr)
    uint32_t sample_length = 0;
    const AdpcmContext* adpcm_context = nullptr;
    bool loop = false;

    // Managed by nn::snd.
    Status status = Status::kFree;
    uint16_t buffer_id = 0;
    WaveBuffer* next = nullptr;
};

// 12 gains: main bus FL,FR,RL,RR, aux A FL,FR,RL,RR, aux B FL,FR,RL,RR.
using MixParam = std::array<float, 12>;

class Voice
{
public:
    explicit Voice(int id = 0) : id_(id)
    {
    }

    int Id() const
    {
        return id_;
    }

    // The nn::snd::Voice API. The setters mark the VoiceImpl dirty.
    void SetChannelCount(int count);
    void SetSampleFormat(SampleFormat format);
    void SetSampleRate(uint32_t rate);
    void SetPitch(float pitch);            // 0x17f29c (clamped to >= 0)
    void SetVolume(float volume);          // 0x186494
    void SetMixParam(const MixParam& mix); // mix gains (VoiceImpl +0x38..+0x64)
    void SetInterpolationType(InterpolationType type);
    void SetAdpcmParam(const int16_t coefs[16]); // copied to DSP coefficients by UpdateParameters
    void EnableMonoFilter(bool enable);          // 0x1863d0
    void EnableBiquadFilter(bool enable);        // 0x17f1f0
    void SetMonoFilterCoefficients(int16_t b0, int16_t a1);
    void SetBiquadFilterCoefficients(const int16_t* coefs); // 0x18641c: b0, b1, b2, a1, a2
    void AppendWaveBuffer(WaveBuffer* buffer);              // 0x18f5d4
    void SetState(VoiceState state);                        // 0x18da98
    void SetFrontBypass(bool bypass);                       // 0x18da60: format bit 4, with no dirty bit

    void SetPriority(int priority)
    {
        priority_ = priority;
    }

    VoiceState State() const
    {
        return state_;
    }

    int Priority() const
    {
        return priority_;
    }

    const WaveBuffer* WaveBufferHead() const
    {
        return wave_head_;
    }

    // nn::snd's internals, which SoundSystem calls every frame.
    void ApplyPendingState(DspLink& link); // the DSP side of SetState: 0x18edc4 and 0x18eecc
    void UpdateParameters(DspLink& link);  // 0x186134
    void UpdateWaveBuffers(DspLink& link); // 0x18f678
    void ProcessStatus(DspLink& link);     // 0x18f46c
    void ApplySyncCount(DspLink& link);    // 0x18f430
    void EnableOnDsp(DspLink& link);       // 0x18f68c

    bool NeedsEnable() const
    {
        return !enabled_on_dsp_;
    }

    // The DSP cycles the voice takes a frame, as UpdateParameters last worked them out.
    uint32_t DspCost() const
    {
        return dsp_cost_;
    }

    // Called when the voice is (re)allocated (0x191404).
    void Reset();

    void MarkAllDirty();
    void BumpSyncCounter();

private:
    void ResetWaveBuffers(); // 0x18d9f4

    // The DSP cycles a frame for the voice's format, rate, interpolation, filters and buses (0x1862ac).
    uint32_t ComputeDspCost() const;

    int id_;
    int priority_ = 0;

    // VoiceImpl
    int16_t sync_counter_ = 0;             // +0x04
    uint16_t next_buffer_id_ = 0;          // +0x06
    bool enabled_on_dsp_ = false;          // +0x0c: status.is_enabled == 1, also set when enable is written
    VoiceState state_ = VoiceState::kStop; // +0x0d
    InterpolationType interpolation_ = InterpolationType::kPolyphase; // +0x0e
    uint8_t filter_flags_ = 0;                                        // +0x0f bit0 mono, bit1 biquad
    int16_t mono_b0_ = 0, mono_a1_ = 0;                               // +0x10
    std::array<int16_t, 5> biquad_coefs_{};                           // +0x14: b0, b1, b2, a1, a2 (Reset keeps them)
    uint16_t format_flags_ = 1;                                       // +0x1e: bits0-1 channel count, bits2-3 format
    uint32_t sample_rate_ = 32728;                                    // +0x20
    float pitch_ = 1.0f;                                              // +0x24
    float rate_ = 1.0f;                                               // +0x28
    uint32_t dsp_cost_ = 0;                                           // +0x2c
    WaveBuffer* wave_head_ = nullptr;                                 // +0x30
    int16_t queued_ = 0;                                              // +0x34
    int16_t queue_slot_ = 0;                                          // +0x36
    MixParam mix_{};                                                  // +0x38
    float volume_ = 1.0f;                                             // +0x68
    uint16_t dirty_ = 0;                                              // +0x6c
    bool pending_disable_ = false;
    bool pending_reset_ = false;
    std::array<int16_t, 16> adpcm_coefs_{};
    bool adpcm_coefs_dirty_ = false;
};

} // namespace threesf::nnsnd
