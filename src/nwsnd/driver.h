// SPDX-License-Identifier: MIT

// nw::snd driver layer (CTR): Voice, Channel and their managers, modelled on Pokemon X's code.bin. The engine is
// single-threaded here; the game's command queues and locks are replaced by direct calls made in the same order within
// a sound frame.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>

#include "common/common_types.h"
#include "nnsnd/voice.h"
#include "nwsnd/envelope.h"
#include "nwsnd/util.h"

namespace threesf::nwsnd
{

class Engine;

// nw::snd internal WaveInfo (filled by WaveFileReader).
struct WaveInfo
{
    struct Channel
    {
        PAddr data = 0; // physical address of the channel's sample data
        std::array<int16_t, 16> coefs{};
        nnsnd::AdpcmContext context;
        nnsnd::AdpcmContext loop_context;
    };

    uint8_t sample_format = 3; // nw::snd SampleFormat: 0 PCM8, 1 PCM16, 3 DSP ADPCM
    bool loop = false;
    int channel_count = 1;
    uint32_t sample_rate = 32728;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    std::array<Channel, 2> channels;
};

class Voice
{
public:
    enum class Status
    {
        kFinishWave = 0,
        kDropVoice = 2, // dropped by nw::snd's voice manager
        kDropDsp = 3    // dropped by nn::snd, to make room for another DSP voice
    };

    using Callback = std::function<void(Voice*, Status)>;

    // Dirty bits (+0x20).
    static constexpr uint16_t kStart = 0x1, kPitch = 0x4, kMix = 0x8, kLpf = 0x10, kBiquad = 0x20, kVolume = 0x40;

    // 0x18b9ec: takes a DSP voice for each channel (one or two). Returns false if there aren't enough.
    bool Alloc(int channel_count, int priority, Callback callback);
    void Stop(); // 0x18b990

    void Initialize(const WaveInfo& info); // 0x31c2b4
    void SetPriority(int p);               // 0x31c3c8
    void SetInterpolationType(int type);   // 0x31c474

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

private:
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
    // Active voices and copies of their values. Keeps the voices alive so pointers elsewhere in the snapshot remain
    // valid.
    struct Snapshot
    {
        std::list<std::shared_ptr<Voice>> pool;
        std::vector<Voice> values; // of the voices in `pool`, in its order
        std::list<Voice*> list;
    };

    // Pokemon X sets nw::snd's voice count to 23 before the sound system starts (0x1150d0 writes it to 0x5d10c0, where
    // nw::snd's default is 24), so one of the DSP's 24 voices is never used.
    static constexpr int kVoiceCount = 23;

    explicit VoiceManager(Engine& engine) : engine_(engine)
    {
    }

    // 0x187070: allocates a voice, dropping the lowest-priority one first if all kVoiceCount are in use.
    Voice* AllocVoice(int channel_count, int priority, Voice::Callback callback);
    void FreeVoice(Voice* v); // 0x18b928

    // 0x18dd54: drops the lowest-priority voice (the oldest among equals) unless it outranks `priority`, and tells its
    // owner. Returns how many DSP voices it had, so 0 if none was dropped.
    int DropLowestPriorityVoice(int priority);

    void ChangePriority(Voice* v);

    // 0x174540
    void UpdateAllVoices();

    // Saves state between frames, after freed voices have been released.
    Snapshot Save() const;
    void Restore(const Snapshot& snapshot);

private:
    friend class Voice;

    // 0x18ddc4: takes a voice out of the priority list and returns it to the pool.
    void Release(Voice* v);

    Engine& engine_;
    std::list<std::shared_ptr<Voice>> pool_;
    std::vector<std::shared_ptr<Voice>> graveyard_;
    std::list<Voice*> list_; // lowest priority first; oldest first among equals
};

class Channel
{
public:
    enum class CallbackStatus
    {
        kStopped = 0,
        kDrop = 1,
        kFinish = 2
    };

    using Callback = std::function<void(Channel*, CallbackStatus)>;

    void InitParam(Callback cb);                  // 0x320e70
    void Start(const WaveInfo& info, int length); // 0x320bdc
    void Update();                                // 0x17afcc
    void NoteOff();                               // 0x320c6c
    void Release();                               // 0x320cbc
    void Stop();                                  // 0x320b6c

    void SetSweepParam(float pitch, int time, bool auto_update) // 0x3208f0
    {
        sweep_pitch_ = pitch;
        sweep_length_ = time;
        sweep_counter_ = 0;
        auto_sweep_ = auto_update;
    }

