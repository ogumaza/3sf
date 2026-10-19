// SPDX-License-Identifier: MIT

// The ripper's choice of music for 3sfrip's --bgm: by a sound's label, or by the shape of its sequence.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "ntdlib/sar/formats.h"

namespace threesf::rip
{

// What a walk through a sequence's commands reaches from where the sequence starts: every track it opens and every
// branch, without playing it.
struct SequenceShape
{
    int tracks = 1; // the track it starts on, and one for each track it opens
    int notes = 0;  // note commands, each counted once
};

// Walks `data`, a sequence's commands (the payload of its CSEQ's DATA block), from `start`.
SequenceShape WalkSequence(std::span<const uint8_t> data, uint32_t start);

// Whether a sound's label marks it as music: its first two words (the runs of ASCII letters and digits in it, in any
// case) are SEQ and M, or one of its words starts with BGM, or is JIN, JINGLE or FANFARE with or without a number.
bool LabelMarksMusic(std::string_view label);

// Whether a sound is music, as 3sfrip's --bgm picks: its label marks it as music, or it's a sequence that opens at
// least two tracks besides the one it starts on and has at least 50 notes. The second rule finds the music that a label
// doesn't mark, and the music in archives without labels.
bool IsMusic(const ntdlib::sar::SoundArchive& archive, const ntdlib::sar::SoundInfo& sound);

} // namespace threesf::rip
