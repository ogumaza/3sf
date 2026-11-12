// SPDX-License-Identifier: MIT

#include "nnsnd/aux_effects.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "ntdlib/ctr/nnsnd/dsp_abi.h"
#include "ntdlib/ctr_fx/effects.h"
#include "ntdlib/ctr_fx/vfp.h"

namespace threesf::nnsnd
{

using ntdlib::ctr::nnsnd::FirmwareDelayParams;
using ntdlib::ctr::nnsnd::FirmwareReverbParams;

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
    case AuxEffect::Kind::kNwSndReverb2011:
        return Reverb(Library::kNwSnd2011, effect.reverb);
    case AuxEffect::Kind::kDelay:
        return Delay(effect.delay);
    case AuxEffect::Kind::kFirmwareDelay:
    case AuxEffect::Kind::kFirmwareReverb:
        break;
    }

    throw std::invalid_argument("an effect of kind " + std::to_string(static_cast<uint32_t>(effect.kind)));
}

// Whether nn::snd's DSP delay takes `value` as a feedback or a damping (0x22d8fc). It refuses a value below 0, and one
// whose bits, read as a signed integer, exceed those of 1.
bool InUnitRange(float value)
{
    return !(value < 0.0f) && std::bit_cast<int32_t>(value) <= std::bit_cast<int32_t>(1.0f);
}

// The frames of a reverb's three filters: each filter's whole frames.
uint64_t FilterFrames(const ntdlib::ctr_fx::ReverbParam& param)
{
    uint64_t frames = 0;
    for (const uint32_t size : param.filter_sizes)
    {
        frames += size / kAuxChannelSamples;
    }

    return frames;
}

// The damping that nn::snd's DSP delay and reverb take: `damping` at most 0.95, by the bits of the floats.
float ClampDamping(float damping)
{
    return std::bit_cast<int32_t>(damping) > std::bit_cast<int32_t>(0.95f) ? 0.95f : damping;
}

} // namespace

FirmwareDelay::FirmwareDelay(const ntdlib::ctr_fx::DelayParam& param)
{
    namespace vfp = ntdlib::ctr_fx::vfp;

    if (!InUnitRange(param.damping) || !InUnitRange(param.feedback))
    {
        throw std::invalid_argument("the firmware's delay takes a feedback and a damping from 0 to 1");
    }

    // 0x22d924 to 0x22d9e0. The damping is at most 0.95, by the bits of the floats.
    channels = param.enable_surround ? 4 : 2;
    frames = ntdlib::ctr_fx::DelayFrames(param.delay_time);
    feedback = static_cast<uint16_t>(vfp::ToU32(vfp::Mul(param.feedback, 128.0f)));
    const float damping = ClampDamping(param.damping);
    damping_a = static_cast<uint16_t>(vfp::ToS32(vfp::Mul(vfp::Sub(1.0f, damping), 128.0f)));
    damping_b = static_cast<uint16_t>(vfp::ToS32(vfp::Mul(damping, 128.0f)));
}

FirmwareDelayParams FirmwareDelay::Record(uint32_t line) const
{
    FirmwareDelayParams p{};
    p.buffer = line;
    p.channels = channels;
    p.frames = static_cast<uint16_t>(frames);
    p.feedback = feedback;
    p.damping_a = damping_a;
    p.damping_b = damping_b;
    p.enable = 1;
    p.dirty = 7;

    return p;
}

uint64_t FirmwareDelay::LineBytes() const
{
    return uint64_t{frames} * channels * kAuxChannelSamples * sizeof(int32_t);
}

