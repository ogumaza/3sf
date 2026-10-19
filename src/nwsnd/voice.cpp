// SPDX-License-Identifier: MIT

#include "nwsnd/voice.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"

#include "nnsnd/sound_system.h"
#include "nwsnd/engine.h"
#include "nwsnd/util.h"

namespace threesf::nwsnd
{
namespace
{

using ntdlib::nwsnd::CalcPanRatio;
using ntdlib::nwsnd::CalcSurroundPanRatio;
using ntdlib::nwsnd::CtrPolicy;
using ntdlib::nwsnd::OutputMode;

nnsnd::SampleFormat ToNnFormat(SampleFormat format)
{
    switch (format)
    {
    case SampleFormat::kPcm8:
        return nnsnd::SampleFormat::kPcm8;
    case SampleFormat::kDspAdpcm:
        return nnsnd::SampleFormat::kAdpcm;
    default:
        return nnsnd::SampleFormat::kPcm16;
    }
}

float Clamp01(float v)
{
    if (v > 1.0f)
    {
        return 1.0f;
    }
    if (v < 0.0f)
    {
        return 0.0f;
    }

    return v;
}

// Sample offset -> byte offset (code.bin 0x186888). DSP-ADPCM counts in nibbles: 16 for each frame of 14 samples, and
// for a partial frame its header's two and one for each sample. Half the count is the byte that holds the sample.
uint32_t SamplesToBytes(uint32_t samples, SampleFormat format)
{
    switch (format)
    {
    case SampleFormat::kPcm8:
        return samples;

    case SampleFormat::kDspAdpcm:
        {
            const uint32_t rest = samples % 14;
            return (samples / 14 * 16 + (rest ? rest + 2 : 0)) / 2;
        }

    default:
        return samples * 2;
    }
}

} // namespace

// --------------------------------------------------------------------------------------------- Voice
bool Voice::Alloc(int channel_count, int priority, Callback callback)
{
    // 0x18b9ec
    const int count = std::clamp(channel_count, 1, 2);
    const int dsp_priority = std::clamp(priority, 0, 0x7fff);
    nnsnd::SoundSystem& snd = engine_->Snd();
    allocating_ = true;
    channel_count_ = 0;
    alloc_failed_ = false;

    if (count == 1)
    {
        nnsnd::Voice* nn = snd.AllocVoice(dsp_priority, {place_, false});
        if (!nn)
        {
            return false;
        }

        nn_[0] = nn;
        channel_count_ = 1;
    }
    else
    {
        // If nn::snd cannot allocate a channel, drop an nw::snd voice in priority order and retry. nn::snd may also
        // drop this voice's first channel while allocating its second.
        for (int i = 0; i < count; i++)
        {
            nnsnd::Voice* nn = nullptr;
            while (!(nn = snd.AllocVoice(dsp_priority, {place_, true})))
            {
                if (engine_->Voices().DropLowestPriorityVoice(dsp_priority) == 0)
                {
                    alloc_failed_ = true;
                    break;
                }
            }

            nn_[i] = nn;
            channel_count_++;
        }

        if (alloc_failed_)
        {
            for (int i = 0; i < channel_count_; i++)
            {
                if (nn_[i])
                {
                    snd.FreeVoice(nn_[i]);
                }
            }

            return false;
        }
    }

    allocating_ = false;
    callback_ = std::move(callback);
    active_ = true;

    return true;
}

void Voice::Stop()
{
    // 0x18b990
    if (playing_)
    {
        ForEachDspVoice([](nnsnd::Voice* v, int) { v->SetState(nnsnd::VoiceState::kStop); });
        playing_ = false;
    }

    started_ = false;
}

void Voice::OnDspVoiceDropped(nnsnd::Voice* dropped, bool stereo)
{
    // 0x18dc1c (mono), 0x18dc6c (stereo). nn::snd has already freed `dropped`.
    if (!stereo)
    {
        nn_[0] = nullptr;
    }
    else if (allocating_)
    {
        // It dropped this voice's first channel to make room for the second: the allocation fails.
        alloc_failed_ = true;
        for (int i = 0; i < channel_count_; i++)
        {
            if (nn_[i] == dropped)
            {
                nn_[i] = nullptr;
                return;
            }
        }
    }
    else
    {
        for (int i = 0; i < channel_count_; i++)
        {
            if (nn_[i] && nn_[i] != dropped)
            {
                nn_[i]->SetState(nnsnd::VoiceState::kStop);
                engine_->Snd().FreeVoice(nn_[i]);
            }

            nn_[i] = nullptr;
        }
    }

    started_ = false;
    channel_count_ = 0;
    engine_->Voices().Release(place_);
    active_ = false;

    auto cb = callback_;
    if (cb)
    {
        cb(place_, VoiceCallbackStatus::kDropDsp);
    }
}

void Voice::Initialize(const WaveInfo& info)
{
    const auto initialize = [&info](nnsnd::Voice* v, int)
    {
        v->SetChannelCount(1);
        v->SetSampleFormat(ToNnFormat(info.format));
        v->SetSampleRate(info.sample_rate);
        v->SetInterpolationType(nnsnd::InterpolationType::kPolyphase); // HardwareManager's default SRC type, 4-tap
    };
    ForEachDspVoice(initialize);

    started_ = false;
    playing_ = false;
}

void Voice::SetPriority(int p)
{
    priority_ = p;
    engine_->Voices().ChangePriority(place_);

    if (p == 1)
    {
        ForEachDspVoice([this](nnsnd::Voice* v, int) { engine_->Snd().ChangePriority(v, 1); });
    }
}

void Voice::ApplyInterpolationType()
{
    const int type = interpolation_;
    const auto dsp_type = static_cast<nnsnd::InterpolationType>(type == 1 ? 1 : type == 2 ? 2 : 0);
    ForEachDspVoice([dsp_type](nnsnd::Voice* v, int) { v->SetInterpolationType(dsp_type); });
}

void Voice::Free(Engine& engine)
{
    engine.Voices().FreeVoice(place_);
}

void Voice::SetOutputs(const Outputs& outputs)
{
    SetPan(outputs.pan);
    SetSurroundPan(outputs.span);
    SetMainSend(outputs.main_send);
    for (int i = 0; i < 2; i++)
    {
        SetFxSend(i, outputs.fx_send[i]);
    }
}

void Voice::SetVolume(float v)
{
    if (v < 0.0f)
    {
        v = 0.0f;
    }

    if (volume_ != v)
    {
        volume_ = v;
        dirty_ |= kVolume;
    }
}

void Voice::SetPitch(float p)
{
    if (pitch_ != p)
    {
        pitch_ = p;
        dirty_ |= kPitch;
    }
}

void Voice::SetPan(float p)
{
    if (pan_ != p)
    {
        pan_ = p;
        dirty_ |= kMix;
    }
}

void Voice::SetSurroundPan(float p)
{
    if (span_ != p)
    {
        span_ = p;
        dirty_ |= kMix;
    }
}

void Voice::SetLpfFreq(float f)
{
    if (lpf_ != f)
    {
        lpf_ = f;
        dirty_ |= kLpf;
    }
}

void Voice::SetBiquadFilter(int type, float value)
{
    value = Clamp01(value);
    bool changed = false;
    if (biquad_type_ != type)
    {
        biquad_type_ = static_cast<uint8_t>(type);
        changed = true;
    }

    if (biquad_value_ != value)
    {
        biquad_value_ = value;
        changed = true;
    }

    if (changed)
    {
        dirty_ |= kBiquad;
    }
}

void Voice::SetFrontBypass(bool bypass)
{
    // It changes no dirty bit: the DSP gets the flag with its voices' first buffers, and the mix with its next update.
    ForEachDspVoice([bypass](nnsnd::Voice* v, int) { v->SetFrontBypass(bypass); });
    front_bypass_ = bypass;
}

void Voice::SetMainSend(float s)
{
    s = s + 1.0f;
    if (s < 0.0f)
    {
        s = 0.0f;
    }

    if (main_send_ != s)
    {
        main_send_ = s;
        dirty_ |= kMix;
    }
}

void Voice::SetFxSend(int bus, float s)
{
    if (s < 0.0f)
    {
        s = 0.0f;
    }

    if (fx_send_[bus] != s)
    {
        fx_send_[bus] = s;
        dirty_ |= kMix;
    }
}

void Voice::AppendWaveBuffer(int channel, nnsnd::WaveBuffer* wb, bool last)
{
    nn_[channel]->AppendWaveBuffer(wb);
    if (last)
    {
        watch_ = wb;
    }
}

void Voice::CheckFinish()
{
    if (!active_ || !playing_ || !nn_[0] || !watch_)
    {
        return;
    }
    if (watch_->status == nnsnd::WaveBuffer::Status::kWait || watch_->status == nnsnd::WaveBuffer::Status::kPlay)
    {
        return;
    }

    if (callback_)
    {
        callback_(place_, VoiceCallbackStatus::kFinishWave);
    }

    playing_ = false;
    started_ = false;
}

void Voice::CalcMixParam(int channel, nnsnd::MixParam& out) const
{
    const float main = Clamp01(main_send_);
    const float fx_a = Clamp01(fx_send_[0]);
    const float fx_b = Clamp01(fx_send_[1]);

    ntdlib::nwsnd::PanInfo info = ntdlib::nwsnd::PanInfoOf(pan_curve_);
    info.front_bypass = front_bypass_;

    const auto output = static_cast<OutputMode>(engine_->OutputMode());
    float hi = 0.0f, lo = 0.0f, front = 0.0f, rear = 0.0f;
    if (output == OutputMode::kMono)
    {
        hi = lo = CalcPanRatio<CtrPolicy>(0.0f, info, output);
    }
    else
    {
        if (channel_count_ < 2 || pan_mode_ != 1)
        {
            float p = pan_;
            if (channel_count_ == 2)
            {
                if (channel == 0)
                {
                    p -= 1.0f;
                }
                else if (channel == 1)
                {
                    p += 1.0f;
                }
            }

            hi = CalcPanRatio<CtrPolicy>(p, info, output);
            lo = CalcPanRatio<CtrPolicy>(0.0f - p, info, output);
        }
        else if (channel == 0)
        {
            hi = CalcPanRatio<CtrPolicy>(pan_, info, output);
            lo = 0.0f;
        }
        else
        {
            hi = 0.0f;
            lo = CalcPanRatio<CtrPolicy>(0.0f - pan_, info, output);
        }
    }

    if (output == OutputMode::kSurround)
    {
        front = CalcSurroundPanRatio<CtrPolicy>(span_, info);
        rear = CalcSurroundPanRatio<CtrPolicy>(2.0f - span_, info);
    }
    else
    {
        front = CalcSurroundPanRatio<CtrPolicy>(0.0f, info);
        rear = CalcSurroundPanRatio<CtrPolicy>(2.0f, info);
    }

    const float f_hi = front * hi, f_lo = front * lo, r_lo = rear * lo, r_hi = rear * hi;
    out[0] = f_hi * main;
    out[1] = f_lo * main;
    out[2] = r_hi * main;
    out[3] = r_lo * main;
    out[4] = fx_a * f_hi;
    out[5] = fx_a * f_lo;
    out[6] = fx_a * r_hi;
    out[7] = fx_a * r_lo;
    out[8] = fx_b * f_hi;
    out[9] = fx_b * f_lo;
    out[10] = fx_b * r_hi;
    out[11] = fx_b * r_lo;
}

void Voice::SyncParams()
{
    // 0x17aadc
    if (!started_)
    {
        return;
    }

    if (dirty_ & kPitch)
    {
        ApplyPitch();
        dirty_ &= static_cast<uint16_t>(~kPitch);
    }

    if (dirty_ & kVolume)
    {
        ApplyVolume();
        dirty_ &= static_cast<uint16_t>(~kVolume);
    }

    if (dirty_ & kMix)
    {
        ApplyMix();
        dirty_ &= static_cast<uint16_t>(~kMix);
    }

    if (dirty_ & kLpf)
    {
        const uint16_t freq = util::CalcLpfFreq(lpf_);
        const auto set_filter = [freq](nnsnd::Voice* v, int)
        {
            if (freq < 16000)
            {
                v->EnableMonoFilter(true);
                int16_t b0, a1;
                util::CalcMonoFilterCoefficients(freq, b0, a1);
                v->SetMonoFilterCoefficients(b0, a1);
            }
            else
            {
                v->EnableMonoFilter(false);
            }
        };
        ForEachDspVoice(set_filter);
        dirty_ &= static_cast<uint16_t>(~kLpf);
    }

    if (dirty_ & kBiquad)
    {
        // The hardware manager's callback for the type works the coefficients out. A type without one, or a value of 0,
        // turns the filter off.
        const int16_t* coefs =
            biquad_value_ > 0.0f ? util::BiquadPresetCoefficients(biquad_type_, biquad_value_) : nullptr;
        const auto set_filter = [coefs](nnsnd::Voice* v, int)
        {
            v->EnableBiquadFilter(coefs != nullptr);
            if (coefs)
            {
                v->SetBiquadFilterCoefficients(coefs);
            }
        };
        ForEachDspVoice(set_filter);
        dirty_ &= static_cast<uint16_t>(~kBiquad);
    }
}

void Voice::UpdateState()
{
    // 0x17ad10: a voice that has been started but isn't playing yet gets its parameters and starts on the DSP.
    if (!active_ || !(dirty_ & kStart) || !started_ || playing_)
    {
        return;
    }

    ApplyPitch();
    ApplyMix();
    ApplyVolume();
    playing_ = true;
    dirty_ &= static_cast<uint16_t>(~(kStart | kPitch | kMix));

    ForEachDspVoice([](nnsnd::Voice* v, int) { v->SetState(nnsnd::VoiceState::kPlay); });
}

void Voice::ApplyPitch()
{
    ForEachDspVoice([this](nnsnd::Voice* v, int) { v->SetPitch(pitch_); });
}

void Voice::ApplyMix()
{
    const auto set_mix = [this](nnsnd::Voice* v, int channel)
    {
        nnsnd::MixParam mix{};
        CalcMixParam(channel, mix);
        v->SetMixParam(mix);
    };
    ForEachDspVoice(set_mix);
}

void Voice::ApplyVolume()
{
    ForEachDspVoice([this](nnsnd::Voice* v, int) { v->SetVolume(volume_); });
}

void Voice::AppendWave(Engine&, const WaveInfo& info)
{
    Initialize(info);

    // 0x320914, for a wave that plays from its start (a note always does).
    const uint32_t loop_start_bytes = SamplesToBytes(info.loop_start, info.format);
    const bool adpcm = info.format == SampleFormat::kDspAdpcm;
    for (int ch = 0; ch < channel_count_; ch++)
    {
        const auto& c = info.channels[ch];
        if (adpcm)
        {
            contexts_[ch] = c.context;
            if (info.loop)
            {
                loop_contexts_[ch] = c.loop_context;
            }

            nn_[ch]->SetAdpcmParam(c.coefs.data());
        }

        nnsnd::WaveBuffer& wb0 = wave_buffers_[ch * 2];
        wb0.Initialize();
        wb0.buffer_address = c.data;
        wb0.sample_length = info.loop_end;
        wb0.loop = false;
        if (adpcm)
        {
            wb0.adpcm_context = &contexts_[ch];
        }
        AppendWaveBuffer(ch, &wb0, !info.loop);

        if (info.loop)
        {
            nnsnd::WaveBuffer& wb1 = wave_buffers_[ch * 2 + 1];
            wb1.Initialize();
            wb1.buffer_address = c.data + loop_start_bytes;
            wb1.sample_length = info.loop_end - info.loop_start;
            wb1.loop = true;
            if (adpcm)
            {
                wb1.adpcm_context = &loop_contexts_[ch];
            }
            AppendWaveBuffer(ch, &wb1, true);
        }
    }

    ApplyInterpolationType();
}

// --------------------------------------------------------------------------------------------- VoiceManager
VoiceManager::VoiceManager(Engine& engine) : engine_(engine)
{
    // Before any place has been freed, the pool hands out its last place first.
    for (VoiceId place = 0; place < kVoiceCount; place++)
    {
        free_.push_back(place);
    }

    const auto on_drop = [this](nnsnd::DropOwner owner, nnsnd::Voice* dropped)
    {
        At(owner.place).OnDspVoiceDropped(dropped, owner.stereo);
    };
    engine_.Snd().SetDropHandler(on_drop);
}

VoiceId VoiceManager::AllocVoice(Engine&, int channel_count, int priority, Voice::Callback callback)
{
    // 0x187070. With every voice in use, the lowest-priority one makes room, unless it outranks the new one. If the
    // dropped voice held no DSP voices, the allocation fails anyway.
    if (static_cast<int>(list_.size()) >= kVoiceCount && DropLowestPriorityVoice(priority) == 0)
    {
        return kNoVoice;
    }

    if (free_.empty())
    {
        throw std::logic_error("no free place for an nw::snd voice");
    }

    const VoiceId place = free_.back();
    free_.pop_back();
    Voice& v = pool_[static_cast<std::size_t>(place)];
    v = Voice{};
    v.engine_ = &engine_;
    v.place_ = place;
    if (!v.Alloc(channel_count, priority, std::move(callback)))
    {
        free_.push_back(place);
        return kNoVoice;
    }

    v.priority_ = priority & 0xff; // 0x187130
    InsertByPriority(place);

    return place;
}

VoiceId VoiceManager::AllocVoice(Engine& engine, int channel_count, uint32_t priority, ntdlib::nwsnd::ChannelId owner)
{
    const auto on_event = [&engine, owner](VoiceId, VoiceCallbackStatus status)
    {
        engine.Channels().At(owner).OnVoiceEvent(engine, status);
    };
    return AllocVoice(engine, channel_count, static_cast<int>(priority), on_event);
}

int VoiceManager::DropLowestPriorityVoice(int priority)
{
    // 0x18dd54
    if (list_.empty())
    {
        return 0;
    }

    const VoiceId victim = list_.front();
    Voice& v = At(victim);
    if (v.priority_ > priority)
    {
        return 0;
    }

    const int channels = v.channel_count_;
    auto cb = v.callback_;
    v.Stop();
    FreeVoice(victim);
    if (cb)
    {
        cb(victim, VoiceCallbackStatus::kDropVoice);
    }

    return channels;
}

void VoiceManager::FreeVoice(VoiceId place)
{
    // 0x18b928
    if (place == kNoVoice || !At(place).active_)
    {
        return;
    }

    Voice& v = At(place);
    for (int i = 0; i < v.channel_count_; i++)
    {
        if (v.nn_[i])
        {
            engine_.Snd().FreeVoice(v.nn_[i]);
            v.nn_[i] = nullptr;
        }
    }

    v.channel_count_ = 0;
    Release(place);
    v.active_ = false;
}

void VoiceManager::Release(VoiceId place)
{
    list_.remove(place);
    free_.push_back(place);
}

void VoiceManager::InsertByPriority(VoiceId place)
{
    // Insert after every voice with lower or equal priority.
    const int priority = At(place).priority_;
    auto it = std::find_if(list_.begin(), list_.end(), [&](VoiceId o) { return At(o).priority_ > priority; });
    list_.insert(it, place);
}

void VoiceManager::ChangePriority(VoiceId place)
{
    list_.remove(place);
    InsertByPriority(place);
}

VoiceManager::Snapshot VoiceManager::Save() const
{
    return {pool_, free_, list_};
}

void VoiceManager::Restore(const Snapshot& snapshot)
{
    pool_ = snapshot.pool;
    free_ = snapshot.free;
    list_ = snapshot.list;
}

void VoiceManager::UpdateAllVoices()
{
    // 0x174540. CheckFinish calls the callback of each finished voice. The callback can free voices. Freeing a voice
    // takes it out of list_. Each pass therefore runs over a copy of list_. The copy is made again after CheckFinish.
    std::vector<VoiceId> voices(list_.begin(), list_.end());
    for (VoiceId v : voices)
    {
        At(v).CheckFinish();
    }

    voices.assign(list_.begin(), list_.end());
    for (VoiceId v : voices)
    {
        At(v).SyncParams();
    }

    for (VoiceId v : voices)
    {
        At(v).UpdateState();
    }
}

} // namespace threesf::nwsnd
