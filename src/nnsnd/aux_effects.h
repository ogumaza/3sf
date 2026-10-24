// SPDX-License-Identifier: MIT

// The effects on nn::snd's two aux buses: the reverbs and delays of nw::snd and nn::snd that a game puts there, on
// ntdlib's models of them (ntdlib::ctr_fx). The games set their effects up at run time. In archive mode, the set's
// archive descriptor gives them (docs/3sf.md, "Effects on the aux buses").
//
// The DSP mixes each bus into the intermediate mix (the firmware's structure 7) of the region that it writes. nn::snd's
// send step (code.bin 0x18b2d4) processes each bus there in place: first its aux callback, then the effect in its slot
// (0x18d5c8). nw::snd's aux callback (0x186ebc) runs the effects that the game has appended to the bus, in order.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "ntdlib/ctr_fx/effects.h"

namespace threesf::nnsnd
{

// The aux buses, A and B.
constexpr int kAuxBusCount = 2;

// A bus's frame: four channels of 160 samples each, one channel after another (front left, front right, rear left
// and rear right).
constexpr std::size_t kAuxChannelSamples = 160;
constexpr std::size_t kAuxFrameSamples = 4 * kAuxChannelSamples;
using AuxFrame = std::array<int32_t, kAuxFrameSamples>;

// An effect on an aux bus, and the library whose code it follows. The kinds are those of the archive descriptor.
struct AuxEffect
{
    enum class Kind : uint32_t
    {
        kNwSndReverb = 1,     // nw::snd's FxReverb
        kNnSndReverb = 2,     // nn::snd's FxReverb, as games from 2013 on link it
        kNnSndReverb2011 = 3, // nn::snd's FxReverb, as Ocarina of Time 3D links it
        kDelay = 4,           // nw::snd's FxDelay, or nn::snd's: the two compute the same
    };

    // Whether `kind` is one of the kinds above.
    static bool Known(uint32_t kind);

    // Whether the effect is a reverb, with the parameters in `reverb`, rather than a delay, with those in `delay`.
    bool IsReverb() const;

    // The samples that the model's buffers hold.
    uint64_t BufferSamples() const;

    Kind kind = Kind::kNwSndReverb;
    ntdlib::ctr_fx::ReverbParam reverb;
    ntdlib::ctr_fx::DelayParam delay;
};

// The effects on each bus, in the order in which they process a frame. A game's nw::snd effects come first, in the
// order of nw::snd's list, then at most one of nn::snd's.
using AuxEffectLists = std::array<std::vector<AuxEffect>, kAuxBusCount>;

// The models of the effects on the buses. A copy holds the models' whole state.
class AuxEffects
{
public:
    AuxEffects() = default;

    // Sets the models of `lists` up, with their buffers in silence. Throws std::invalid_argument for a kind that
    // isn't known and for parameters that the libraries refuse.
    explicit AuxEffects(const AuxEffectLists& lists);

    // Whether bus `bus` has an effect.
    bool Has(int bus) const;

    // Runs the effects of bus `bus` over a frame, in place.
    void Process(int bus, AuxFrame& frame);

private:
    using Model = std::variant<ntdlib::ctr_fx::Reverb, ntdlib::ctr_fx::Delay>;

    std::array<std::vector<Model>, kAuxBusCount> models_;
};

} // namespace threesf::nnsnd
