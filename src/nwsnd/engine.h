// SPDX-License-Identifier: MIT

// The nw::snd sound engine for one sound archive: owns the voice and channel managers, the HardwareManager state and
// the sequence players, and runs the sound frame (SoundThread::FrameProcess, code.bin 0x1666d0) between the receive and
// send steps of the nn::snd model it's given.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "csar/formats.h"
#include "dsp/fcram.h"
#include "dsp/teakra_dsp.h"
#include "nnsnd/sound_system.h"
#include "nwsnd/driver.h"
#include "nwsnd/sequence.h"
#include "nwsnd/util.h"

namespace threesf::nwsnd
{

struct EngineOptions
{
    // HardwareManager output mode (0 mono, 1 stereo, 2 surround).
    uint8_t output_mode = 1;
};

class Engine
{
public:
    Engine(nnsnd::SoundSystem& snd, Fcram& fcram, const csar::SoundArchive& archive, EngineOptions options = {});
    ~Engine();

    nnsnd::SoundSystem& Snd()
    {
        return snd_;
    }

    VoiceManager& Voices()
    {
        return voices_;
    }

    ChannelManager& Channels()
    {
        return channels_;
    }

    Random& Rng()
    {
        return random_;
    }

    uint8_t OutputMode() const
    {
        return options_.output_mode;
    }

    // Starts a sequence sound by its index in the archive's sound table. Returns the player, or nullptr if there's no
    // such sequence or its file isn't in the archive. Throws if the sequence's data is malformed.
    SequenceSoundPlayer* StartSequence(uint32_t index);

    // Bank::NoteOn (code.bin 0x48f954): finds the velocity region, loads its wave and starts a channel for it.
    Channel* NoteOn(const csar::Bank* bank, const NoteOnInfo& info);

    // One sound frame: nn::snd receive, FrameProcess, nn::snd send. Returns false if the DSP stopped answering.
    bool RunFrame();

    // True while any sequence player is active or any channel is sounding.
    bool IsBusy() const;

    uint64_t FrameCount() const
    {
        return frame_count_;
    }

    // The global sequence variables, which the game keeps in one table for every sequence player (initialized by
    // code.bin 0x13739c). Each engine has its own, so that engines on different threads don't share them.
    std::array<int16_t, 16>& GlobalVariables()
    {
        return global_vars_;
    }

    struct LoadedWarc
    {
        std::span<const uint8_t> file;
        PAddr base = 0; // the whole CWAR's address in FCRAM
        csar::WaveArchive archive;
    };

    // The engine's state, for a snapshot of the model (see ArchiveModel::Snapshot): the voices and channels in use, the
    // players' values and the caches. It keeps alive everything it points at.
    struct Snapshot
    {
        VoiceManager::Snapshot voices;
        ChannelManager::Snapshot channels;
        Random random;
        std::vector<SequenceSoundPlayer> players;
        std::map<uint32_t, std::shared_ptr<const csar::Bank>> banks;
        std::map<uint32_t, std::shared_ptr<const LoadedWarc>> warcs;
        std::map<std::pair<uint32_t, uint32_t>, std::optional<WaveInfo>> waves;
        uint64_t frame_count = 0;
        std::array<int16_t, 16> global_vars{};
    };

    // Between frames.
    Snapshot Save() const;

    // Restores the engine from a snapshot. Restore FCRAM to the same state so cached wave archive addresses remain
    // valid.
    void Restore(const Snapshot& snapshot);

private:
    void FrameProcess();
    const csar::Bank* GetBank(uint32_t bank_item);
    std::optional<WaveInfo> GetWave(uint32_t wave_archive_item, uint32_t index);
    const LoadedWarc* GetWarc(uint32_t wave_archive_item);

    nnsnd::SoundSystem& snd_;
    Fcram& fcram_;
    const csar::SoundArchive& archive_;
    EngineOptions options_;
    VoiceManager voices_;
    ChannelManager channels_;
    Random random_;

    // Declared after channels_ so players are destroyed first, after the destructor detaches their tracks' channels.
    std::vector<std::unique_ptr<SequenceSoundPlayer>> players_;

    // Caches of what the archive's files hold. Entries never change once made, and they're shared, so that snapshots
    // can hold them and players can keep pointers to them.
    std::map<uint32_t, std::shared_ptr<const csar::Bank>> banks_;
    std::map<uint32_t, std::shared_ptr<const LoadedWarc>> warcs_;
    std::map<std::pair<uint32_t, uint32_t>, std::optional<WaveInfo>> waves_;
    uint64_t frame_count_ = 0;
    std::array<int16_t, 16> global_vars_;
};

// The FCRAM that archive mode gives the DSP (docs/3sf.md, "Archive mode").
inline constexpr std::size_t kArchiveModeFcram = 64 * 1024 * 1024;

// Archive mode's whole stack for one sound archive: FCRAM, the DSP, the nn::snd model and the engine. The player and
// the ripper's length analysis both build it this way. A sample sink goes on `dsp` before Start.
struct ArchiveModel
{
    // Starts `firmware` on the DSP and then the engine on `archive`, which has to outlive the engine, with
    // `output_mode` (0 mono, 1 stereo, 2 surround). Returns false if the firmware didn't start.
    bool Start(std::span<const uint8_t> firmware, const csar::SoundArchive& archive, uint8_t output_mode = 1);

    // Snapshot of FCRAM, the DSP, nn::snd and the engine. Keeps active voices and channels alive. It holds pointers to
    // this model's objects, so it can only be restored to this model.
    struct Snapshot
    {
        Fcram::Snapshot fcram;
        dsp::TeakraDsp::Snapshot dsp;
        nnsnd::SoundSystem snd;
        std::optional<Engine::Snapshot> engine;
    };

    // Takes a snapshot between frames, sharing the memory pages that haven't changed since `previous`.
    Snapshot Save(const Snapshot* previous);
    void Restore(const Snapshot& snapshot);

    Fcram fcram{kArchiveModeFcram};
    dsp::TeakraDsp dsp{fcram};
    nnsnd::SoundSystem snd{dsp};
    std::optional<Engine> engine;
};

} // namespace threesf::nwsnd
