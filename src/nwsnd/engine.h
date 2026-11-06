// SPDX-License-Identifier: MIT

// The nw::snd sound engine for one sound archive: owns the voice and channel managers, the HardwareManager state and
// the sequence players, and runs the sound frame (SoundThread::FrameProcess, code.bin 0x1666d0) between the receive and
// send steps of the nn::snd model it's given.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/length_hooks.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/types.h"

#include "dsp/fcram.h"
#include "dsp/teakra_dsp.h"
#include "nnsnd/aux_effects.h"
#include "nnsnd/sound_system.h"
#include "nwsnd/sequence.h"
#include "nwsnd/voice.h"

namespace threesf::nwsnd
{

struct EngineOptions
{
    // HardwareManager output mode (0 mono, 1 stereo, 2 surround).
    uint8_t output_mode = 1;
};

// The ripper's length analysis sets the engine's hooks (LengthHooks). ntdlib's SequenceSoundPlayer reports the
// sequence's loops, every track's passes and the open-track commands to them.
class Engine : public ntdlib::nwsnd::LengthHooks
{
public:
    struct LoadedWarc
    {
        std::span<const uint8_t> file;
        ntdlib::PAddr base = 0; // the whole CWAR's address in FCRAM
        ntdlib::sar::WaveArchive archive;
    };

    // A variable that a player takes at the start of a later frame, before the frame's work (ScheduleVariable).
    struct ScheduledVariable
    {
        std::size_t player = 0; // the player's place in the engine's list
        int index = 0;
        int16_t value = -1;
        uint64_t frame = 0; // the engine's frame count at which the variable changes
    };

    // The engine's state, for a snapshot of the model (see ArchiveModel::Snapshot): a copy of the voices' pool, the
    // channels, the players and the caches. It keeps alive everything it points at.
    struct Snapshot
    {
        VoiceManager::Snapshot voices;
        ChannelManager channels{kChannelCount};
        ntdlib::nwsnd::Random random;
        std::deque<SequenceSoundPlayer> players;
        std::map<uint32_t, std::shared_ptr<const ntdlib::sar::Bank>> banks;
        std::map<uint32_t, std::shared_ptr<const LoadedWarc>> warcs;
        std::map<std::pair<uint32_t, uint32_t>, std::optional<WaveInfo>> waves;
        std::array<int16_t, 16> global_vars{};
        uint64_t frames = 0;
        std::vector<ScheduledVariable> scheduled;
    };

    // More channels than can ever be in use at once. Each channel holds one of the 23 voices. A new note takes a
    // channel before a voice. While a voice is dropped to make room, there can be one channel more.
    static constexpr int kChannelCount = 64;

    Engine(nnsnd::SoundSystem& snd, Fcram& fcram, const ntdlib::sar::SoundArchive& archive, EngineOptions options = {});

    nnsnd::SoundSystem& Snd()
    {
        return snd_;
    }

    // nw::snd's voices, by the places that channels hold.
    VoiceManager& Voices()
    {
        return voices_;
    }

    ChannelManager& Channels()
    {
        return channels_;
    }

    const ChannelManager& Channels() const
    {
        return channels_;
    }

    ntdlib::nwsnd::Random& Rng()
    {
        return random_;
    }

    uint8_t OutputMode() const
    {
        return options_.output_mode;
    }

    // Starts a sequence sound by its index in the archive's sound table. Returns the player, or nullptr if there's no
    // such sequence or its file isn't in the archive. Throws if the sequence's data is malformed. `banks` gives the
    // sequence's four bank slots in place of the archive's, as a game's StartInfo can (SequenceStart::banks).
    SequenceSoundPlayer* StartSequence(uint32_t index,
                                       const std::optional<std::array<uint32_t, 4>>& banks = std::nullopt);

    // Bank::NoteOn (code.bin 0x48f954) for the bank in slot `bank_index` of a player's `bank_items`: finds the velocity
    // region, loads its wave and starts a channel for it.
    Channel* NoteOn(const std::vector<uint32_t>& bank_items, int bank_index, const NoteOnInfo& info);

    // A channel's callback, for its owner: track `track` of player `player`.
    void ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus status);

    // One sound frame: nn::snd receive, the scheduled variables of the frame, FrameProcess, nn::snd send. Returns false
    // if the DSP stopped answering.
    bool RunFrame();

