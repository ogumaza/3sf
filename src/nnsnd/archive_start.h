// SPDX-License-Identifier: MIT

// The start of archive mode's model with the effects on the aux buses that a set's archive descriptor gives
// (docs/3sf.md, "Effects on the aux buses"). The model of nn::snd runs the effects of each bus in the aux callback of
// the bus (SetAuxCallback of ntdlib::ctr::nnsnd::SoundSystem).

#pragma once

#include <cstdint>
#include <span>

#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/sar/formats.h"

#include "nnsnd/aux_effects.h"

namespace threesf::nnsnd
{

// Starts `model` on `firmware` and `archive`, with `output_mode` (0 mono, 1 stereo, 2 surround), then puts `effects` on
// the aux buses. A game puts its effects there once nn::snd has started. Returns false if the firmware didn't start.
// Throws std::invalid_argument for effects that the libraries refuse, before the model starts. A bus with an effect is
// turned on and returns at full volume (0x17940c and 0x179624). nn::snd then runs its effects in each frame's send
// step. The firmware runs its delays and reverbs. Their lines start in silence in FCRAM.
bool StartArchiveModel(ntdlib::ctr::snd::ArchiveModel& model, std::span<const uint8_t> firmware,
                       const ntdlib::sar::SoundArchive& archive, uint8_t output_mode, const AuxEffectLists& effects);

} // namespace threesf::nnsnd
