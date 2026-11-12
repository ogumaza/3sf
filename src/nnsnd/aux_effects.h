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
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "ntdlib/ctr/dsp/dsp.h"
#include "ntdlib/ctr/nnsnd/dsp_abi.h"
#include "ntdlib/ctr_fx/effects.h"

namespace threesf::nnsnd
{

// The aux buses, A and B.
constexpr int kAuxBusCount = 2;

// The samples of each of a bus's four channels in a frame (front left, front right, rear left and rear right).
constexpr std::size_t kAuxChannelSamples = ntdlib::ctr::dsp::kSamplesPerFrame;

// An effect on an aux bus, and the library whose code it follows. The kinds are those of the archive descriptor.
struct AuxEffect
{
    enum class Kind : uint32_t
    {
        kNwSndReverb = 1,     // nw::snd's FxReverb
        kNnSndReverb = 2,     // nn::snd's FxReverb, as games from 2013 on link it
        kNnSndReverb2011 = 3, // nn::snd's FxReverb, as Ocarina of Time 3D links it
        kDelay = 4,           // nw::snd's FxDelay, or nn::snd's: the two compute the same
        kFirmwareDelay = 5,   // the DSP firmware's delay, which nn::snd sets up
        kNwSndReverb2011 = 6, // nw::snd's FxReverb, as StreetPass Mii Plaza links it (2011)
        kFirmwareReverb = 7,  // the reverb that nn::snd sets up in the DSP firmware
    };

    // Whether `kind` is one of the kinds above.
    static bool Known(uint32_t kind);

    // Whether the effect is a reverb, with the parameters in `reverb`, rather than a delay, with those in `delay`.
    bool IsReverb() const;

    // Whether the firmware runs the effect, rather than the ARM11.
    bool InFirmware() const;

    // The samples that the model's buffers hold.
    uint64_t BufferSamples() const;

    Kind kind = Kind::kNwSndReverb;
    ntdlib::ctr_fx::ReverbParam reverb;
    ntdlib::ctr_fx::DelayParam delay;
};

// The effects on each bus, in the order in which they process a frame. A game's nw::snd effects come first, in the
// order of nw::snd's list, then at most one of nn::snd's. The firmware's delay and reverb, at most one of each on each
// bus, run in the firmware wherever they stand in the list.
using AuxEffectLists = std::array<std::vector<AuxEffect>, kAuxBusCount>;

// Whether `effects` puts an effect on either bus.
bool HasEffects(const AuxEffectLists& effects);

// The firmware's delay as nn::snd's DSP delay sets it up (Luigi's Mansion of 2018, code.bin 0x22d8cc), in the fields of
// the firmware's parameters (ntdlib's dsp_abi.h, FirmwareDelayParams).
struct FirmwareDelay
{
    // Sets the delay up from `param`. Throws std::invalid_argument for a feedback or a damping that nn::snd refuses.
    explicit FirmwareDelay(const ntdlib::ctr_fx::DelayParam& param);

    // The bytes of the delay line in FCRAM: frames × channels × 160 32-bit samples.
    uint64_t LineBytes() const;

    // The bus's record in the firmware's parameters for a line at physical address `line`. nn::snd leaves the same
    // record once it has sent the line, the parameters and the enable.
    ntdlib::ctr::nnsnd::FirmwareDelayParams Record(uint32_t line) const;

    uint16_t channels = 2;
    uint32_t frames = 1;
    uint16_t feedback = 0;
    uint16_t damping_a = 0;
    uint16_t damping_b = 0;
};

// The firmware's reverb as nn::snd's DSP reverb sets it up (StreetPass Mii Plaza's update v14.6.0, code.bin 0x2eeda8),
// in the fields of the firmware's parameters (ntdlib's dsp_abi.h, FirmwareReverbParams).
struct FirmwareReverb
{
    // Sets the reverb up from `param`. Throws std::invalid_argument for a coloration, a damping or a gain that nn::snd
    // refuses. The constructor also throws for surround and for a filter that isn't a whole number of frames. The DSP
    // reverb has no surround.
    explicit FirmwareReverb(const ntdlib::ctr_fx::ReverbParam& param);

    // The bytes of the reverb's five lines in FCRAM, one after another: each line's frames × 2 channels × 160 32-bit
    // samples.
    uint64_t BufferBytes() const;

    // Where each line starts in the buffer, in bytes: the early reflections, the pre-delay, the two comb filters and
    // the all-pass filter.
    std::array<uint32_t, 5> LineOffsets() const;

    // The bus's record in the firmware's parameters for a buffer at physical address `buffer`. nn::snd leaves the same
    // record once it has sent the parameters and the enable.
    ntdlib::ctr::nnsnd::FirmwareReverbParams Record(uint32_t buffer) const;

    uint16_t channels = 2;
    std::array<uint16_t, 5> frames{}; // the lines' lengths in frames of 160 samples, in the order above
    uint16_t early_gain = 0;          // × 128
    uint16_t fused_gain = 0;          // × 128
    uint16_t all_pass_gain = 0;       // the coloration × 128
    std::array<int16_t, 2> comb_gains{};
    int16_t damping_a = 0;
    int16_t damping_b = 0;
};

// The models of the effects on one bus. A copy holds the models' whole state.
class AuxEffects
{
public:
    AuxEffects() = default;

    // Sets the models of `effects` up, with their buffers in silence. Throws std::invalid_argument for a kind that
    // isn't known and for parameters that the libraries refuse.
    explicit AuxEffects(std::span<const AuxEffect> effects);

    // Whether the bus has an effect.
    bool Any() const;

    // The firmware's delay on the bus, if it has one.
    const std::optional<FirmwareDelay>& FirmwareDelayOnBus() const
    {
        return firmware_delay_;
    }

    // The firmware's reverb on the bus, if it has one.
    const std::optional<FirmwareReverb>& FirmwareReverbOnBus() const
    {
        return firmware_reverb_;
    }

    // Runs the effects over a frame of `channels`, of `samples` samples each, in place.
    void Process(const ntdlib::ctr_fx::Channels& channels, std::size_t samples);

private:
    using Model = std::variant<ntdlib::ctr_fx::Reverb, ntdlib::ctr_fx::Delay>;

    std::vector<Model> models_;
    std::optional<FirmwareDelay> firmware_delay_;
    std::optional<FirmwareReverb> firmware_reverb_;
};

} // namespace threesf::nnsnd
