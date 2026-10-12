// SPDX-License-Identifier: MIT

#include "nwsnd/util.h"

#include <cmath>
#include <cstdint>

#include "nwsnd/tables.h"

namespace threesf::nwsnd::util
{

void CalcMonoFilterCoefficients(uint16_t freq, int16_t& b0, int16_t& a1)
{
    // code.bin 0x17f210. The cosine is nn::math's CosFIdx (0x172d44). CosFIdx takes an angle in 1/256 turns and
    // interpolates the cosines in its table of sines and cosines linearly. w is therefore cos(2 pi f / 32000). At 16000
    // Hz or less the angle stays under half a turn. CosFIdx's wrapping of larger and negative angles then changes
    // nothing.
    if (freq > 16000)
    {
        freq = 16000;
    }

    const float x = static_cast<float>(freq) * 0.008f;
    const uint32_t i = static_cast<uint32_t>(x);
    const float w = tables::kSin[i][2] + (x - static_cast<float>(i)) * tables::kSin[i][3];
    const float b = 2.0f - w;
    const float c = std::sqrt(b * b - 1.0f) - b;
    b0 = static_cast<int16_t>(static_cast<int>((c + 1.0f) * 32768.0f));
    a1 = static_cast<int16_t>(-static_cast<int>(c * 32768.0f));
}

} // namespace threesf::nwsnd::util
