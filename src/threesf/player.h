// SPDX-License-Identifier: MIT

// 3SF player core: loads a .3sf/.mini3sf set and produces its audio, in game mode (the game's sound code on an emulated
// 3DS) or archive mode (3SF's model of the SDK sound player). Both run the game's DSP firmware in Teakra. Portable C++
// with no platform audio or UI. The CLI renderer, the foobar2000 component and the XMPlay plugin use it.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/player/playback.h"
#include "xsflib/player/player.h"

#include "threesf/format.h"

namespace threesf
{
namespace horizon
{

class System;

} // namespace horizon

class ArchivePlayer;

// Loading, starting, rendering, the state, the tags and snapshots come from xsflib's player framework (PlayerBase).
class Player : public xsflib::player::PlayerBase<Player, LoadedSet>
{
public:
    // The emulation's part of a snapshot: game mode's system or archive mode's model. Unchanged memory pages are shared
    // with the previous snapshot.
    struct SourceSnapshot;

    // A snapshot of the player between emulation steps. It holds pointers into this player's emulation and can only be
    // restored to this player, before the next Start.
    using Snapshot = xsflib::player::Snapshot<SourceSnapshot>;

    // The rate players report. The exact rate is 268111856 / 8192 = 32728.498 Hz.
    static constexpr int kSampleRate = kPlayerSampleRate;

    Player();
    ~Player();

    // True for an archive-mode set.
    bool ArchiveMode() const
    {
        return set_.archive.has_value();
    }

private:
    friend class xsflib::player::PlayerBase<Player, LoadedSet>;

    static constexpr const char* kNotPlayable = "no process or archive descriptor";
    static constexpr const char* kStartFailure = "emulation failed: ";
    static constexpr const char* kStepFailure = "emulation failed: ";

    static std::optional<std::string> LoadFile(const std::string& path, const FileReader& reader, Set& set)
    {
        return LoadSet(path, reader, set);
    }

    void Unload();
    bool Playable() const;

    // Game mode: boots the emulated system and runs it until the sound starts (the silence before is skipped). Archive
    // mode: starts the DSP and the sound.
    bool StartSource();

    bool RunStep();

    // The sound has finished when the driver reports status 2.
    bool Finished() const;

    std::vector<int16_t>& Output();
    uint32_t DriverStatus() const;
    uint64_t EmulatedCycles() const;
    std::shared_ptr<const SourceSnapshot> SaveSource(const SourceSnapshot* previous);
    bool CanRestore(const SourceSnapshot* source) const;
    void RestoreSource(const SourceSnapshot& source);

    std::unique_ptr<horizon::System> system_;
    std::unique_ptr<ArchivePlayer> archive_;
};

// Playback with length, fade, volume and seeking, and its options (xsflib::player).
using Playback = xsflib::player::Playback<Player>;
using PlaybackOptions = xsflib::player::PlaybackOptions<Player>;

} // namespace threesf
