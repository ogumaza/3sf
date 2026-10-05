// SPDX-License-Identifier: MIT

// 3SF player core: loads a .3sf/.mini3sf set and produces its audio, in game mode (the game's sound code on an emulated
// 3DS) or archive mode (3SF's model of the SDK sound player). Both run the game's DSP firmware in Teakra. Portable C++
// with no platform audio or UI; used by the CLI renderer and the foobar2000 plugin.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "threesf/format.h"

namespace threesf
{
namespace horizon
{

class System;

} // namespace horizon

class ArchivePlayer;

class Player
{
public:
    enum class State
    {
        kIdle,
        kPlaying,
        kFinished,
        kError
    };

    // Snapshot of the player between emulation steps. Unchanged memory pages are shared with the previous snapshot. It
    // holds pointers into this player's emulation and can only be restored to this player, before the next Start.
    class Snapshot;

    // The rate players report. The exact rate is 268111856 / 8192 = 32728.498 Hz.
    static constexpr int kSampleRate = kPlayerSampleRate;

    Player();
    ~Player();

    // Loads a file and its libraries, without starting emulation.
    bool Load(const std::string& path, const FileReader& reader);

    // Loads an already assembled set.
    bool Load(LoadedSet set);

    // Game mode: boots the emulated system and runs it until the sound starts (the silence before is skipped). Archive
    // mode: starts the DSP and the sound. Can be called again to restart from the beginning.
    bool Start();

    // Renders up to `frames` stereo frames (interleaved L, R) into `out`. Returns the number of frames written, which
    // is less than requested only after an error.
    std::size_t Render(int16_t* out, std::size_t frames);

    State GetState() const
    {
        return state_;
    }

    // Returns the frame count at the end of the emulation step that first reported the sound finished, independent of
    // Render's requested block size. Only meaningful in State::kFinished.
    uint64_t FinishedFrame() const
    {
        return finished_frame_;
    }

    const std::string& Error() const
    {
        return error_;
    }

    const Tags& GetTags() const
    {
        return set_.tags;
    }

    // Tagged length and fade in milliseconds (-1 if absent).
    long long LengthMs() const;
    long long FadeMs() const;

    // Tagged volume (linear gain, 1.0 if absent).
    double Volume() const;

    // True for an archive-mode set.
    bool ArchiveMode() const
    {
        return set_.archive.has_value();
    }

    // The rip's files, as the player loaded them.
    const LoadedSet& GetLoadedSet() const
    {
        return set_;
    }

    // Takes a snapshot, sharing pages with `previous` (best the latest snapshot). Returns nullptr when the player isn't
    // playing, or if saving failed, which leaves the player as it was.
    std::shared_ptr<const Snapshot> Save(const Snapshot* previous = nullptr);

    // Restores `snapshot`, including any audio waiting to be delivered. Returns false if the snapshot belongs to an
    // earlier run, or if restoration fails (see Error()).
    bool Restore(const Snapshot& snapshot);

private:
    bool Fail(const std::string& message);
    bool StartImpl();
    uint32_t DriverStatus() const;
    bool Step();
    std::vector<int16_t>& Output();

    LoadedSet set_;
    std::unique_ptr<horizon::System> system_;
    std::unique_ptr<ArchivePlayer> archive_;
    State state_ = State::kIdle;
    std::string error_;
    std::size_t consumed_ = 0;    // samples of Output() already handed out (two per frame)
    uint64_t dropped_ = 0;        // samples dropped from the front of Output() since the sound started
    uint64_t finished_frame_ = 0; // see FinishedFrame
    uint64_t run_ = 0;            // counts Start calls, so that a snapshot goes with the run it was taken in
};

} // namespace threesf
