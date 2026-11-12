// SPDX-License-Identifier: MIT

// Archive mode (docs/3sf.md): plays the sounds that a set selects from a sound archive, up to 16 together. The model of
// the SDK sound player (nw::snd and nn::snd) in ntdlib::ctr plays them on the DSP firmware in Teakra. A rip made from a
// sound archive, rather than from the game's code, uses archive mode.

#pragma once

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
    // Snapshot of the model between frames, excluding output_. Can only be restored to this player.
    struct Snapshot;

    ArchivePlayer();
    ~ArchivePlayer();

    // Loads the sound archive and the firmware named by the set's archive descriptor, and starts the DSP and the
    // selected sound. Returns an error message on failure.
    std::optional<std::string> Start(const LoadedSet& set);

    // Runs one sound frame (160 output samples at the DSP rate). Returns false if the DSP stopped producing frames.
    bool RunFrame();

    // False once the sequence has ended and no voice is sounding.
    bool Busy() const;

    std::shared_ptr<Snapshot> Save(const Snapshot* previous);
    void Restore(const Snapshot& snapshot);

    // The DSP's output (interleaved L, R), appended by RunFrame.
    std::vector<int16_t>& Output()
    {
        return output_;
    }

private:
    struct State;

    std::vector<int16_t> output_;
    std::unique_ptr<State> state_;
};

} // namespace threesf
