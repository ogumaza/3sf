// SPDX-License-Identifier: MIT

// The games' effects on the aux buses (see effects.h).

#include "rip/effects.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/ctr_fx/effects.h"

#include "nnsnd/aux_effects.h"
#include "rip/game.h"

namespace threesf::rip
{
namespace
{

using nnsnd::AuxEffect;
using ntdlib::ctr_fx::DelayParam;
using ntdlib::ctr_fx::ReverbParam;

// The key of a game's entry: its program ID. Keys of another kind, such as the CRC-32 of each build's code, would
// change this function and the keys alone.
uint64_t KeyOf(const GameFiles& game)
{
    return game.ProgramId();
}

struct Entry
{
    uint64_t key;
    GameEffects effects;
};

AuxEffect Reverb(AuxEffect::Kind kind, const ReverbParam& param)
{
    AuxEffect effect;
    effect.kind = kind;
    effect.reverb = param;

    return effect;
}

AuxEffect Delay(const DelayParam& param)
{
    AuxEffect effect;
    effect.kind = AuxEffect::Kind::kDelay;
    effect.delay = param;

    return effect;
}

// nn::snd's FxDelay with one of the 18 presets of Ocarina of Time 3D and Majora's Mask 3D: a time and a feedback, with
// a damping of 0.2 and all four channels.
AuxEffect DelayPreset(uint32_t time, float feedback)
{
    return Delay({.delay_time = time, .feedback = feedback, .damping = 0.2f, .enable_surround = true});
}

const std::vector<Entry>& Entries()
{
    static const std::vector<Entry> kEntries = {
        // Pokémon X: the reverb that the field module starts on aux bus A, with the parameters that game mode's driver
        // passes (the game's profile in rip.cpp). The game runs it while the player is in the field. A rip runs it for
        // every sound.
        {0x0004000000055d00,
         {"Pokémon X",
          {{{Reverb(AuxEffect::Kind::kNwSndReverb, {.early_reflection_time = 0,
                                                    .fused_time = 4600,
                                                    .pre_delay_time = 1,
                                                    .coloration = 0.5f,
                                                    .damping = 0.91f,
                                                    .filter_sizes = {1120, 1440, 4960},
                                                    .early_gain = 0.3f,
                                                    .fused_gain = 0.73f})},
            {}}}}},

        // WarioWare Gold: a reverb on each bus, for the whole game. The game sets them up at 0x105a74, through
        // 0x7082c0 and 0x3cb18c (0x7084f0 and 0x3cb44c in the Rev 1 build). One of its modes also puts a pitch
        // shifter on bus A, which the rips leave out.
        {0x00040000001d1c00,
         {"WarioWare Gold",
          {{{Reverb(AuxEffect::Kind::kNwSndReverb, {.early_reflection_time = 60,
                                                    .fused_time = 800,
                                                    .pre_delay_time = 100,
                                                    .coloration = 0.5f,
                                                    .damping = 0.4f,
                                                    .filter_sizes = {3040, 3680, 2080},
                                                    .early_gain = 0.6f,
                                                    .fused_gain = 0.4f})},
            {Reverb(AuxEffect::Kind::kNwSndReverb, {.early_reflection_time = 40,
                                                    .fused_time = 4000,
                                                    .pre_delay_time = 150,
                                                    .coloration = 0.25f,
                                                    .damping = 0.4f,
                                                    .filter_sizes = {2720, 3680, 2080},
                                                    .early_gain = 0.6f,
                                                    .fused_gain = 0.4f})}}}}},

        // The Legend of Zelda: Ocarina of Time 3D: 0x465b34 puts nn::snd's FxReverb on bus A for good, with nn::snd's
        // defaults. A scene picks bus B's delay from 18 presets (0x32c5dc). The rips take preset 10 for every sound,
        // the one that two of the game's set-ups pick (0x450bc8, 0x458eec). The game multiplies its time by 4.
        {0x0004000000033500,
         {"The Legend of Zelda: Ocarina of Time 3D",
          {{{Reverb(AuxEffect::Kind::kNnSndReverb2011, {})}, {DelayPreset(128, 0.01875f)}}}}},

        // The Legend of Zelda: Majora's Mask 3D: no effect on bus A. A scene picks bus B's delay from the same 18
        // presets (0x179b9c). The rips take preset 7 for every sound, the one that the game picks at boot (0x15d198).
        {0x0004000000125500, {"The Legend of Zelda: Majora's Mask 3D", {{{}, {DelayPreset(80, 0.625f)}}}}},
    };

    return kEntries;
}

// The effect's name in 3sfrip's output.
const char* NameOf(const AuxEffect& effect)
{
    switch (effect.kind)
    {
    case AuxEffect::Kind::kNwSndReverb:
        return "nw::snd's FxReverb";
    case AuxEffect::Kind::kNnSndReverb:
        return "nn::snd's FxReverb";
    case AuxEffect::Kind::kNnSndReverb2011:
        return "nn::snd's FxReverb of 2011";
    case AuxEffect::Kind::kDelay:
        return "FxDelay";
    }

    return "an unknown effect";
}

} // namespace

const GameEffects* FindGameEffects(const GameFiles& game)
{
    const uint64_t key = KeyOf(game);
    for (const Entry& entry : Entries())
    {
        if (entry.key == key)
        {
            return &entry.effects;
        }
    }

    return nullptr;
}

std::string DescribeEffects(const nnsnd::AuxEffectLists& effects)
{
    std::string text;
    for (std::size_t bus = 0; bus < effects.size(); bus++)
    {
        text += bus == 0 ? "bus A " : ", bus B ";
        if (effects[bus].empty())
        {
            text += "none";
        }

        for (std::size_t i = 0; i < effects[bus].size(); i++)
        {
            text += (i > 0 ? " then " : "") + std::string(NameOf(effects[bus][i]));
        }
    }

    return text;
}

} // namespace threesf::rip
