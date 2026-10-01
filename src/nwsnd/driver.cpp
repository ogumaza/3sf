// SPDX-License-Identifier: MIT

#include "nwsnd/driver.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "nwsnd/engine.h"

namespace threesf::nwsnd
{
namespace
{

// The game's 1/255 (code.bin 0x17b2ec), the float nearest to it.
const float kOneOver255 = std::bit_cast<float>(0x3b808081u);

nnsnd::SampleFormat ToNnFormat(uint8_t nw_format)
{
    return nw_format == 0   ? nnsnd::SampleFormat::kPcm8
           : nw_format == 3 ? nnsnd::SampleFormat::kAdpcm
                            : nnsnd::SampleFormat::kPcm16;
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

// Sample offset -> byte offset (code.bin 0x186888).
uint32_t SamplesToBytes(uint32_t samples, uint8_t nw_format)
{
    switch (nw_format)
    {
    case 0:
        return samples;
    case 3:
        return samples / 14 * 8;
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
        nnsnd::Voice* nn = snd.AllocVoice(dsp_priority, [this](nnsnd::Voice* d) { OnDspVoiceDropped(d, false); });
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
            while (!(nn = snd.AllocVoice(dsp_priority, [this](nnsnd::Voice* d) { OnDspVoiceDropped(d, true); })))
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
        for (int i = 0; i < channel_count_; i++)
        {
            if (nn_[i])
            {
                nn_[i]->SetState(nnsnd::VoiceState::kStop);
            }
        }

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
    engine_->Voices().Release(this);
    active_ = false;

    auto cb = callback_;
    if (cb)
    {
        cb(this, Status::kDropDsp);
    }
}

void Voice::Initialize(const WaveInfo& info)
{
    for (int i = 0; i < channel_count_; i++)
    {
        nnsnd::Voice* v = nn_[i];
        if (!v)
        {
            continue;
        }

        v->SetChannelCount(1);
        v->SetSampleFormat(ToNnFormat(info.sample_format));
        v->SetSampleRate(info.sample_rate);
        v->SetInterpolationType(nnsnd::InterpolationType::kPolyphase); // HardwareManager's default SRC type, 4-tap
    }

    started_ = false;
    playing_ = false;
}

void Voice::SetPriority(int p)
{
    priority_ = p;
    engine_->Voices().ChangePriority(this);

    if (p == 1)
    {
        for (int i = 0; i < channel_count_; i++)
        {
            if (nn_[i])
            {
                engine_->Snd().ChangePriority(nn_[i], 1);
            }
        }
    }
}

void Voice::SetInterpolationType(int type)
{
    for (int i = 0; i < channel_count_; i++)
    {
        if (nn_[i])
        {
            nn_[i]->SetInterpolationType(static_cast<nnsnd::InterpolationType>(type == 1 ? 1 : type == 2 ? 2 : 0));
        }
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
    for (int i = 0; i < channel_count_; i++)
    {
        if (nn_[i])
        {
            nn_[i]->SetFrontBypass(bypass);
        }
    }

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
        callback_(this, Status::kFinishWave);
    }

    playing_ = false;
    started_ = false;
}

void Voice::CalcMixParam(int channel, nnsnd::MixParam& out) const
{
    const float main = Clamp01(main_send_);
    const float fx_a = Clamp01(fx_send_[0]);
    const float fx_b = Clamp01(fx_send_[1]);

    PanInfo info;
    switch (pan_curve_)
    {
    case 1:
        info = {0, true, false};
        break;
    case 2:
        info = {0, true, true};
        break;
    case 3:
        info = {1, false, false};
        break;
    case 4:
        info = {1, true, false};
        break;
    case 5:
        info = {1, true, true};
        break;
    case 6:
        info = {2, false, false};
        break;
    case 7:
        info = {2, true, false};
        break;
    case 8:
        info = {2, true, true};
        break;
    default:
        info = {0, false, false};
        break;
    }
    info.front_bypass = front_bypass_;

    const uint8_t mode = engine_->OutputMode();
    const bool surround = mode == 2;
    float hi = 0.0f, lo = 0.0f, front = 0.0f, rear = 0.0f;
    if (mode == 0)
    {
        hi = lo = util::CalcPanRatio(0.0f, info, surround);
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

            hi = util::CalcPanRatio(p, info, surround);
            lo = util::CalcPanRatio(0.0f - p, info, surround);
        }
        else if (channel == 0)
        {
            hi = util::CalcPanRatio(pan_, info, surround);
            lo = 0.0f;
        }
        else
        {
            hi = 0.0f;
            lo = util::CalcPanRatio(0.0f - pan_, info, surround);
        }
    }

    if (mode == 2)
    {
        front = util::CalcSurroundPanRatio(span_, info);
        rear = util::CalcSurroundPanRatio(2.0f - span_, info);
    }
    else
    {
        front = util::CalcSurroundPanRatio(0.0f, info);
        rear = util::CalcSurroundPanRatio(2.0f, info);
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
        for (int i = 0; i < channel_count_; i++)
        {
            if (nn_[i])
            {
                nn_[i]->SetPitch(pitch_);
            }
        }
        dirty_ &= static_cast<uint16_t>(~kPitch);
    }

    if (dirty_ & kVolume)
    {
        for (int i = 0; i < channel_count_; i++)
        {
            if (nn_[i])
            {
                nn_[i]->SetVolume(volume_);
            }
        }
        dirty_ &= static_cast<uint16_t>(~kVolume);
    }

    if (dirty_ & kMix)
    {
        for (int i = 0; i < channel_count_; i++)
        {
            if (!nn_[i])
            {
                continue;
            }

            nnsnd::MixParam mix{};
            CalcMixParam(i, mix);
            nn_[i]->SetMixParam(mix);
        }
        dirty_ &= static_cast<uint16_t>(~kMix);
    }

    if (dirty_ & kLpf)
    {
        const uint16_t freq = util::CalcLpfFreq(lpf_);
        for (int i = 0; i < channel_count_; i++)
        {
            if (!nn_[i])
            {
                continue;
            }

            if (freq < 16000)
            {
                nn_[i]->EnableMonoFilter(true);
                int16_t b0, a1;
                util::CalcMonoFilterCoefficients(freq, b0, a1);
                nn_[i]->SetMonoFilterCoefficients(b0, a1);
            }
            else
            {
                nn_[i]->EnableMonoFilter(false);
            }
        }
        dirty_ &= static_cast<uint16_t>(~kLpf);
    }

    if (dirty_ & kBiquad)
    {
        // The hardware manager's callback for the type works the coefficients out. A type without one, or a value of 0,
        // turns the filter off.
        const int16_t* coefs =
            biquad_value_ > 0.0f ? util::BiquadPresetCoefficients(biquad_type_, biquad_value_) : nullptr;
        for (int i = 0; i < channel_count_; i++)
        {
            if (!nn_[i])
            {
                continue;
            }

            nn_[i]->EnableBiquadFilter(coefs != nullptr);
            if (coefs)
            {
                nn_[i]->SetBiquadFilterCoefficients(coefs);
            }
        }
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

    for (int i = 0; i < channel_count_; i++)
    {
        if (nn_[i])
        {
            nn_[i]->SetPitch(pitch_);
        }
    }

    for (int i = 0; i < channel_count_; i++)
    {
        if (!nn_[i])
        {
            continue;
        }

        nnsnd::MixParam mix{};
        CalcMixParam(i, mix);
        nn_[i]->SetMixParam(mix);
    }

    for (int i = 0; i < channel_count_; i++)
    {
        if (nn_[i])
        {
            nn_[i]->SetVolume(volume_);
        }
    }

    playing_ = true;
    dirty_ &= static_cast<uint16_t>(~(kStart | kPitch | kMix));

    for (int i = 0; i < channel_count_; i++)
    {
        if (nn_[i])
        {
            nn_[i]->SetState(nnsnd::VoiceState::kPlay);
        }
    }
}

// --------------------------------------------------------------------------------------------- VoiceManager
Voice* VoiceManager::AllocVoice(int channel_count, int priority, Voice::Callback callback)
{
    // 0x187070. With every voice in use, the lowest-priority one makes room, unless it outranks the new one. If the
    // dropped voice held no DSP voices, the allocation fails anyway.
    if (static_cast<int>(list_.size()) >= kVoiceCount && DropLowestPriorityVoice(priority) == 0)
    {
        return nullptr;
    }

    auto owned = std::make_unique<Voice>();
    Voice* v = owned.get();
    v->engine_ = &engine_;
    if (!v->Alloc(channel_count, priority, std::move(callback)))
    {
        return nullptr;
    }

    v->priority_ = priority & 0xff; // 0x187130
    pool_.push_back(std::move(owned));

    // Insert after every voice with lower or equal priority.
    auto it = std::find_if(list_.begin(), list_.end(), [&](Voice* o) { return o->priority_ > v->priority_; });
    list_.insert(it, v);

    return v;
}

int VoiceManager::DropLowestPriorityVoice(int priority)
{
    // 0x18dd54
    if (list_.empty())
    {
        return 0;
    }

    Voice* victim = list_.front();
    if (victim->priority_ > priority)
    {
        return 0;
    }

    const int channels = victim->channel_count_;
    auto cb = victim->callback_;
    victim->Stop();
    FreeVoice(victim);
    if (cb)
    {
        cb(victim, Voice::Status::kDropVoice);
    }

    return channels;
}

void VoiceManager::FreeVoice(Voice* v)
{
    // 0x18b928
    if (!v || !v->active_)
    {
        return;
    }

    for (int i = 0; i < v->channel_count_; i++)
    {
        if (v->nn_[i])
        {
            engine_.Snd().FreeVoice(v->nn_[i]);
            v->nn_[i] = nullptr;
        }
    }

    v->channel_count_ = 0;
    Release(v);
    v->active_ = false;
}

void VoiceManager::Release(Voice* v)
{
    list_.remove(v);

    for (auto it = pool_.begin(); it != pool_.end(); ++it)
    {
        if (it->get() == v)
        {
            // Callers may still hold the pointer, so the object stays alive in a graveyard until UpdateAllVoices clears
            // it at the end of the frame's voice update.
            graveyard_.push_back(std::move(*it));
            pool_.erase(it);
            break;
        }
    }
}

void VoiceManager::ChangePriority(Voice* v)
{
    list_.remove(v);
    auto it = std::find_if(list_.begin(), list_.end(), [&](Voice* o) { return o->priority_ > v->priority_; });
    list_.insert(it, v);
}

void VoiceManager::UpdateAllVoices()
{
    // 0x174540
    std::vector<Voice*> snapshot(list_.begin(), list_.end());
    for (Voice* v : snapshot)
    {
        v->CheckFinish();
    }

    snapshot.assign(list_.begin(), list_.end());
    for (Voice* v : snapshot)
    {
        v->SyncParams();
    }

    for (Voice* v : snapshot)
    {
        v->UpdateState();
    }

    graveyard_.clear();
}

// --------------------------------------------------------------------------------------------- Channel
void Channel::InitParam(Callback cb)
{
    // 0x320e70
    next_in_track_ = nullptr;
    callback_ = std::move(cb);
    auto_sweep_ = true;
    release_priority_fix_ = false;
    ignore_note_off_ = false;
    length_ = 0;
    key_ = 60;
    original_key_ = 60;
    init_volume_ = 1.0f;
    init_pan_ = 0.0f;
    init_span_ = 0.0f;
    tune_ = 1.0f;
    cached_pitch_ = 0.0f;
    cached_ratio_ = 1.0f;
    track_volume_ = 1.0f;
    bend_ = 0.0f;
    track_pan_ = 0.0f;
    track_span_ = 0.0f;
    lpf_ = 0.0f;
    biquad_type_ = 0;
    biquad_value_ = 0.0f;
    main_send_ = 0.0f;
    fx_send_ = {0.0f, 0.0f};
    sweep_pitch_ = 0.0f;
    sweep_length_ = 0;
    sweep_counter_ = 0;
    env_.Initialize();
    lfo_.param_.Init();
    lfo_type_ = 0;
    pan_mode_ = 0;
    pan_curve_ = 0;
    key_group_ = 0;
    interpolation_type_ = 0; // polyphase, from HardwareManager's default SRC type (4-tap)
}

void Channel::Start(const WaveInfo& info, int length)
{
    // 0x320bdc
    length_ = length;
    lfo_.Reset();
    env_.Reset();
    sweep_counter_ = 0;
    voice_->Initialize(info);
    AppendWaveBuffer(info);
    voice_->pan_mode_ = pan_mode_;
    voice_->pan_curve_ = pan_curve_;
    voice_->SetInterpolationType(interpolation_type_);
    voice_->Start();
    active_ = true;
}

void Channel::AppendWaveBuffer(const WaveInfo& info)
{
    // 0x320914, for a wave that plays from its start (a note always does).
    const uint32_t loop_start_bytes = SamplesToBytes(info.loop_start, info.sample_format);
    for (int ch = 0; ch < voice_->channel_count_; ch++)
    {
        const auto& c = info.channels[ch];
        if (info.sample_format == 3)
        {
            contexts_[ch] = c.context;
            if (info.loop)
            {
                loop_contexts_[ch] = c.loop_context;
            }

            voice_->nn_[ch]->SetAdpcmParam(c.coefs.data());
        }

        nnsnd::WaveBuffer& wb0 = wave_buffers_[ch * 2];
        wb0.Initialize();
        wb0.buffer_address = c.data;
        wb0.sample_length = info.loop_end;
        wb0.loop = false;
        if (info.sample_format == 3)
        {
            wb0.adpcm_context = &contexts_[ch];
        }
        voice_->AppendWaveBuffer(ch, &wb0, !info.loop);

        if (info.loop)
        {
            nnsnd::WaveBuffer& wb1 = wave_buffers_[ch * 2 + 1];
            wb1.Initialize();
            wb1.buffer_address = c.data + loop_start_bytes;
            wb1.sample_length = info.loop_end - info.loop_start;
            wb1.loop = true;
            if (info.sample_format == 3)
            {
                wb1.adpcm_context = &loop_contexts_[ch];
            }
            voice_->AppendWaveBuffer(ch, &wb1, true);
        }
    }
}

void Channel::Update()
{
    // 0x17afcc
    if (!active_)
    {
        return;
    }

    // The game scales the volume by the channel's silence level, which stays at 255 unless the game silences the track,
    // and 3SF never does.
    const float vol_base = (255.0f * kOneOver255) * (init_volume_ * track_volume_);

    if (env_.GetStatus() == EnvGenerator::Status::kRelease && env_.GetValue() < -90.4f)
    {
        Stop();
        return;
    }

    float sweep = 0.0f;
    if (sweep_pitch_ != 0.0f && sweep_counter_ < sweep_length_)
    {
        sweep = (static_cast<float>(sweep_length_ - sweep_counter_) * sweep_pitch_) / static_cast<float>(sweep_length_);
    }

    float pitch = sweep + static_cast<float>(static_cast<int>(key_) - static_cast<int>(original_key_)) + bend_;
    if (lfo_type_ == 0)
    {
        pitch = lfo_.GetValue() + pitch;
    }

    float ratio;
    if (cached_pitch_ == pitch)
    {
        ratio = cached_ratio_;
    }
    else
    {
        ratio = util::CalcPitchRatio(static_cast<int>(pitch * 256.0f));
        cached_pitch_ = pitch;
        cached_ratio_ = ratio;
    }

    float pan = init_pan_ + track_pan_;
    if (lfo_type_ == 2)
    {
        pan = lfo_.GetValue() + pan;
    }

    const float span = init_span_ + track_span_;

    if (auto_sweep_)
    {
        UpdateSweep(5);
    }
    lfo_.Update(5);
    env_.Update(5);

    const float lpf_freq = lpf_ + 1.0f;
    float vol = util::CalcVolumeRatio(env_.GetValue()) * vol_base;
    if (lfo_type_ == 1)
    {
        vol = util::CalcVolumeRatio(lfo_.GetValue() * 6.0f) * vol;
    }

    if (voice_)
    {
        voice_->SetVolume(vol);
        voice_->SetPitch(tune_ * ratio);
        voice_->SetPan(pan);
        voice_->SetSurroundPan(span);
        voice_->SetLpfFreq(lpf_freq);
        voice_->SetBiquadFilter(biquad_type_, biquad_value_);
        voice_->SetMainSend(main_send_);
        for (int i = 0; i < 2; i++)
        {
            voice_->SetFxSend(i, fx_send_[i]);
        }
    }
}

void Channel::NoteOff()
{
    // 0x320c6c
    if (ignore_note_off_)
    {
        return;
    }

    Release();
}

void Channel::Release()
{
    // 0x320cbc
    if (env_.GetStatus() != EnvGenerator::Status::kRelease)
    {
        if (voice_ && !release_priority_fix_)
        {
            voice_->SetPriority(1);
        }
        env_.SetStatus(EnvGenerator::Status::kRelease);
    }
}

void Channel::Stop()
{
    // 0x320b6c
    if (!voice_)
    {
        return;
    }

    Voice* v = voice_;
    voice_ = nullptr;
    engine_->Voices().FreeVoice(v);
    active_ = false;
    auto cb = callback_;
    if (cb)
    {
        cb(this, CallbackStatus::kStopped);
    }

    if (auto_free_)
    {
        auto_free_ = false;
        engine_->Channels().Free(this);
    }
}

void Channel::OnVoiceEvent(Voice::Status status)
{
    // 0x17fb84. A voice that finished is freed here; a dropped one was freed by the voice manager.
    CallbackStatus cs = CallbackStatus::kDrop;
    if (status == Voice::Status::kFinishWave)
    {
        cs = CallbackStatus::kFinish;
        engine_->Voices().FreeVoice(voice_);
    }

    auto cb = callback_;
    if (cb)
    {
        cb(this, cs);
    }

    voice_ = nullptr;
    active_ = false;
    auto_free_ = false;
    engine_->Channels().Free(this);
}

// --------------------------------------------------------------------------------------------- ChannelManager
Channel* ChannelManager::AllocChannel(int channel_count, int priority, Channel::Callback callback)
{
    // 0x320868
    auto owned = std::make_unique<Channel>();
    Channel* ch = owned.get();
    ch->engine_ = &engine_;
    ch->auto_free_ = true;
    Voice* v =
        engine_.Voices().AllocVoice(channel_count, priority, [ch](Voice*, Voice::Status s) { ch->OnVoiceEvent(s); });
    if (!v)
    {
        return nullptr;
    }

    pool_.push_back(std::move(owned));
    active_list_.push_back(ch);
    ch->voice_ = v;
    ch->InitParam(std::move(callback));

    return ch;
}

void ChannelManager::Free(Channel* ch)
{
    active_list_.remove(ch);
    for (auto it = pool_.begin(); it != pool_.end(); ++it)
    {
        if (it->get() == ch)
        {
            graveyard_.push_back(std::move(*it));
            pool_.erase(it);
            break;
        }
    }
}

void ChannelManager::UpdateAllChannel()
{
    // 0x1742a0
    std::vector<Channel*> snapshot(active_list_.begin(), active_list_.end());
    for (Channel* ch : snapshot)
    {
        if (std::find(active_list_.begin(), active_list_.end(), ch) != active_list_.end())
        {
            ch->Update();
        }
    }
}

int ChannelManager::ActiveCount() const
{
    return static_cast<int>(active_list_.size());
}

} // namespace threesf::nwsnd
