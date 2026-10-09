// SPDX-License-Identifier: MIT

#include "nnsnd/voice.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nnsnd/dsp_link.h"

namespace threesf::nnsnd
{
namespace
{

// Dirty bits of VoiceImpl +0x6c.
constexpr uint16_t kDirtyGains = 0x1;
constexpr uint16_t kDirtyPitch = 0x2;
constexpr uint16_t kDirtyFilterEnable = 0x4;
constexpr uint16_t kDirtyMonoFilter = 0x8;
constexpr uint16_t kDirtyBiquad = 0x10;
constexpr uint16_t kDirtyInterpolation = 0x20;
constexpr uint16_t kDirtySyncCount = 0x8000;

// SourceParams::dirty bits written by nn::snd.
constexpr uint32_t kCfgPartialReset = 1u << 4;    // 0x193094
constexpr uint32_t kCfgAdpcmCoefs = 1u << 2;      // 0x18b2c0
constexpr uint32_t kCfgEnable = 1u << 16;         // 0x1914e0 / 0x18edc4
constexpr uint32_t kCfgInterpolation = 1u << 17;  // 0x179478
constexpr uint32_t kCfgRate = 1u << 18;           // 0x1794e0
constexpr uint32_t kCfgBufferQueue = 1u << 19;    // 0x1930e4
constexpr uint32_t kCfgPlayPosition = 1u << 21;   // 0x1932f0
constexpr uint32_t kCfgFiltersEnabled = 1u << 22; // 0x1797e0
constexpr uint32_t kCfgSimpleFilter = 1u << 23;   // 0x179838
constexpr uint32_t kCfgBiquad = 1u << 24;         // 0x179898
constexpr uint32_t kCfgGains = 7u << 25;          // 0x17f2bc
constexpr uint32_t kCfgSyncCount = 1u << 28;      // 0x191534
constexpr uint32_t kCfgReset = 1u << 29;          // 0x18eecc
constexpr uint32_t kCfgEmbeddedBuffer = 1u << 30; // 0x1932f0

// 1/32728 (code.bin 0x1863b0, 0x3800280d): the DSP output rate used to turn pitch into a rate multiplier. With these
// exact bits, a 32728 Hz wave at unison gets a rate of exactly 1.0, which the firmware's polyphase interpolator passes
// through unfiltered. A rounded decimal literal would give 1.0000007, and audibly duller treble.
const float kInvDspRate = std::bit_cast<float>(0x3800280du);

// Polyphase sub-mode threshold (code.bin 0x1863b8).
constexpr float kPolyphaseDownsampleThreshold = 4.0f / 3.0f;

// The DSP cycles a voice takes a frame (UpdateParameters, 0x1862ac), from the tables at 0x56d860. Each has a column
// for one channel and one for two. The sample format's cycles scale with the rate and add to 1,400 (0x1863c4).
constexpr float kBaseCycles = 1400.0f;
constexpr std::array<std::array<int32_t, 2>, 3> kFormatCycles = {{{1200, 1750}, {1100, 1800}, {3000, 0}}};
constexpr std::array<std::array<uint32_t, 2>, 3> kInterpolationCycles = {{{800, 1300}, {1800, 3100}, {1800, 2900}}};
// By the filter flags: none, the one-pole filter, the biquad, or both.
constexpr std::array<std::array<uint32_t, 2>, 4> kFilterCycles = {
    {{300, 150}, {850, 1200}, {2400, 4000}, {3250, 5200}}};
// For each bus the voice mixes into: the main bus, and aux A or B when any of its gains isn't zero (0x18638c: 275 * 8).
constexpr uint32_t kBusCycles = 2200;

// The ARM11's VFP conversion to an unsigned integer (vcvt.u32.f32): it rounds towards zero, saturates, and turns NaN
// into 0.
uint32_t VfpToUnsigned(float value)
{
    if (!(value > 0.0f))
    {
        return 0;
    }

    return value >= 4294967296.0f ? UINT32_MAX : static_cast<uint32_t>(value);
}

} // namespace

void Voice::Reset()
{
    // 0x191404 -> 0x1934b0. The VoiceImpl keeps its sync counter and its pending dirty bits (notably 0x8000 from the
    // Stop that freed it), which is what lets the next ApplySyncCount resynchronise the DSP's status reports with this
    // voice.
    state_ = VoiceState::kPause;
    enabled_on_dsp_ = false;
    format_flags_ = static_cast<uint16_t>((format_flags_ & ~0x7fu) | 5u); // mono PCM16

    volume_ = 1.0f;
    mix_ = {};
    mix_[0] = 1.0f;
    mix_[1] = 1.0f;
    dirty_ |= kDirtyGains;

    sample_rate_ = 32728;
    pitch_ = 1.0f;
    dirty_ |= kDirtyPitch;

    interpolation_ = InterpolationType::kPolyphase;
    dirty_ |= kDirtyInterpolation;

    filter_flags_ = 0;
    dirty_ |= kDirtyFilterEnable;
    mono_b0_ = 0;
    mono_a1_ = 0;

    wave_head_ = nullptr;
    queued_ = 0;
    queue_slot_ = 0;
    next_buffer_id_ = 0;
}

void Voice::MarkAllDirty()
{
    // 0x186128 (called for every voice by nn::snd initialisation, 0x148190)
    dirty_ = 0xffff;
}

void Voice::BumpSyncCounter()
{
    // nn::snd AllocVoice (0x18ece0) advances the counter of the voice it hands out.
    sync_counter_++;
}

void Voice::SetChannelCount(int count)
{
    format_flags_ = static_cast<uint16_t>((format_flags_ & ~3u) | (count & 3));
}

void Voice::SetSampleFormat(SampleFormat format)
{
    format_flags_ = static_cast<uint16_t>((format_flags_ & ~0xcu) | (static_cast<uint16_t>(format) << 2));
}

void Voice::SetSampleRate(uint32_t rate)
{
    sample_rate_ = rate;
    dirty_ |= kDirtyPitch;
}

void Voice::SetPitch(float pitch)
{
    pitch_ = std::max(pitch, 0.0f);
    dirty_ |= kDirtyPitch;
}

void Voice::SetVolume(float volume)
{
    volume_ = volume;
    dirty_ |= kDirtyGains;
}

void Voice::SetMixParam(const MixParam& mix)
{
    mix_ = mix;
    dirty_ |= kDirtyGains;
}

void Voice::SetInterpolationType(InterpolationType type)
{
    interpolation_ = type;
    dirty_ |= kDirtyInterpolation;
}

void Voice::SetAdpcmParam(const int16_t coefs[16])
{
    std::copy(coefs, coefs + 16, adpcm_coefs_.begin());
    adpcm_coefs_dirty_ = true;
}

void Voice::EnableMonoFilter(bool enable)
{
    filter_flags_ = static_cast<uint8_t>(enable ? (filter_flags_ | 1) : (filter_flags_ & ~1));
    dirty_ |= kDirtyFilterEnable;
}

void Voice::EnableBiquadFilter(bool enable)
{
    filter_flags_ = static_cast<uint8_t>(enable ? (filter_flags_ | 2) : (filter_flags_ & ~2));
    dirty_ |= kDirtyFilterEnable;
}

void Voice::SetBiquadFilterCoefficients(const int16_t* coefs)
{
    std::copy(coefs, coefs + 5, biquad_coefs_.begin());
    dirty_ |= kDirtyBiquad;
}

void Voice::SetFrontBypass(bool bypass)
{
    // There's no dirty bit for it: the DSP gets the flag with the format of the next first buffer.
    format_flags_ = static_cast<uint16_t>((format_flags_ & ~0x10u) | (bypass ? 0x10u : 0u));
}

void Voice::SetMonoFilterCoefficients(int16_t b0, int16_t a1)
{
    mono_b0_ = b0;
    mono_a1_ = a1;
    dirty_ |= kDirtyMonoFilter;
}

void Voice::AppendWaveBuffer(WaveBuffer* buffer)
{
    // 0x18f5d4
    if (buffer->sample_length == 0)
    {
        buffer->status = WaveBuffer::Status::kDone;
        return;
    }

    buffer->next = nullptr;
    buffer->status = WaveBuffer::Status::kWait;
    if (!wave_head_)
    {
        wave_head_ = buffer;
    }
    else
    {
        WaveBuffer* p = wave_head_;
        while (p->next)
        {
            p = p->next;
        }
        p->next = buffer;
    }

    if (next_buffer_id_ == 0)
    {
        next_buffer_id_ = 1;
    }

    buffer->buffer_id = next_buffer_id_++;
}

void Voice::ResetWaveBuffers()
{
    // 0x18d9f4
    for (WaveBuffer* p = wave_head_; p; p = p->next)
    {
        p->status = WaveBuffer::Status::kDone;
    }

    wave_head_ = nullptr;
    queued_ = 0;
    queue_slot_ = 0;
    dirty_ |= kDirtySyncCount;
    sync_counter_++;
}

void Voice::SetState(VoiceState state)
{
    // 0x18da98 -> VoiceImpl::SetState 0x18dac4. The game writes the DSP side of Stop/Pause (enable = 0 via 0x18edc4;
    // for Stop also the source reset flag via 0x18eecc) straight into the region being prepared. Here they wait for
    // ApplyPendingState. UpdateParameters calls it in the same frame before the frame counter is committed, and
    // SoundSystem calls it again after the walk that drops voices for want of DSP cycles.
    if (state == VoiceState::kStop)
    {
        ResetWaveBuffers();
    }

    state_ = state;

    if (state == VoiceState::kStop)
    {
        pending_disable_ = true;
        pending_reset_ = true;
        enabled_on_dsp_ = false;
    }
    else if (state == VoiceState::kPause)
    {
        pending_disable_ = true;
    }
}

void Voice::ApplyPendingState(DspLink& link)
{
    auto& cfg = link.SourceParamsOf(id_);
    if (pending_disable_)
    {
        // 0x18edc4: enable = 0
        cfg.enable = 0;
        cfg.dirty |= kCfgEnable;
        pending_disable_ = false;
    }

    if (pending_reset_)
    {
        // 0x18eecc: source reset flag
        cfg.dirty |= kCfgReset;
        pending_reset_ = false;
    }
}

void Voice::UpdateParameters(DspLink& link)
{
    // 0x186134
    ApplyPendingState(link);

    auto& cfg = link.SourceParamsOf(id_);
    if (adpcm_coefs_dirty_)
    {
        // 0x18b2c0: coefficients go to the region being written, dirty bit 2 in the config.
        int16_t* coefs = link.AdpcmCoefs(id_);
        std::memcpy(coefs, adpcm_coefs_.data(), sizeof(int16_t) * 16);
        cfg.dirty |= kCfgAdpcmCoefs;
        adpcm_coefs_dirty_ = false;
    }

    // A change of the gains, the rate or the interpolation recomputes the voice's DSP cost. As in the game, a change
    // of the filters alone doesn't.
    bool cost_changes = false;
    if (dirty_ & kDirtyGains)
    {
        // 0x17990c -> 0x17f2bc
        cost_changes = true;
        for (int bus = 0; bus < 3; bus++)
        {
            for (int ch = 0; ch < 4; ch++)
            {
                cfg.gain[bus][ch] = mix_[bus * 4 + ch] * volume_;
            }
        }
        cfg.dirty |= kCfgGains;
    }

    if (dirty_ & kDirtyPitch)
    {
        cost_changes = true;
        rate_ = pitch_ * (static_cast<float>(static_cast<int32_t>(sample_rate_)) * kInvDspRate);
        cfg.rate = rate_;
        cfg.dirty |= kCfgRate;
        if (interpolation_ == InterpolationType::kPolyphase)
        {
            dirty_ |= kDirtyInterpolation;
        }
    }

    if (dirty_ & kDirtyFilterEnable)
    {
        cfg.filters = filter_flags_;
        cfg.dirty |= kCfgFiltersEnabled;
    }

    if (dirty_ & kDirtyMonoFilter)
    {
        cfg.one_pole_b0 = mono_b0_;
        cfg.one_pole_a1 = mono_a1_;
        cfg.dirty |= kCfgSimpleFilter;
    }

    if (dirty_ & kDirtyBiquad)
    {
        cfg.biquad_b0 = biquad_coefs_[0];
        cfg.biquad_b1 = biquad_coefs_[1];
        cfg.biquad_b2 = biquad_coefs_[2];
        cfg.biquad_a1 = biquad_coefs_[3];
        cfg.biquad_a2 = biquad_coefs_[4];
        cfg.dirty |= kCfgBiquad;
    }

    if (dirty_ & kDirtyInterpolation)
    {
        // 0x179478: mode byte, plus a rate-dependent sub-mode byte for mode 0.
        uint8_t mode = 2;
        uint8_t submode = 1;
        if (interpolation_ == InterpolationType::kPolyphase)
        {
            mode = 0;
            submode = rate_ <= kPolyphaseDownsampleThreshold ? 1 : 0;
            if (submode && rate_ <= 1.0f)
            {
                submode = 2;
            }
        }
        else if (interpolation_ == InterpolationType::kLinear)
        {
            mode = 1;
        }

        cfg.interpolation = mode;
        if (mode == 0)
        {
            cfg.polyphase_kernel = submode;
        }

        cfg.dirty |= kCfgInterpolation;
        cost_changes = true;
    }

    if (cost_changes)
    {
        dsp_cost_ = ComputeDspCost();
    }

    dirty_ &= kDirtySyncCount;
}

uint32_t Voice::ComputeDspCost() const
{
    // 0x1862ac. Channel counts are 1 or 2, and the indices stay in the tables.
    const std::size_t channels = (format_flags_ & 3) == 2 ? 1 : 0;
    const auto format = std::min<std::size_t>(static_cast<std::size_t>((format_flags_ >> 2) & 3), 2);
    const auto interpolation = std::min<std::size_t>(static_cast<std::size_t>(interpolation_), 2);
    const auto filters = static_cast<std::size_t>(filter_flags_ & 3);

    // vmla rounds the product before it adds.
    const float format_cycles = static_cast<float>(kFormatCycles[format][channels]) * rate_;
    const uint32_t cost = VfpToUnsigned(kBaseCycles + format_cycles) + kInterpolationCycles[interpolation][channels] +
                          kFilterCycles[filters][channels];

    // The gains are compared as bits: -0.0 counts as a gain.
    uint32_t buses = 1;
    for (int bus = 1; bus < 3; bus++)
    {
        const bool used = std::any_of(mix_.begin() + bus * 4, mix_.begin() + bus * 4 + 4,
                                      [](float gain) { return std::bit_cast<uint32_t>(gain) != 0; });
        buses += used ? 1 : 0;
    }

    return cost + buses * kBusCycles;
}

void Voice::UpdateWaveBuffers(DspLink& link)
{
    // 0x18f678
    if (state_ != VoiceState::kPlay)
    {
        return;
    }

    WaveBuffer* p = wave_head_;
    for (int i = queued_; i != 0 && p; i--)
    {
        p = p->next;
    }

    auto& cfg = link.SourceParamsOf(id_);
    for (int n = queued_; n < 5; n++)
    {
        if (!p)
        {
            break;
        }

        if (queued_ == 0)
        {
            // The first buffer goes into the embedded buffer (0x1932f0).
            queue_slot_ = 0;
            cfg.dirty |= kCfgPartialReset; // 0x193094
            p->status = WaveBuffer::Status::kPlay;
            const SampleFormat fmt = static_cast<SampleFormat>((format_flags_ >> 2) & 3);
            cfg.buffer_id = p->buffer_id;
            cfg.length = p->sample_length;
            cfg.address = p->buffer_address;
            cfg.format = format_flags_;
            cfg.SetLooping(p->loop);

            if (fmt == SampleFormat::kAdpcm)
            {
                if (p->adpcm_context)
                {
                    cfg.adpcm_ps = p->adpcm_context->pred_scale;
                    cfg.adpcm_yn[0] = static_cast<uint16_t>(p->adpcm_context->yn1);
                    cfg.adpcm_yn[1] = static_cast<uint16_t>(p->adpcm_context->yn2);
                    cfg.SetAdpcmContext(true);
                }
                else
                {
                    cfg.SetAdpcmContext(false);
                }
            }

            cfg.play_position = 0;
            cfg.dirty |= kCfgEmbeddedBuffer | kCfgPlayPosition;
        }
        else
        {
            // Later buffers go into the queue slots (0x1930e4).
            auto& b = cfg.queue[queue_slot_];
            b.buffer_id = p->buffer_id;
            b.address = p->buffer_address;
            b.length = p->sample_length;
            if (p->adpcm_context)
            {
                b.adpcm_ps = p->adpcm_context->pred_scale;
                b.adpcm_yn[0] = static_cast<uint16_t>(p->adpcm_context->yn1);
                b.adpcm_yn[1] = static_cast<uint16_t>(p->adpcm_context->yn2);
                b.adpcm_context = 1;
            }
            else
            {
                b.adpcm_context = 0;
            }

            b.looping = p->loop ? 1 : 0;
            cfg.queue_dirty = static_cast<uint16_t>(cfg.queue_dirty | (1u << queue_slot_));
            cfg.dirty |= kCfgBufferQueue;
            queue_slot_ = static_cast<int16_t>(queue_slot_ + 1);
            if (queue_slot_ > 3)
            {
                queue_slot_ = 0;
            }
        }

        queued_++;
        p = p->next;
    }
}

void Voice::ProcessStatus(DspLink& link)
{
    // 0x18f46c
    const auto& st = link.SourceReportOf(id_);
    if (static_cast<int16_t>(st.sync) == sync_counter_)
    {
        if (st.buffer_changed != 0 && wave_head_)
        {
            const uint16_t cur = st.current_buffer;
            const uint16_t last = st.last_buffer;
            WaveBuffer* p = wave_head_;
            std::array<WaveBuffer*, 5> finished{};
            int nfinished = 0;
            if (queued_ != 0)
            {
                // Buffers before the current one have finished. The search can stop early, before it gets there.
                bool stopped = false;
                while (p->buffer_id != cur)
                {
                    queued_--;
                    const int16_t remaining = queued_;
                    finished[nfinished++] = p;
                    const bool all_done = cur == 0 && (p->buffer_id == last || last == 0);
                    p = p->next;
                    if (all_done || remaining == 0 || !p)
                    {
                        stopped = true;
                        break;
                    }
                }

                if (!stopped)
                {
                    p->status = WaveBuffer::Status::kPlay;
                }
            }

            if (cur == 0)
            {
                queued_ = 0;
            }

            for (int i = 0; i < nfinished; i++)
            {
                finished[i]->status = WaveBuffer::Status::kDone;
            }

            wave_head_ = p;
        }
    }

    enabled_on_dsp_ = st.enabled == 1;
}

void Voice::ApplySyncCount(DspLink& link)
{
    // 0x18f430 -> 0x191534
    if (dirty_ & kDirtySyncCount)
    {
        auto& cfg = link.SourceParamsOf(id_);
        cfg.sync = static_cast<uint16_t>(sync_counter_);
        cfg.dirty |= kCfgSyncCount;
        dirty_ &= static_cast<uint16_t>(~kDirtySyncCount);
    }
}

void Voice::EnableOnDsp(DspLink& link)
{
    // 0x18f68c -> 0x1914e0
    auto& cfg = link.SourceParamsOf(id_);
    cfg.enable = 1;
    cfg.dirty |= kCfgEnable;
    enabled_on_dsp_ = true;
}

} // namespace threesf::nnsnd
