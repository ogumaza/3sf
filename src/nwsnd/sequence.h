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
#include <vector>

#include "csar/formats.h"
#include "nwsnd/driver.h"
#include "nwsnd/envelope.h"

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

    bool Moving() const
    {
        return counter < duration;
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

    // Whether the track won't read on while its notes sound: it waits for them to end (after a note of length 0 with
    // note wait on) and some still sound, or it waits for ever. A track whose notes have all ended reads on at its next
    // tick.
    bool WaitsForNotes() const
    {
        return (note_finish_wait_ && channel_list_ != nullptr) || WaitsForEver();
    }

    // Whether the track will never read on, or not before the game stops the sound. The track waits for a negative
    // number of ticks, waits out a held note (see kHeldTicks in sequence.cpp), or waits for its notes to end while one
    // never will. A note or wait whose length comes from a variable left at its default of -1 makes a negative wait.
    bool WaitsForEver() const
    {
        return wait_ < 0 || held_wait_ || WaitsOnEndlessNote();
    }

    // Whether the track waits for its notes to end while one of them has no length and plays a looping wave. That note
    // ends only when the game stops it.
    bool WaitsOnEndlessNote() const;

    // Whether the track's last pass of a loop changed nothing in the sound, and nothing has changed since (for the
    // ripper's length analysis, while the player has on_loop_ set). A track that polls or counts a variable once a tick
    // makes such idle passes. See CountLoop.
    bool IsIdle() const
    {
        return idle_ && changed_at_ < looped_at_;
    }

    // Whether the track is idle, its last pass left the variables that tracks read as the pass before did, and it has
    // drawn no random number since that pass began. Such a track does the same on every pass until the game changes a
    // variable.
    bool IsStill() const
    {
        return IsIdle() && still_ && drew_at_ < looped_at_;
    }

    // Whether the track's last pass of a loop played no note. A track after track 0 whose passes play no notes doesn't
    // keep a later track from being the main track (see SequenceSoundPlayer::IsMainTrack). A still track whose passes
    // play notes can change the sound again: once its notes end, its next pass starts a new one.
    bool IsQuiet() const
    {
        return quiet_;
    }

    // Whether a note of the track will end when its length runs out. A held note (see kHeldTicks in sequence.cpp)
    // doesn't count: the game stops it first. Nor does a note while the damper is on.
    bool HasEndingNote() const;

    // Whether the track's volume, pan, surround pan or pitch bend is moving to a new value.
    bool IsMoving() const
    {
        return vol_.Moving() || pan_move_.Moving() || span_move_.Moving() || bend_.Moving();
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

    // What a command can change in the track's sound, for the ripper's length analysis: the track's settings, its
    // ramps' places and targets, its newest channel, and the player's tempo, timebase and main volume.
    struct SoundState
    {
        uint32_t prg_no;
        uint8_t bank_index;
        int8_t transpose;
        uint8_t velocity_range, bend_range, priority, porta_key, porta_time, main_send, volume2, biquad_type;
        int8_t init_pan;
        std::array<uint8_t, 4> envelope;
        int16_t hold;
        std::array<uint8_t, 2> fx_send;
        float lpf, biquad_value, sweep_pitch;
        LfoParam lfo;
        uint8_t lfo_type;
        bool mute, tie, mono, porta, damper, front_bypass, note_wait;
        std::array<int32_t, 12> ramps; // the value, target and ticks to go of the track's four ramps
        const Channel* newest;
        uint8_t timebase, main_volume;
        uint16_t tempo;

        bool operator==(const SoundState&) const = default;
    };

    SoundState State() const;

    // Parse, with what the ripper's length analysis follows while the player has on_loop_ set: each command's place in
    // the order that the player's tracks run them, and whether the command changes the track's sound.
    int RunCommand(bool do_note_on);

    void InitParam();                                                 // 0x31e698
    int Parse(bool do_note_on);                                       // 0x49036c (MmlParser::Parse)
    int32_t ReadArg(int type);                                        // 0x4908d4
    void CommandProc(uint32_t cmd, int32_t arg1, int32_t arg2);       // 0x48fad4
    void ReleaseAllChannel();                                         // 0x31ddc4
    void FreeAllChannel();                                            // 0x31dd0c
    void Mute(int mode);                                              // 0x31e5cc
    void OnChannelEvent(Channel* ch, Channel::CallbackStatus status); // 0x31e148
    uint8_t ReadByte();
    Channel::Callback MakeCallback();

    // 0x31e2a4. Returns whether the note changes the sound, for the ripper's length analysis. A note changes it unless
    // it carries on the newest note at the same key and volume, with no sweep. Only a tie or a monophonic note carries
    // on the newest note.
    bool NoteOn(int key, int velocity, int32_t length, bool tie);

    // The track went back to `target`, the start of a loop. The pass that ended there began at the track's last run of
    // `target`. A pass that changed nothing in the sound is idle, such as a pass that only polls or counts a variable.
    // An idle pass is never a loop of the sequence (SequenceSoundPlayer::on_loop_). Any other pass is one if the track
    // is track 0, or if it's the main track (SequenceSoundPlayer::IsMainTrack) and played a note on the way round. A
    // pass that played no note is one too while the track holds a note that sounds until the game stops it
    // (HoldsSustainedNote) and every other open track rests (SequenceSoundPlayer::OthersRest). The pass then changes
    // that note, such as its volume or pitch.
    void CountLoop(uint32_t target);

    // The command being run changed the track's sound.
    void MarkChange();

    // The track read variable `index` (see CommandVariable) in a command's argument or in a comparison. IsRead returns
    // whether a track has read variable `index` so far: any track for the player's and the global variables, and this
    // one for the track's variables.
    void MarkRead(int index);
    bool IsRead(int index) const;

    // Whether the track holds a note that sounds until the game stops it on a looping wave, settled at its envelope's
    // sustain level. Such a note is a held note (see kHeldTicks in sequence.cpp) or a note of length 0.
    bool HoldsSustainedNote() const;

    int16_t* Variable(int index) // 0x31dd40
    {
        return index < 16 ? &vars_[index] : nullptr;
    }

    // A variable by its index in a command: 0 to 15 the player's variables, 16 to 31 the global ones, 32 to 47 the
    // track's. nullptr for any other index.
    int16_t* CommandVariable(int index);

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

    // What the ripper's length analysis follows while the player has on_loop_ set. Times are counts of the commands
    // that the player's tracks have run (SequenceSoundPlayer::commands_run_).
    std::vector<uint64_t> run_;                // when the track last ran each command, or 0 if it hasn't
    uint64_t changed_at_ = 0;                  // when the track last changed the sound
    uint64_t drew_at_ = 0;                     // when the track last drew a random number
    uint64_t looped_at_ = 0;                   // when the track last went back to the start of a loop
    uint64_t noted_at_ = 0;                    // when the track last played a note
    uint64_t conditional_at_ = 0;              // when the track last ran a conditional command
    uint16_t variables_read_ = 0;              // the track's variables that it has read, a bit each
    std::array<int16_t, 48> loop_variables_{}; // the variables that tracks read, at the last loop (0 for the others)
    bool idle_ = false;                        // whether the track's last pass of a loop changed nothing in the sound
    bool still_ = false; // whether that pass was idle, drew no random number and left the variables as the one before
    bool quiet_ = false; // whether that pass played no note
    bool conditional_ = false; // whether the command being run is conditional
    bool holds_ = false;       // whether the track's last note is a held one (see kHeldTicks in sequence.cpp)
    bool held_wait_ = false;   // whether the track waits out a held note
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

    // Whether the sequence has started and hasn't ended.
    bool IsActive() const
    {
        return started_ && !finished_;
    }

    // Detaches every track's channels (see SequenceTrack::DetachChannels), before the player goes away.
    void DetachChannels();

    // Sets variable `index` to `value` before the sequence starts. A game can do that through the sound's handle. 0 to
    // 15 are the player's variables, and 16 to 31 the global ones. Other indices change nothing.
    void SetVariable(int index, int16_t value);

    // Whether no open track will change the sound while the notes sound, or the tempo is 0 (for the ripper's length
    // analysis). Each open track waits for its notes to end or waits for ever, or only makes still passes
    // (SequenceTrack::IsStill) and has no note whose length will run out (SequenceTrack::HasEndingNote). Then the
    // sequence plays on unchanged while the notes sound.
    bool IsSettled() const;

    // Whether no open track will change the sound again, or the tempo is 0. Each open track waits for ever, or only
    // makes still passes that play no notes (SequenceTrack::IsQuiet).
    bool IsStuck() const;

    // Whether any open track's volume, pan, surround pan or pitch bend is moving to a new value.
    bool IsMoving() const;

    // When a track last changed the sound, counted in the commands that the tracks have run while on_loop_ is set (for
    // the ripper's length analysis). A track changes the sound when it plays a note, changes a setting or a ramp, or
    // opens a track. 0 if no track has.
    uint64_t ChangedAt() const
    {
        return changed_at_;
    }

    // A pass of track `track` that changed the sound ended with a jump back to `target`, a command the track has
    // already run, or at the end of a loop that repeats for ever and starts at `target`: a loop of the sequence (for
    // the ripper's length analysis). The track is track 0, or the main track if it played a note on the way round (see
    // IsMainTrack). A later track also counts if it holds a note that sounds until the game stops it while every other
    // open track rests (see OthersRest). A jump back into code that the track hasn't run isn't a loop. Sequences that
    // share their start make such jumps.
    std::function<void(int track, uint32_t target)> on_loop_;

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

    // Whether track `index` is the main track: every track before it is closed, idle (SequenceTrack::IsIdle) or waiting
    // for ever (SequenceTrack::WaitsForEver), or comes after track 0 and plays no notes in its passes
    // (SequenceTrack::IsQuiet). The main track's loops are the sequence's loops. Track 0 keeps every later track from
    // the loops while its passes aren't idle.
    bool IsMainTrack(int index) const;

    // Whether every open track but track `index` is idle (SequenceTrack::IsIdle) or waits for ever
    // (SequenceTrack::WaitsForEver).
    bool OthersRest(int index) const;

    // Whether `test` holds for every open track (and true when none is open).
    bool EveryOpenTrack(const std::function<bool(const SequenceTrack& track)>& test) const;

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

    // What the ripper's length analysis follows while on_loop_ is set: the commands that the tracks have run, when a
    // track last changed the sound (ChangedAt), and the player's and the global variables that a track has read, a bit
    // each.
    uint64_t commands_run_ = 0;
    uint64_t changed_at_ = 0;
    uint32_t variables_read_ = 0;
};

} // namespace threesf::nwsnd