    void UpdateSweep(int n) // 0x320848
    {
        sweep_counter_ = std::min(sweep_counter_ + n, sweep_length_);
    }

    void OnVoiceEvent(Voice::Status status); // 0x17fb84

    Engine* engine_ = nullptr;
    EnvGenerator env_;                                // +0x90
    Lfo lfo_;                                         // +0xac
    uint8_t lfo_type_ = 0;                            // +0xc4: 0 pitch, 1 volume, 2 pan
    bool active_ = false;                             // +0xc6
    bool auto_free_ = false;                          // +0xc7
    bool auto_sweep_ = true;                          // +0xc8
    bool release_priority_fix_ = false;               // +0xc9
    bool ignore_note_off_ = false;                    // +0xca
    uint8_t biquad_type_ = 0;                         // +0xcb
    float track_volume_ = 1.0f;                       // +0xcc
    float track_pan_ = 0.0f;                          // +0xd4
    float track_span_ = 0.0f;                         // +0xd8
    float lpf_ = 0.0f;                                // +0xdc
    float biquad_value_ = 0.0f;                       // +0xe0
    float main_send_ = 0.0f;                          // +0xe4
    std::array<float, 2> fx_send_{};                  // +0xe8
    float bend_ = 0.0f;                               // +0xf0 (semitones)
    float sweep_pitch_ = 0.0f;                        // +0xf4
    int sweep_counter_ = 0;                           // +0xf8
    int sweep_length_ = 0;                            // +0xfc
    float init_volume_ = 1.0f;                        // +0x100
    float init_pan_ = 0.0f;                           // +0x104
    float init_span_ = 0.0f;                          // +0x108
    float tune_ = 1.0f;                               // +0x10c
    float cached_pitch_ = 0.0f, cached_ratio_ = 1.0f; // +0x118
    int length_ = 0;                                  // +0x120
    uint8_t pan_mode_ = 0, pan_curve_ = 0;            // +0x124
    uint8_t key_ = 60, original_key_ = 60;            // +0x126
    uint8_t key_group_ = 0;                           // +0x128
    uint8_t interpolation_type_ = 0;                  // +0x129
    bool wave_loops_ = false;                         // whether the wave loops, for the ripper's length analysis
    Callback callback_;                               // +0x12c
    Voice* voice_ = nullptr;                          // +0x134
    Channel* next_in_track_ = nullptr;                // +0x138
    std::array<nnsnd::WaveBuffer, 4> wave_buffers_;   // 2 per channel
    std::array<nnsnd::AdpcmContext, 2> contexts_;
    std::array<nnsnd::AdpcmContext, 2> loop_contexts_;

private:
    void AppendWaveBuffer(const WaveInfo& info); // 0x320914
};

// nw::snd NoteOnInfo, built by SequenceTrack::NoteOn for the bank (code.bin 0x31e374).
struct NoteOnInfo
{
    uint32_t prg_no = 0;
    int key = 60;
    int velocity = 127;
    int32_t length = -1;
    int init_pan = 0;
    int priority = 64;
    Channel::Callback callback;
};

class ChannelManager
{
public:
    // The channels in use and their values, for a snapshot of the engine (see VoiceManager::Snapshot).
    struct Snapshot
    {
        std::list<std::shared_ptr<Channel>> pool;
        std::vector<Channel> values; // of the channels in `pool`, in its order
        std::list<Channel*> active_list;
    };

    explicit ChannelManager(Engine& engine) : engine_(engine)
    {
    }

    // Channel::AllocChannel (0x320868)
    Channel* AllocChannel(int channel_count, int priority, Channel::Callback callback);
    void Free(Channel* ch);

    // 0x1742a0
    void UpdateAllChannel();

    int ActiveCount() const;

    // Whether a channel's envelope is in its attack, hold, decay or release, for the ripper's length analysis.
    bool AnyChanging() const;

    // Releases channel objects freed during the frame.
    void CollectGarbage()
    {
        graveyard_.clear();
    }

    // Saves state between frames, after freed channels have been released.
    Snapshot Save() const;
    void Restore(const Snapshot& snapshot);

private:
    Engine& engine_;
    std::list<std::shared_ptr<Channel>> pool_;
    std::vector<std::shared_ptr<Channel>> graveyard_;
    std::list<Channel*> active_list_;
};

} // namespace threesf::nwsnd
