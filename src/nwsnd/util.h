// SPDX-License-Identifier: MIT

// nw::snd (NintendoWare for CTR) utility math, as linked into Pokemon X. Every function names the code.bin routine it
// models.

#pragma once

#include <bit>
#include <cstdint>

namespace threesf::nwsnd
{

// Float constants that more than one part of the model uses, exactly as they appear in code.bin's literal pools.
inline constexpr float kOneOver63 = std::bit_cast<float>(0x3c820821u);       // 1/63
inline constexpr float kOneOver127 = std::bit_cast<float>(0x3c010204u);      // 1/127 (code.bin 0x1814dc)
inline constexpr float kOneOver127Cubed = std::bit_cast<float>(0x35030c28u); // 1/127^3

// Pan curve selection passed to the pan-ratio functions (nw::snd internal PanInfo).
struct PanInfo
{
    uint8_t curve = 0;           // 0 = sqrt, 1 = sin/cos, 2 = linear
    bool center_zero_db = false; // normalize so the centre is 0 dB
    bool clamp_to_unity = false; // clamp to 1.0 instead of 2.0
    bool front_bypass = false;   // use the stereo curves even for surround output
};

namespace util
{

// Pitch ratio for a pitch in 1/256 semitones (code.bin 0x17f77c).
float CalcPitchRatio(int pitch);

// Linear amplitude for a level in dB, clamped to [-90.4, +6] (code.bin 0x17f8a8).
float CalcVolumeRatio(float decibel);

// Squared-volume level in 0.1 dB units for a 0..127 value (table at code.bin 0x576764).
int16_t DecibelSquare(int value);

// Envelope attack multipliers (table at code.bin 0x576864).
float AttackTable(int attack);

// Pan ratio for pan in [-1, 1] (code.bin 0x18b770).
float CalcPanRatio(float pan, const PanInfo& info, bool surround_output);

// Front/rear ratio for surround pan in [0, 2] (code.bin 0x18b878).
float CalcSurroundPanRatio(float span, const PanInfo& info);

// Cutoff frequency for an nw::snd LPF value in [0, 1] (code.bin 0x17f6e4).
uint16_t CalcLpfFreq(float lpf);

// One-pole low-pass coefficients for a cutoff (code.bin 0x17f210).
void CalcMonoFilterCoefficients(uint16_t freq, int16_t& b0, int16_t& a1);

// Returns the coefficients (b0, b1, b2, a1, a2) that nw::snd's biquad preset `type` gives for a filter value in [0, 1],
// or nullptr for a type without a preset. nw::snd registers the presets as types 1 to 5 when it starts (code.bin
// 0x1372a8) and leaves the other types without one.
const int16_t* BiquadPresetCoefficients(int type, float value);

// LFO quarter-sine table value (table at code.bin 0x576a64): round(127 sin(i pi / 64)).
int8_t LfoSine(int i);

} // namespace util

// Util::CalcRandom (code.bin 0x1741fc): x = x * 0x19660D + 0x3C6EF35F, returns x >> 16.
class Random
{
public:
    uint16_t Next()
    {
        x_ = x_ * 0x19660Du + 0x3C6EF35Fu;

        return static_cast<uint16_t>(x_ >> 16);
    }

private:
    uint32_t x_ = 0x12345678;
};

} // namespace threesf::nwsnd
