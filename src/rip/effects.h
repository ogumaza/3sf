// SPDX-License-Identifier: MIT

// The effects that games put on the aux buses, in archive-mode rips of their sound archives (docs/3sf.md, "Effects on
// the aux buses"). A game sets its effects up at run time, and its sound archive doesn't hold them. ntdlib's table
// (ntdlib/ctr/snd/game_effects.h) gives them for each known game. 3sfrip writes them into the archive descriptor of
// each rip of the game.

#pragma once

#include <cstdint>
#include <string>

#include "ntdlib/ctr/snd/game_effects.h"

#include "nnsnd/aux_effects.h"

namespace threesf::rip
{

// The effect set of the rips whose sequences the game plays with effect number `game_effect` (ntdlib's
// SequenceEffect), in the list of `effects`: N + 1 for its effect N, or 0 for the archive's effects. A rip of such a
// sequence gets that effect set of the archive descriptor (docs/3sf.md, "Effect sets"). kArchiveEffects, a number
// missing from the list and a null `effects` give 0.
uint32_t EffectSetOf(const ntdlib::ctr::snd::ArchiveEffects* effects, uint8_t game_effect);

// What `effects` puts on each bus, for 3sfrip's output, such as "bus A nw::snd's FxReverb, bus B none".
std::string DescribeEffects(const nnsnd::AuxEffectLists& effects);

} // namespace threesf::rip
