// SPDX-License-Identifier: MIT

// nw::snd sequence playback (CTR): SequenceTrack with its MML parser, and SequenceSoundPlayer. Modelled on Pokemon X's
// code.bin: a method that follows one of the game's routines gives its address, and a field comment gives the field's
// offset in the game's object (TP = track + 0x1c, the parser track param).

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "csar/formats.h"
#include "nwsnd/driver.h"

namespace threesf::nwsnd
{

class Engine;
class SequenceSoundPlayer;

// MoveValue<T, int16_t>: a linear ramp to `target` over `duration` sequence ticks.
template <typename T>
struct MoveValue
{
    void Init(T v)
    {
        start = v;
        target = v;
        duration = 0;
        counter = 0;
    }

    void Update()
    {
        if (counter < duration)
        {
            counter++;
        }
    }

    T Get() const
    {
        if (counter >= duration)
        {
            return target;
        }

        const int diff = static_cast<int>(target) - static_cast<int>(start);
        return static_cast<T>(diff * counter / duration + static_cast<int>(start));
    }

    void SetTarget(T t, int16_t frames)
    {
        start = Get();
        counter = 0;
        target = t;
        duration = frames;
    }

    T start{};
    T target{};
    int16_t duration = 0;
    int16_t counter = 0;
};

class SequenceTrack
{
public:
    SequenceTrack(SequenceSoundPlayer& player, int index);

    // Clears callbacks of channels still pointing at this track before it is destroyed. This cannot be done in the
    // destructor: snapshots hold copies of tracks, and destroying those copies must not affect the channels.
    void DetachChannels();

    void SetSeqData(std::span<const uint8_t> data, uint32_t offset); // 0x186d80
    void Open();                                                     // 0x186d90
    void Close();                                                    // 0x31e240

    bool IsOpen() const
    {
        return open_;
    }

    // 0x31dbf4: returns -1 when the track reached FIN, 1 otherwise (0 if closed).
    int ParseNextTick(bool do_note_on);

    void UpdateChannelLength(); // 0x31e1c4
    void UpdateChannelParam();  // 0x31de18

private:
    enum ArgType
    {
        kArgNone = 0,
        kArgU8 = 1,
        kArgS16 = 2,
        kArgVmidi = 3,
        kArgRandom = 4,
        kArgVariable = 5
    };

    struct CallStackEntry
    {
        bool is_loop = false;
        uint8_t loop_count = 0;
        uint32_t address = 0;
    };

    void InitParam();                                                 // 0x31e698
    int Parse(bool do_note_on);                                       // 0x49036c (MmlParser::Parse)
    int32_t ReadArg(int type);                                        // 0x4908d4
    void CommandProc(uint32_t cmd, int32_t arg1, int32_t arg2);       // 0x48fad4
    void ReleaseAllChannel();                                         // 0x31ddc4
    void FreeAllChannel();                                            // 0x31dd0c
    void Mute(int mode);                                              // 0x31e5cc
    void NoteOn(int key, int velocity, int32_t length, bool tie);     // 0x31e2a4
    void OnChannelEvent(Channel* ch, Channel::CallbackStatus status); // 0x31e148
    uint8_t ReadByte();
    Channel::Callback MakeCallback();

    int16_t* Variable(int index) // 0x31dd40
    {
        return index < 16 ? &vars_[index] : nullptr;
    }

