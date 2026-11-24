// SPDX-License-Identifier: MIT

// The games' effects on the aux buses (see effects.h).

#include "rip/effects.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "ntdlib/ctr/snd/game_effects.h"

#include "nnsnd/aux_effects.h"

namespace threesf::rip
{
namespace
{

using nnsnd::AuxEffect;

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
    case AuxEffect::Kind::kFirmwareDelay:
        return "the firmware's delay";
    case AuxEffect::Kind::kNwSndReverb2011:
        return "nw::snd's FxReverb of 2011";
    case AuxEffect::Kind::kFirmwareReverb:
        return "the firmware's reverb";
    }

    return "an unknown effect";
}

} // namespace

uint32_t EffectSetOf(const ntdlib::ctr::snd::ArchiveEffects* effects, uint8_t game_effect)
{
    if (!effects)
    {
        return 0;
    }

    for (std::size_t i = 0; i < effects->game_effects.size(); i++)
    {
        if (effects->game_effects[i].number == game_effect)
        {
            return static_cast<uint32_t>(i + 1);
        }
    }

    return 0;
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
