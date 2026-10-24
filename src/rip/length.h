// SPDX-License-Identifier: MIT

// The ripper's length analysis: how long each sequence of a sound archive plays, worked out by running it on the
// nw::snd model and the DSP firmware. docs/3sf.md, "Lengths", has the rules.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "xsflib/rip/length.h"

#include "nnsnd/aux_effects.h"
#include "threesf/format.h"

namespace threesf::rip
{

// A sequence's length and fade in milliseconds, and whether the sequence never makes a sound. A silent sequence gets
// the tail alone.
using Timing = xsflib::rip::Timing;

// Works out how long sequences play by running them on the nw::snd model and the DSP firmware. Analyze can be called
// from several threads at once.
class LengthAnalyzer
{
public:
    // Analyzes sequences of `archive_bytes`, with the group files `group_files` beside it, on `firmware`, with
    // `effects` on the aux buses. The analyzer sets `variables` before each sequence starts (see the 3sf_var tag in
    // docs/3sf.md).
    LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const std::vector<uint8_t>& firmware,
                   const Variables& variables = {}, const std::vector<std::vector<uint8_t>>& group_files = {},
                   const nnsnd::AuxEffectLists& effects = {});

    ~LengthAnalyzer();

    // A looping sequence gets two loops and a 10 s fade. Any other sequence runs to its last sound and a tail of 0.5 s.
    // The analysis stops at 600 s. With effects, the last sound can come from them after the sequence has ended. A
    // sequence with effects ends once its output has been silent for a second. docs/3sf.md, "Lengths", has the rules.
    // Returns nullopt when the length can't be worked out. No length tag is written then. Throws std::bad_alloc when
    // memory runs out.
    std::optional<Timing> Analyze(uint32_t sound_index) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

// The timing of a sound in a game-mode rip whose driver starts a reverb on aux bus A (GameProfile::reverb), for a
// sound that sends to that bus. The reverb rings on after the sound's last note. The model doesn't play the reverb.
// The model's timing, `model`, therefore ends too early. RingOut renders the sound in game mode as a player plays it,
// from `library` and a .mini3sf of `sound_id`. The render stops once it has stayed silent for a second past the end of
// `model`, or 30 s past that end. A sound that ends then ends at the render's last sound and a tail of 0.5 s, if that
// comes later than `model`. A sound that loops keeps `model`: its fade ends the reverb as well. So does a sound that
// never sounds. Returns nullopt when the render fails. Can be called from several threads at once.
std::optional<Timing> RingOut(const std::vector<uint8_t>& library, uint32_t sound_id, const Timing& model);

} // namespace threesf::rip
