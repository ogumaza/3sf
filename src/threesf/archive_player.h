// SPDX-License-Identifier: MIT

// Archive mode (docs/3sf.md): one sequence from a sound archive, played by 3SF's model of the SDK sound player (nw::snd
// and nn::snd, src/nwsnd and src/nnsnd) driving the DSP firmware in Teakra. Used when a rip is made from a sound
// archive rather than from the game's code.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "threesf/format.h"

namespace threesf
{

class ArchivePlayer
{
public:
    ArchivePlayer();
    ~ArchivePlayer();

    // Loads the sound archive and the firmware named by the set's archive descriptor, and starts the DSP and the
    // selected sound. Returns an error message on failure.
    std::optional<std::string> Start(const LoadedSet& set);

    // Runs one sound frame (160 output samples at the DSP rate). Returns false if the DSP stopped producing frames.
    bool RunFrame();

    // False once the sequence has ended and no voice is sounding.
    bool Busy() const;

    // The DSP's output (interleaved L, R), appended by RunFrame.
    std::vector<int16_t> output_;

    // Snapshot of the model between frames, excluding output_. Can only be restored to this player.
    struct Snapshot;

    std::shared_ptr<Snapshot> Save(const Snapshot* previous);
    void Restore(const Snapshot& snapshot);

    // Approximate memory used by `snapshot`, excluding pages shared with `previous`.
    static std::size_t SnapshotBytes(const Snapshot& snapshot, const Snapshot* previous);

private:
    struct State;

    std::unique_ptr<State> state_;
};

} // namespace threesf
