// SPDX-License-Identifier: MIT

#include "nnsnd/aux_effects.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

#include "ntdlib/ctr_fx/effects.h"

namespace threesf::nnsnd
{
namespace
{

using ntdlib::ctr_fx::Delay;
using ntdlib::ctr_fx::Library;
using ntdlib::ctr_fx::Reverb;

// The model of `effect`, set up as its library sets the effect up.
std::variant<Reverb, Delay> MakeModel(const AuxEffect& effect)
{
    switch (effect.kind)
    {
    case AuxEffect::Kind::kNwSndReverb:
        return Reverb(Library::kNwSnd, effect.reverb);
    case AuxEffect::Kind::kNnSndReverb:
        return Reverb(Library::kNnSnd, effect.reverb);
    case AuxEffect::Kind::kNnSndReverb2011:
        return Reverb(Library::kNnSnd2011, effect.reverb);
    case AuxEffect::Kind::kDelay:
        return Delay(effect.delay);
    }

    throw std::invalid_argument("an effect of kind " + std::to_string(static_cast<uint32_t>(effect.kind)));
}

} // namespace

bool AuxEffect::Known(uint32_t kind)
{
    return kind >= static_cast<uint32_t>(Kind::kNwSndReverb) && kind <= static_cast<uint32_t>(Kind::kDelay);
}

bool AuxEffect::IsReverb() const
{
    return kind == Kind::kNwSndReverb || kind == Kind::kNnSndReverb || kind == Kind::kNnSndReverb2011;
}

uint64_t AuxEffect::BufferSamples() const
{
    // The models lay each channel's buffers out in whole frames (ntdlib/ctr_fx/effects.h).
    if (!IsReverb())
    {
        const uint64_t channels = delay.enable_surround ? 4 : 2;
        return uint64_t{ntdlib::ctr_fx::DelayFrames(delay.delay_time)} * kAuxChannelSamples * channels;
    }

    uint64_t frames = uint64_t{ntdlib::ctr_fx::ReverbFrames(reverb.early_reflection_time)} +
                      ntdlib::ctr_fx::ReverbFrames(reverb.pre_delay_time);
    for (const uint32_t size : reverb.filter_sizes)
    {
        frames += size / kAuxChannelSamples;
    }

    const uint64_t channels = reverb.enable_surround ? 4 : 2;
    return frames * kAuxChannelSamples * channels;
}

AuxEffects::AuxEffects(const AuxEffectLists& lists)
{
    for (std::size_t bus = 0; bus < models_.size(); bus++)
    {
        for (const AuxEffect& effect : lists[bus])
        {
            models_[bus].push_back(MakeModel(effect));
        }
    }
}

bool AuxEffects::Has(int bus) const
{
    return !models_[static_cast<std::size_t>(bus)].empty();
}

void AuxEffects::Process(int bus, AuxFrame& frame)
{
    const ntdlib::ctr_fx::Channels channels = {&frame[0], &frame[kAuxChannelSamples], &frame[2 * kAuxChannelSamples],
                                               &frame[3 * kAuxChannelSamples]};
    for (Model& model : models_[static_cast<std::size_t>(bus)])
    {
        std::visit([&channels](auto& effect) { effect.Process(channels, kAuxChannelSamples); }, model);
    }
}

} // namespace threesf::nnsnd
