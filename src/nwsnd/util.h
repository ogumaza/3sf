// SPDX-License-Identifier: MIT

// nw::snd (NintendoWare for CTR) utility math, as linked into Pokemon X. Every function names the code.bin routine it
// models.

#pragma once

#include <cstdint>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"

namespace threesf::nwsnd::util
{

// Cutoff frequency for an nw::snd LPF value in [0, 1] (code.bin 0x17f6e4).
inline uint16_t CalcLpfFreq(float lpf)
{
    return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::CtrPolicy>(lpf);
}

// One-pole low-pass coefficients for a cutoff (code.bin 0x17f210).
void CalcMonoFilterCoefficients(uint16_t freq, int16_t& b0, int16_t& a1);

// Returns the coefficients (b0, b1, b2, a1, a2) that nw::snd's biquad preset `type` gives for a filter value in [0, 1],
// or nullptr for a type without a preset. nw::snd registers the presets as types 1 to 5 when it starts (code.bin
// 0x1372a8) and leaves the other types without one. The presets' GetCoef routines are at code.bin 0x48e5d8, 0x48e584,
// 0x48ee94, 0x48f124 and 0x48f188.
inline const int16_t* BiquadPresetCoefficients(int type, float value)
{
    return ntdlib::nwsnd::BiquadPresetCoefficients<ntdlib::nwsnd::CtrPolicy>(type, value);
}

} // namespace threesf::nwsnd::util
