// SPDX-License-Identifier: MIT

#include "nwsnd/util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "nwsnd/tables.h"

namespace threesf::nwsnd::util
{

float CalcPitchRatio(int pitch)
{
    int octave = 0;
    if (pitch < 0)
    {
        const int d = (3071 - pitch) / 3072;
        octave = -d;
        pitch += d * 3072;
    }

    if (pitch >= 3072)
    {
        const int d = pitch / 3072;
        octave += d;
        pitch -= d * 3072;
    }

    const int semi = pitch / 256;
    const int fine = pitch - semi * 256;
    float ratio = 1.0f;
    if (octave > 0)
    {
        if (octave & 1)
        {
            ratio = 2.0f;
        }
        for (int k = octave >> 1; k != 0; k--)
        {
            ratio = ratio * 2.0f * 2.0f;
        }
    }
    else if (octave < 0)
    {
        if ((-octave) & 1)
        {
            ratio = ratio * 0.5f;
        }
        for (int k = (-octave) >> 1; k != 0; k--)
        {
            ratio = ratio * 0.5f * 0.5f;
        }
    }

    if (semi != 0)
    {
        ratio = tables::kSemitone[semi] * ratio;
    }

    if (fine != 0)
    {
        ratio = tables::kFine[fine] * ratio;
    }

    return ratio;
}

float CalcVolumeRatio(float decibel)
{
    if (decibel > 6.0f)
    {
        decibel = 6.0f;
    }
    else if (decibel < -90.4f)
    {
        decibel = -90.4f;
    }

    const int idx = static_cast<int>(decibel * 10.0f) + 904;
    return tables::kDecibel[std::clamp(idx, 0, 964)];
}

int16_t DecibelSquare(int value)
{
    return tables::kDecibelSquare[std::clamp(value, 0, 127)];
}

float AttackTable(int attack)
{
    return tables::kAttack[std::clamp(attack, 0, 127)];
}

float CalcPanRatio(float pan, const PanInfo& info, bool surround_output)
{
    if (pan > 1.0f)
    {
        pan = 1.0f;
    }
    else if (pan < -1.0f)
    {
        pan = -1.0f;
    }

    const float x = (pan + 1.0f) * 0.5f;
    const bool surround = surround_output && !info.front_bypass;
    const auto& table = surround ? tables::kSurroundPan[info.curve % 3] : tables::kStereoPan[info.curve % 3];
    const int idx = static_cast<int>(0.5f + x * 256.0f);
    float ratio = table[std::clamp(idx, 0, 256)];
    if (info.center_zero_db)
    {
        ratio = ratio / table[128];
    }

    const float max = info.clamp_to_unity ? 1.0f : 2.0f;
    if (ratio > max)
    {
        return max;
    }

    if (ratio < 0.0f)
    {
        return 0.0f;
    }

    return ratio;
}

float CalcSurroundPanRatio(float span, const PanInfo& info)
{
    if (span > 2.0f)
    {
        span = 2.0f;
    }
    else if (span < 0.0f)
    {
        span = 0.0f;
    }

    const int idx = static_cast<int>(0.5f + span * 0.5f * 256.0f);
    float ratio = tables::kStereoPan[info.curve % 3][std::clamp(idx, 0, 256)];
    return std::clamp(ratio, 0.0f, 2.0f);
}

const int16_t* BiquadPresetCoefficients(int type, float value)
{
    // The presets' GetCoef (code.bin 0x48e5d8, 0x48e584, 0x48ee94, 0x48f124 and 0x48f188) turn the value into a step in
    // float arithmetic, truncate it towards zero and clamp it to the table. The band-pass presets scale (2 - v) v
    // instead of v.
    const auto step = [](float x, int last)
    {
        return std::clamp(static_cast<int>(x), 0, last);
    };
    switch (type)
    {
    case 1:
        return tables::kBiquadLpf[step(value * 111.0f, 111)];
    case 2:
        return tables::kBiquadHpf[step(value * 96.0f, 96)];
    case 3:
        return tables::kBiquadBpf512[step((2.0f - value) * value * 121.0f, 121)];
    case 4:
        return tables::kBiquadBpf1024[step((2.0f - value) * value * 92.0f, 92)];
    case 5:
        return tables::kBiquadBpf2048[step((2.0f - value) * value * 92.0f, 92)];
    default:
        return nullptr;
    }
}

uint16_t CalcLpfFreq(float lpf)
{
    float v = 1.0f;
    if (!(lpf > 1.0f))
    {
        if (lpf < 0.13561438f)
        {
            return 80;
        }

        v = lpf;
    }

    if (v >= 0.9f)
    {
        return 16000;
    }

    const int idx = static_cast<int>((v - 0.13561438f) * 29.999998f);
    return tables::kLpfFreq[std::clamp(idx, 0, 23)];
}

void CalcMonoFilterCoefficients(uint16_t freq, int16_t& b0, int16_t& a1)
{
    // code.bin 0x17f210. The cosine is nn::math's CosFIdx (0x172d44), which takes an angle in 1/256 turns and
    // interpolates its table linearly, so w = 2 pi f / 32000. At 16000 Hz or less the angle stays under half a turn,
    // where CosFIdx's wrapping of larger and negative angles changes nothing.
    if (freq > 16000)
    {
        freq = 16000;
    }

    const float x = static_cast<float>(freq) * 0.008f;
    const uint32_t i = static_cast<uint32_t>(x);
    const float w = tables::kCos[i] + (x - static_cast<float>(i)) * tables::kCosDelta[i];
    const float b = 2.0f - w;
    const float c = std::sqrt(b * b - 1.0f) - b;
    b0 = static_cast<int16_t>(static_cast<int>((c + 1.0f) * 32768.0f));
    a1 = static_cast<int16_t>(-static_cast<int>(c * 32768.0f));
}

int8_t LfoSine(int i)
{
    return tables::kLfoSine[std::clamp(i, 0, 32)];
}

} // namespace threesf::nwsnd::util