    // Sets variable `index` of `player` (SequenceSoundPlayer::SetVariable) to `value` at the start of frame `frame`
    // of the player, before the frame's ticks. Frame 0 is the frame of the player's first tick: the next frame that
    // RunFrame runs once the player has started. A game changes a variable so through the sound's handle, or through
    // another sound that plays with it. `player` has to be one of the engine's players.
    void ScheduleVariable(const SequenceSoundPlayer& player, int index, int16_t value, uint64_t frame);

    // True while any sequence player is active or any channel is sounding.
    bool IsBusy() const;

    // The global sequence variables, which the game keeps in one table for every sequence player (initialized by
    // code.bin 0x13739c). Each engine has its own, so that engines on different threads don't share them.
    std::array<int16_t, 16>& GlobalVariables()
    {
        return global_vars_;
    }

    // The counted loops that the length analysis makes endless, each a track and its loop's body. ntdlib's
    // SequenceTrack calls LoopsForEver at the end of each pass of a counted loop.
    void SetEndlessLoops(std::set<std::pair<int, uint32_t>> loops)
    {
        endless_loops_ = std::move(loops);
    }

    bool LoopsForEver(const SequenceSoundPlayer&, int track, uint32_t address) const
    {
        return endless_loops_.contains({track, address});
    }

    // Between frames.
    Snapshot Save() const;

    // Restores the engine from a snapshot. Restore FCRAM to the same state so cached wave archive addresses remain
    // valid.
    void Restore(const Snapshot& snapshot);

private:
    void FrameProcess();
    const ntdlib::sar::Bank* GetBank(uint32_t bank_item);
    std::optional<WaveInfo> GetWave(uint32_t wave_archive_item, uint32_t index);
    const LoadedWarc* GetWarc(uint32_t wave_archive_item);

    nnsnd::SoundSystem& snd_;
    Fcram& fcram_;
    const ntdlib::sar::SoundArchive& archive_;
    EngineOptions options_;
    VoiceManager voices_;
    ChannelManager channels_{kChannelCount};
    ntdlib::nwsnd::Random random_;

    // The players by their ids. A deque keeps each player where it is while others start.
    std::deque<SequenceSoundPlayer> players_;

    // Caches of what the archive's files hold. Entries never change once made. The banks and wave archives are shared.
    // Snapshots can then hold them.
    std::map<uint32_t, std::shared_ptr<const ntdlib::sar::Bank>> banks_;
    std::map<uint32_t, std::shared_ptr<const LoadedWarc>> warcs_;
    std::map<std::pair<uint32_t, uint32_t>, std::optional<WaveInfo>> waves_;

    std::array<int16_t, 16> global_vars_;
    std::set<std::pair<int, uint32_t>> endless_loops_;

    // The frames that RunFrame has run, and the variables that change in later frames.
    uint64_t frames_ = 0;
    std::vector<ScheduledVariable> scheduled_;
};

// The FCRAM that archive mode gives the DSP (docs/3sf.md, "Archive mode").
inline constexpr std::size_t kArchiveModeFcram = 64 * 1024 * 1024;

// Archive mode's whole stack for one sound archive: FCRAM, the DSP, the nn::snd model and the engine. The player and
// the ripper's length analysis both build it this way. A sample sink goes on `dsp` before Start.
struct ArchiveModel
{
    // Snapshot of FCRAM, the DSP, nn::snd and the engine. It holds pointers to this model's objects and can only be
    // restored to this model.
    struct Snapshot
    {
        Fcram::Snapshot fcram;
        dsp::TeakraDsp::Snapshot dsp;
        nnsnd::SoundSystem snd;
        std::optional<Engine::Snapshot> engine;
    };

    // Starts `firmware` on the DSP and then the engine on `archive`, which has to outlive the engine, with
    // `output_mode` (0 mono, 1 stereo, 2 surround) and `effects` on the aux buses. Returns false if the firmware didn't
    // start. Throws std::invalid_argument for effects that the libraries refuse.
    bool Start(std::span<const uint8_t> firmware, const ntdlib::sar::SoundArchive& archive, uint8_t output_mode = 1,
               const nnsnd::AuxEffectLists& effects = {});

    // Takes a snapshot between frames, sharing the memory pages that haven't changed since `previous`.
    Snapshot Save(const Snapshot* previous);

    void Restore(const Snapshot& snapshot);

    Fcram fcram{kArchiveModeFcram};
    dsp::TeakraDsp dsp{fcram};
    nnsnd::SoundSystem snd{dsp};
    std::optional<Engine> engine;
};

} // namespace threesf::nwsnd
