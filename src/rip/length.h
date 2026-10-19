// SPDX-License-Identifier: MIT

// The ripper's length analysis: how long each sequence of a sound archive plays, worked out by running it on the
// nw::snd model and the DSP firmware. docs/3sf.md, "Lengths", has the rules.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "xsflib/rip/length.h"

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
    // Analyzes sequences of `archive_bytes` on `dsp_component`, with `variables` set before each sequence starts (see
    // the 3sf_var tag in docs/3sf.md).
    LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const std::vector<uint8_t>& dsp_component,
                   const Variables& variables = {});

    ~LengthAnalyzer();

    // A looping sequence gets two loops and a 10 s fade, and any other sequence runs to its last sound and a tail of
    // 0.5 s, for at most 600 s. docs/3sf.md, "Lengths", has the rules. Returns nullopt when the length can't be worked
    // out (and then no length tag is written), and throws std::bad_alloc when memory runs out.
    std::optional<Timing> Analyze(uint32_t sound_index) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace threesf::rip
