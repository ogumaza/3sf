// SPDX-License-Identifier: MIT

// nw::snd's voices (CTR), modelled on Pokemon X's code.bin. A Voice plays a wave on one or two of nn::snd's DSP voices,
// from wave buffers that the Voice contains. VoiceManager holds the pool of voices and a priority list that sets the
// order of drops. The channels refer to a voice by its place in the pool.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <vector>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/types.h"

#include "nnsnd/voice.h"

namespace threesf::nwsnd
{

class Engine;

// ntdlib's types for a voice's place in the voice manager's pool (or kNoVoice) and for the reason a voice calls its
// owner back. The core's channels use them too.
using ntdlib::nwsnd::kNoVoice;
using ntdlib::nwsnd::VoiceCallbackStatus;
using ntdlib::nwsnd::VoiceId;

// nw::snd internal WaveInfo (filled by WaveFileReader).
struct WaveInfo
{
    struct Channel
    {
        ntdlib::PAddr data = 0; // physical address of the channel's sample data
        std::array<int16_t, 16> coefs{};
        nnsnd::AdpcmContext context;
        nnsnd::AdpcmContext loop_context;
    };

    uint8_t format = 3; // nw::snd SampleFormat: 0 PCM8, 1 PCM16, 3 DSP ADPCM
    bool loop = false;
    int channel_count = 1;
    uint32_t sample_rate = 32728;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    std::array<Channel, 2> channels;
};

// One output's parameters for a voice: the 3DS has one output.
struct Outputs
{
    float volume = 1.0f;
    int mix_mode = 0;
    float pan = 0.0f;
    float span = 0.0f;
    float main_send = 0.0f;
    std::array<float, 2> fx_send{};
};

class Voice
{
public:
    // The owner's callback, with the voice's place: its wave finished (kFinishWave), nw::snd's voice manager dropped it
    // (kDropVoice), or nn::snd dropped a DSP voice of it (kDropDsp). nn::snd drops a DSP voice to make room for
    // another, or when the voice's DSP cycles don't fit in what's left of a frame.
    using Callback = std::function<void(VoiceId, VoiceCallbackStatus)>;

    // Dirty bits (+0x20).
    static constexpr uint16_t kStart = 0x1, kPitch = 0x4, kMix = 0x8, kLpf = 0x10, kBiquad = 0x20, kVolume = 0x40;

    // 0x18b9ec: takes a DSP voice for each channel (one or two). Returns false if there aren't enough.
    bool Alloc(int channel_count, int priority, Callback callback);

    void Stop(); // 0x18b990

    void SetPriority(int p); // 0x31c3c8

    // The calls of the core's channel (ntdlib/nwsnd/channel.h). The voice doesn't store the sample format and rate. The
    // wave gives them again. The interpolation type is recorded, and AppendWave applies it.
    void SetSampleFormat(uint8_t)
    {
    }

    void SetSampleRate(uint32_t)
    {
    }

    void SetInterpolationType(int type)
    {
        interpolation_ = type;
    }

    // Prepares the voice's DSP voices for the wave (0x31c2b4) and queues the wave from its start, with its loop if it
    // loops (Channel's AppendWaveBuffer, 0x320914). Then it applies the interpolation type (0x31c474). Channel::Start
    // (0x320bdc) makes the three calls in this order.
    void AppendWave(Engine& engine, const WaveInfo& info);

    // The core's Stop does nothing on the 3DS: the channel frees the voice, and FreeVoice stops its DSP voices. Free
    // calls VoiceManager::FreeVoice.
    void Stop(Engine&)
    {
    }

    void Free(Engine& engine);

    // On the 3DS, a voice's pan mode and curve stay 0. Channel::Start copies the channel's mode and curve to the voice
    // before the track sets them. A voice has one output, with no line to choose.
    void SetPanMode(int)
    {
    }

    void SetPanCurve(int)
    {
    }

    void SetOutputLine(uint32_t)
    {
    }

    // The output's pan, surround pan and sends.
    void SetOutputs(const Outputs& outputs);

    void SetPriority(Engine&, int priority)
    {
        SetPriority(priority);
    }

    void Start() // 0x31c540
    {
        started_ = true;
        dirty_ |= kStart;
    }

    void SetVolume(float v);                                              // 0x17fb40
    void SetPitch(float p);                                               // 0x17fae0
    void SetPan(float p);                                                 // 0x17f9f4
    void SetSurroundPan(float p);                                         // 0x17f968
    void SetLpfFreq(float f);                                             // 0x17f900
    void SetBiquadFilter(int type, float value);                          // 0x17f98c
    void SetFrontBypass(bool bypass);                                     // 0x186c04
    void SetMainSend(float s);                                            // 0x17f924
    void SetFxSend(int bus, float s);                                     // 0x17fb04
    void AppendWaveBuffer(int channel, nnsnd::WaveBuffer* wb, bool last); // 0x18b900
    void CheckFinish();                                                   // 0x17aa70
    void SyncParams();                                                    // 0x17aadc
    void UpdateState();                                                   // 0x17ad10

