// SPDX-License-Identifier: MIT

// Exact lookup tables from nw::snd and nn::math. Most contain standard functions (powers of two, decibels, sines and
// cosines), but recomputing them with the C library changes some entries and varies across platforms. Keeping the
// original values preserves the game's rounding.
//
// Most of the tables hold the same values in every NintendoWare sound library. ntdlib holds those
// (ntdlib/nwsnd/tables.h). Pokemon X's code.bin keeps them at these addresses: kSemitone at 0x56e388, kFine at
// 0x56e3b8, kDecibel at 0x56e7b8, kDecibelSquare at 0x576764, kAttack at 0x576864, the pan curves from 0x56f6cc,
// kLpfFreq at 0x56e358, kLfoSine at 0x576a64, the biquad presets from 0x574e7c, and nn::math's sines and cosines (kSin)
// at 0x560ac0.

#pragma once

#include "ntdlib/nwsnd/tables.h"

namespace threesf::nwsnd::tables
{

using ntdlib::nwsnd::tables::kSin;

} // namespace threesf::nwsnd::tables