    SequenceSoundPlayer* player_; // a pointer, so that the track can be copied into a snapshot
    int index_;
    bool open_ = false;             // +0x05
    std::span<const uint8_t> data_; // TP+0x00 base
    uint32_t current_ = 0;          // TP+0x04 current address (offset into data)
    bool has_data_ = false;
    bool cmp_flag_ = true;                                                   // TP+0x08
    bool note_wait_ = true;                                                  // TP+0x09
    bool tie_ = false;                                                       // TP+0x0a
    bool mono_ = false;                                                      // TP+0x0b
    std::array<CallStackEntry, 3> call_stack_;                               // TP+0x0c
    uint8_t call_depth_ = 0;                                                 // TP+0x24
    bool front_bypass_ = false;                                              // TP+0x25 (InitParam leaves it)
    int32_t wait_ = 0;                                                       // TP+0x28
    bool mute_ = false;                                                      // TP+0x2c
    bool note_finish_wait_ = false;                                          // TP+0x2e
    bool porta_ = false;                                                     // TP+0x2f
    bool damper_ = false;                                                    // TP+0x30
    uint8_t bank_index_ = 0;                                                 // TP+0x31
    uint32_t prg_no_ = 0;                                                    // TP+0x34
    LfoParam lfo_;                                                           // TP+0x38
    uint8_t lfo_type_ = 0;                                                   // TP+0x48
    float sweep_pitch_ = 0.0f;                                               // TP+0x4c
    MoveValue<uint8_t> vol_;                                                 // TP+0x50
    MoveValue<int8_t> pan_move_;                                             // TP+0x56
    MoveValue<int8_t> span_move_;                                            // TP+0x5c
    MoveValue<int8_t> bend_;                                                 // TP+0x62
    uint8_t volume2_ = 127;                                                  // TP+0x68
    uint8_t velocity_range_ = 127;                                           // TP+0x69
    uint8_t bend_range_ = 2;                                                 // TP+0x6a
    int8_t init_pan_ = 0;                                                    // TP+0x6b
    int8_t transpose_ = 0;                                                   // TP+0x6c
    uint8_t priority_ = 64;                                                  // TP+0x6d
    uint8_t porta_key_ = 60;                                                 // TP+0x6e
    uint8_t porta_time_ = 0;                                                 // TP+0x6f
    uint8_t attack_ = 0xff, decay_ = 0xff, sustain_ = 0xff, release_ = 0xff; // TP+0x70..0x73
    int16_t hold_ = 0xff;                                                    // TP+0x74
    uint8_t main_send_ = 127;                                                // TP+0x76
    std::array<uint8_t, 2> fx_send_{};                                       // TP+0x77
    uint8_t biquad_type_ = 0;                                                // TP+0x79
    float lpf_ = 0.0f;                                                       // TP+0x7c
    float biquad_value_ = 0.0f;                                              // TP+0x80
    std::array<int16_t, 16> vars_{};                                         // +0xa0
    Channel* channel_list_ = nullptr;                                        // +0xc4
};

class SequenceSoundPlayer
{
public:
    SequenceSoundPlayer(Engine& engine, const csar::SoundInfo& info, csar::Sequence sequence,
                        std::array<const csar::Bank*, 4> banks);

    // Allocates the tracks named by the sound's allocate-track flags, points track 0 at the start offset and starts
    // playback (0x31fe8c, Setup, SetSeqData, 0x320408).
    void Start();

    // 0x3202c8: tick processing and channel parameter update for one sound frame.
    void Update();

    bool IsFinished() const
    {
        return finished_;
    }

    // Detaches every track's channels (see SequenceTrack::DetachChannels), before the player goes away.
    void DetachChannels();

    // Loop detection for rendering: called by track 0 when it jumps backwards.
    std::function<void()> on_loop_;

private:
    friend class SequenceTrack;

    Engine& GetEngine()
    {
        return *engine_;
    }

    const csar::Bank* GetBank(int i) const
    {
        return i >= 0 && i < 4 ? banks_[i] : nullptr;
    }

    SequenceTrack* GetTrack(int i) // 0x180b6c
    {
        return i >= 0 && i < 16 && tracks_[i] ? &*tracks_[i] : nullptr;
    }

    int16_t* Variable(int index); // 0x3201ec
    void UpdateTick();            // 0x31ff50

    // A pointer, and tracks kept in place, so that a snapshot can hold a copy of the player and copy it back without
    // moving the tracks, which channels' callbacks point at.
    Engine* engine_;
    csar::Sequence sequence_;
    std::array<const csar::Bank*, 4> banks_;
    uint32_t allocate_track_flags_;
    uint32_t start_offset_;
    std::array<std::optional<SequenceTrack>, 16> tracks_;
    std::array<int16_t, 16> local_vars_{}; // +0xcc
    float tick_fraction_ = 0.0f;           // +0x68
    bool started_ = false;                 // +0x0d
    bool finished_ = false;                // +0x0f

    // The BasicSoundPlayer params (0x31f1a0) that BasicSound sets from the sound info. The game can also set the
    // player's pitch, pan, filters, sends, pan range and tempo ratio, and the track's volume, pitch and pan. 3SF never
    // does, and at their defaults they leave every result unchanged.
    float volume_ = 1.0f;   // +0x14
    uint8_t pan_mode_ = 0;  // +0x34
    uint8_t pan_curve_ = 0; // +0x35

    // SequenceSoundPlayer params.
    bool release_priority_fix_ = false; // +0x5c
    uint8_t main_volume_ = 127;         // +0x74
    uint8_t channel_priority_ = 64;     // +0x75
    uint8_t timebase_ = 48;             // +0x76
    uint16_t tempo_ = 120;              // +0x78
};

} // namespace threesf::nwsnd