FirmwareReverb::FirmwareReverb(const ntdlib::ctr_fx::ReverbParam& param)
{
    namespace vfp = ntdlib::ctr_fx::vfp;

    // 0x2eede0 to 0x2eee50 test that each of the four ratios lies in [0, 1]. The delay tests its ratios likewise.
    if (!InUnitRange(param.coloration) || !InUnitRange(param.damping) || !InUnitRange(param.early_gain) ||
        !InUnitRange(param.fused_gain))
    {
        throw std::invalid_argument("the firmware's reverb takes a coloration, a damping and gains from 0 to 1");
    }

    // The parameters of nn::snd's DSP reverb have no surround. The reverb has two channels (0x2eeeb0).
    if (param.enable_surround)
    {
        throw std::invalid_argument("the firmware's reverb has no surround");
    }

    // nn::snd counts a filter's whole frames and takes a filter of none. What the firmware then does isn't known. The
    // player refuses such a filter. ntdlib's models refuse one too.
    for (const uint32_t size : param.filter_sizes)
    {
        if (size == 0 || size % kAuxChannelSamples != 0)
        {
            throw std::invalid_argument("the firmware's reverb takes filters of whole frames, one at least");
        }
    }

    // 0x2eee64 to 0x2eef14. The early reflections and the pre-delay take whole frames as the delay does, and each
    // filter's size counts its whole frames.
    frames = {static_cast<uint16_t>(ntdlib::ctr_fx::DelayFrames(param.early_reflection_time)),
              static_cast<uint16_t>(ntdlib::ctr_fx::DelayFrames(param.pre_delay_time)),
              static_cast<uint16_t>(param.filter_sizes[0] / kAuxChannelSamples),
              static_cast<uint16_t>(param.filter_sizes[1] / kAuxChannelSamples),
              static_cast<uint16_t>(param.filter_sizes[2] / kAuxChannelSamples)};

    // 0x2eef18 to 0x2eef58: each comb filter's gain falls by 60 dB over the fused time, 10^(-3·length/(time·rate)),
    // with the firmware's rate of 32,728 Hz. A fused time of 0 gives a gain of 0.
    const float fused = vfp::Mul(vfp::Mul(vfp::FromU32(param.fused_time), 0.001f), 32728.0f);
    for (std::size_t i = 0; i < comb_gains.size(); i++)
    {
        const int32_t length = static_cast<int16_t>(frames[2 + i]) * static_cast<int32_t>(kAuxChannelSamples);
        const float exponent = vfp::Div(vfp::Mul(vfp::FromS32(length), -3.0f), fused);
        comb_gains[i] = static_cast<int16_t>(vfp::ToS32(vfp::Mul(vfp::PowF(10.0f, exponent), 128.0f)));
    }

    // 0x2eef5c to 0x2eeffc and 0x2ef198. The damping is at most 0.95, by the bits of the floats. The high-pass damping
    // negates both coefficients.
    early_gain = static_cast<uint16_t>(vfp::ToU32(vfp::Mul(param.early_gain, 128.0f)));
    fused_gain = static_cast<uint16_t>(vfp::ToU32(vfp::Mul(param.fused_gain, 128.0f)));
    all_pass_gain = static_cast<uint16_t>(vfp::ToU32(vfp::Mul(param.coloration, 128.0f)));
    const float damping = ClampDamping(param.damping);
    const float a = param.high_pass_damping ? vfp::Sub(damping, 1.0f) : vfp::Sub(1.0f, damping);
    const float b = param.high_pass_damping ? -damping : damping;
    damping_a = static_cast<int16_t>(vfp::ToS32(vfp::Mul(a, 128.0f)));
    damping_b = static_cast<int16_t>(vfp::ToS32(vfp::Mul(b, 128.0f)));
}

std::array<uint32_t, 5> FirmwareReverb::LineOffsets() const
{
    // 0x2ef000 to 0x2ef11c: the lines lie one after another, each of frames × channels × 160 32-bit samples.
    std::array<uint32_t, 5> offsets{};
    uint64_t offset = 0;
    for (std::size_t i = 0; i < frames.size(); i++)
    {
        offsets[i] = static_cast<uint32_t>(offset);
        offset += uint64_t{frames[i]} * channels * kAuxChannelSamples * sizeof(int32_t);
    }

    return offsets;
}

