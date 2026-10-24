// SPDX-License-Identifier: MIT

// The effects that games put on the aux buses, for archive-mode rips of their sound archives (docs/3sf.md, "Effects on
// the aux buses"). A game sets its effects up at run time, and its sound archive doesn't hold them. 3sfrip writes them
// into the archive descriptor of each rip of the game.

#pragma once

#include <string>

#include "nnsnd/aux_effects.h"
#include "rip/game.h"

namespace threesf::rip
{

// The effects that a game puts on the aux buses.
struct GameEffects
{
    const char* game; // the game's name, for 3sfrip's output
    nnsnd::AuxEffectLists buses;
};

// The effects of `game`, or nullptr for a game without an entry. The entries are found by the game's program ID.
// Another region's release of a game has another ID. It has no entry until someone has read its code.
const GameEffects* FindGameEffects(const GameFiles& game);

// What `effects` puts on each bus, for 3sfrip's output, such as "bus A nw::snd's FxReverb, bus B none".
std::string DescribeEffects(const nnsnd::AuxEffectLists& effects);

} // namespace threesf::rip