    Engine* engine_ = nullptr;
    VoiceId place_ = kNoVoice; // its place in the pool
    std::array<nnsnd::Voice*, 2> nn_{};
    int channel_count_ = 0;
    Callback callback_;
    int priority_ = 0;
    bool active_ = false;                // +0x14
    bool started_ = false;               // +0x15
    bool playing_ = false;               // +0x16
    uint16_t dirty_ = 0;                 // +0x20
    uint8_t biquad_type_ = 0;            // +0x22
    bool front_bypass_ = false;          // +0x23
    float volume_ = 1.0f;                // +0x24
    float pitch_ = 1.0f;                 // +0x28
    uint8_t pan_mode_ = 0;               // +0x2c
    uint8_t pan_curve_ = 0;              // +0x2d
    float pan_ = 0.0f;                   // +0x30
    float span_ = 0.0f;                  // +0x34
    float lpf_ = 1.0f;                   // +0x38
    float biquad_value_ = 0.0f;          // +0x3c
    float main_send_ = 1.0f;             // +0x44 (stored as send + 1, clamped)
    std::array<float, 2> fx_send_{};     // +0x48
    nnsnd::WaveBuffer* watch_ = nullptr; // +0x1c: last wave buffer, used to detect the end
    int interpolation_ = 0;              // recorded by SetInterpolationType, applied by AppendWave

    // The wave buffers that the voice's DSP voices play, two for each channel (the wave and its loop), and their ADPCM
    // contexts. nn::snd's voices point into them. The addresses stay valid because the pool never moves its voices.
    std::array<nnsnd::WaveBuffer, 4> wave_buffers_;
    std::array<nnsnd::AdpcmContext, 2> contexts_;
    std::array<nnsnd::AdpcmContext, 2> loop_contexts_;

private:
    friend class VoiceManager;

    void Initialize(const WaveInfo& info);                      // 0x31c2b4
    void ApplyInterpolationType();                              // 0x31c474
    void CalcMixParam(int channel, nnsnd::MixParam& out) const; // 0x1868f4

    // Handles nn::snd dropping a DSP voice (0x18dc1c for mono, 0x18dc6c for stereo). Releases any second DSP voice,
    // returns this voice to the pool and notifies its owner.
    void OnDspVoiceDropped(nnsnd::Voice* dropped, bool stereo);

    bool allocating_ = false;   // +0x1a
    bool alloc_failed_ = false; // +0x1b
};

class VoiceManager
{
public:
    // Pokemon X sets nw::snd's voice count to 23 before the sound system starts (0x1150d0 writes it to 0x5d10c0, where
    // nw::snd's default is 24), so one of the DSP's 24 voices is never used.
    static constexpr int kVoiceCount = 23;

    // The pool's voices, its free places and the priority list: a plain copy, for a snapshot of the engine.
    struct Snapshot
    {
        std::array<Voice, kVoiceCount> pool;
        std::vector<VoiceId> free;
        std::list<VoiceId> list;
    };

    // Sets nn::snd's drop handler. The handler passes each dropped DSP voice to the voice that holds it.
    explicit VoiceManager(Engine& engine);

    // 0x187070: allocates a voice, dropping the lowest-priority one first if all kVoiceCount are in use. Returns its
    // place, or kNoVoice. `engine` comes from the core's channel and is always the manager's engine.
    VoiceId AllocVoice(Engine& engine, int channel_count, int priority, Voice::Callback callback);

    // The same for the core's channels (ntdlib/nwsnd/channel_impl.h): the voice reports to the channel at `owner`.
    VoiceId AllocVoice(Engine& engine, int channel_count, uint32_t priority, ntdlib::nwsnd::ChannelId owner);

    void FreeVoice(VoiceId place); // 0x18b928

    // The voice at `place`.
    Voice& At(VoiceId place)
    {
        return pool_[static_cast<std::size_t>(place)];
    }

    // 0x18dd54: drops the lowest-priority voice (the oldest among equals) unless it outranks `priority`, and tells its
    // owner. Returns how many DSP voices it had, so 0 if none was dropped.
    int DropLowestPriorityVoice(int priority);

    void ChangePriority(VoiceId place);

    // 0x174540
    void UpdateAllVoices();

    // Saves and restores the voices between frames.
    Snapshot Save() const;
    void Restore(const Snapshot& snapshot);

private:
    friend class Voice;

    // 0x18ddc4: takes a voice out of the priority list and returns its place to the pool.
    void Release(VoiceId place);

    Engine& engine_;

    // The voices by place. A freed place keeps its voice until the place is handed out again: an owner told of a drop
    // can still read it. The pool hands out the last freed place first.
    std::array<Voice, kVoiceCount> pool_;
    std::vector<VoiceId> free_;
    std::list<VoiceId> list_; // lowest priority first; oldest first among equals
};

} // namespace threesf::nwsnd
