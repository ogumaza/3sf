// SPDX-License-Identifier: MIT

// The effects that games put on the aux buses, for archive-mode rips of their sound archives (docs/3sf.md, "Effects on
// the aux buses"). A game sets its effects up at run time, and its sound archive doesn't hold them. 3sfrip writes them
// into the archive descriptor of each rip of the game.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/ctr_fx/effects.h"
#include "ntdlib/sar/formats.h"

#include "nnsnd/aux_effects.h"
#include "rip/game.h"

namespace threesf::rip
{

// The reverb that Pokémon X's field module starts on aux bus A: nw::snd's FxReverb. Game mode's driver starts it with
// these parameters (GameProfile::reverb in rip.cpp). Archive mode plays it for every sound of the game.
constexpr ntdlib::ctr_fx::ReverbParam kPokemonXFieldReverb = {.early_reflection_time = 0,
                                                              .fused_time = 4600,
                                                              .pre_delay_time = 1,
                                                              .coloration = 0.5f,
                                                              .damping = 0.91f,
                                                              .filter_sizes = {1120, 1440, 4960},
                                                              .early_gain = 0.3f,
                                                              .fused_gain = 0.73f};

// The effects that a game puts on the aux buses.
struct GameEffects
{
    const char* game; // the game's name, for 3sfrip's output
    nnsnd::AuxEffectLists buses;
};

// The effects of `game`, or nullptr for a game without an entry. The entries are found by the game's program ID.
// Another region's release of a game has another ID. It has no entry until someone has read its code.
const GameEffects* FindGameEffects(const GameFiles& game);

// An effect that a known game runs on the aux buses for some of its sequences alone, by the game's number for it
// (ntdlib::ctr::snd::SequenceStart::game_effect).
struct SequenceEffect
{
    uint8_t number;
    nnsnd::AuxEffectLists buses;
};

// The effects that the game of a known sound archive puts on the aux buses. A loose archive, or one in a game's add-on
// content, comes with no program ID of the program that plays it. These entries are found by the archive's SHA-1.
struct ArchiveEffects
{
    const char* game;  // the game's name, for 3sfrip's output
    const char* build; // the release whose archive the entry is, for the note on another build
    const char* label; // a sound that this archive holds and the others of the table don't
    nnsnd::AuxEffectLists buses;

    // The effects that the game runs for some sequences alone. A rip of such a sequence gets an effect set of the
    // archive descriptor (docs/3sf.md, "Effect sets"): set N + 1 for the entry's effect N.
    std::vector<SequenceEffect> game_effects = {};
};

// The effect set of the rips whose sequences the game plays with effect number `game_effect` (see SequenceEffect), in
// the list of `effects`: N + 1 for its effect N, or 0 for the archive's effects. kArchiveEffects, a number that the
// list lacks and a null `effects` give 0.
uint32_t EffectSetOf(const ArchiveEffects* effects, uint8_t game_effect);

// The entry of the archive whose SHA-1 is `archive_sha1`, in 40 lowercase hex digits, or nullptr.
const ArchiveEffects* FindArchiveEffects(std::string_view archive_sha1);

// The entry whose label `archive` holds, when no entry has the archive's SHA-1 `archive_sha1`: another build of a known
// archive. The tables of the effects and of the tracks that games quiet don't know such a build. Otherwise nullptr.
const ArchiveEffects* FindOtherBuild(const ntdlib::sar::SoundArchive& archive, std::string_view archive_sha1);

// What `effects` puts on each bus, for 3sfrip's output, such as "bus A nw::snd's FxReverb, bus B none".
std::string DescribeEffects(const nnsnd::AuxEffectLists& effects);

// The text of the comment tag of sound `sound` of the archive whose SHA-1 is `archive_sha1`, or nullptr. A sound gets
// one when its rip plays it in a way that the game plays it only some of the time.
const char* SoundComment(std::string_view archive_sha1, std::string_view sound);

} // namespace threesf::rip