FirmwareReverbParams FirmwareReverb::Record(uint32_t buffer) const
{
    FirmwareReverbParams p{};
    const std::array<uint32_t, 5> offsets = LineOffsets();
    for (std::size_t i = 0; i < offsets.size(); i++)
    {
        p.lines[i] = buffer + offsets[i];
        p.frames[i] = frames[i];
    }

    p.channels = channels;
    p.early_gain = early_gain;
    p.fused_gain = fused_gain;
    p.all_pass_gain = all_pass_gain;
    p.comb_gains[0] = comb_gains[0];
    p.comb_gains[1] = comb_gains[1];
    p.damping_a = damping_a;
    p.damping_b = damping_b;
    p.enable = 1;
    p.dirty = 7;

    return p;
}

uint64_t FirmwareReverb::BufferBytes() const
{
    uint64_t frame_count = 0;
    for (const uint16_t f : frames)
    {
        frame_count += f;
    }

    return frame_count * channels * kAuxChannelSamples * sizeof(int32_t);
}

bool AuxEffect::Known(uint32_t kind)
{
    return kind >= static_cast<uint32_t>(Kind::kNwSndReverb) && kind <= static_cast<uint32_t>(Kind::kFirmwareReverb);
}

bool AuxEffect::IsReverb() const
{
    return kind == Kind::kNwSndReverb || kind == Kind::kNnSndReverb || kind == Kind::kNnSndReverb2011 ||
           kind == Kind::kNwSndReverb2011 || kind == Kind::kFirmwareReverb;
}

bool AuxEffect::InFirmware() const
{
    return kind == Kind::kFirmwareDelay || kind == Kind::kFirmwareReverb;
}

uint64_t AuxEffect::BufferSamples() const
{
    // The models lay each channel's buffers out in whole frames (ntdlib/ctr_fx/effects.h). So do the firmware's delay
    // and reverb, in FCRAM. The firmware's reverb counts its frames as the delay does.
    if (kind == Kind::kFirmwareReverb)
    {
        const uint64_t frames = uint64_t{ntdlib::ctr_fx::DelayFrames(reverb.early_reflection_time)} +
                                ntdlib::ctr_fx::DelayFrames(reverb.pre_delay_time) + FilterFrames(reverb);
        return frames * kAuxChannelSamples * 2;
    }

    if (!IsReverb())
    {
        const uint64_t channels = delay.enable_surround ? 4 : 2;
        return uint64_t{ntdlib::ctr_fx::DelayFrames(delay.delay_time)} * kAuxChannelSamples * channels;
    }

    const uint64_t frames = uint64_t{ntdlib::ctr_fx::ReverbFrames(reverb.early_reflection_time)} +
                            ntdlib::ctr_fx::ReverbFrames(reverb.pre_delay_time) + FilterFrames(reverb);
    const uint64_t channels = reverb.enable_surround ? 4 : 2;
    return frames * kAuxChannelSamples * channels;
}

bool HasEffects(const AuxEffectLists& effects)
{
    return !effects[0].empty() || !effects[1].empty();
}

AuxEffects::AuxEffects(std::span<const AuxEffect> effects)
{
    for (const AuxEffect& effect : effects)
    {
        if (!effect.InFirmware())
        {
            models_.push_back(MakeModel(effect));
            continue;
        }

        if (effect.kind == AuxEffect::Kind::kFirmwareReverb)
        {
            if (firmware_reverb_)
            {
                throw std::invalid_argument("a bus with two of the firmware's reverbs");
            }

            firmware_reverb_.emplace(effect.reverb);
            continue;
        }

        if (firmware_delay_)
        {
            throw std::invalid_argument("a bus with two of the firmware's delays");
        }

        firmware_delay_.emplace(effect.delay);
    }
}

bool AuxEffects::Any() const
{
    return !models_.empty() || firmware_delay_ || firmware_reverb_;
}

void AuxEffects::Process(const ntdlib::ctr_fx::Channels& channels, std::size_t samples)
{
    for (Model& model : models_)
    {
        std::visit([&channels, samples](auto& effect) { effect.Process(channels, samples); }, model);
    }
}

} // namespace threesf::nnsnd
