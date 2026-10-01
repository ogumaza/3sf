// SPDX-License-Identifier: MIT

#include "nwsnd/envelope.h"

#include <cstdint>

#include "nwsnd/util.h"

namespace threesf::nwsnd
{

void EnvGenerator::Initialize()
{
    attack_ = util::AttackTable(127);
    hold_ = 0;
    decay_ = 65535.0f;
    sustain_ = 127;
    release_ = 65535.0f;
    status_ = Status::kAttack;
    value_ = kInitDecibel * 10.0f;
}

void EnvGenerator::SetAttack(int attack)
{
    attack_ = util::AttackTable(attack);
}

float EnvGenerator::CalcRelease(int release)
{
    if (release == 127)
    {
        return 65535.0f;
    }
    if (release == 126)
    {
        return 24.0f;
    }
    if (release < 50)
    {
        return static_cast<float>(release * 2 + 1) * 0.0078125f * 0.2f;
    }

    return (60.0f / static_cast<float>(126 - release)) * 0.2f;
}

void EnvGenerator::SetDecay(int decay)
{
    decay_ = CalcRelease(decay);
}

void EnvGenerator::SetRelease(int release)
{
    release_ = CalcRelease(release);
}

void EnvGenerator::SetHold(int hold)
{
    const int sq = (hold + 1) * (hold + 1);
    hold_ = static_cast<uint16_t>(sq >> 2);
}

float EnvGenerator::GetValue() const
{
    if (status_ != Status::kAttack || attack_ != 0.0f)
    {
        return value_ * 0.1f;
    }

    return 0.0f;
}

void EnvGenerator::Update(int msec)
{
    switch (status_)
    {
    case Status::kAttack:
        {
            for (int i = 0; i < msec; i++)
            {
                value_ *= attack_;
                if (value_ > -0.03125f)
                {
                    value_ = 0.0f;
                    status_ = Status::kHold;
                    hold_counter_ = hold_;
                    return;
                }
            }
            return;
        }

    case Status::kHold:
        if (hold_counter_ > msec)
        {
            hold_counter_ = static_cast<uint16_t>(hold_counter_ - msec);
            return;
        }

        msec -= hold_counter_;
        hold_counter_ = 0;
        status_ = Status::kDecay;
        [[fallthrough]];

    case Status::kDecay:
        {
            value_ -= decay_ * static_cast<float>(msec);
            const float sustain_db = static_cast<float>(util::DecibelSquare(sustain_));
            if (value_ >= sustain_db)
            {
                return;
            }

            value_ = sustain_db;
            status_ = Status::kSustain;
            return;
        }

    case Status::kSustain:
        return;

    case Status::kRelease:
        value_ -= release_ * static_cast<float>(msec);
        return;
    }
}

void Lfo::Update(int msec)
{
    if (delay_counter_ < param_.delay)
    {
        if (delay_counter_ + static_cast<uint32_t>(msec) <= param_.delay)
        {
            delay_counter_ += static_cast<uint32_t>(msec);
            return;
        }

        msec -= static_cast<int>(param_.delay - delay_counter_);
        delay_counter_ = param_.delay;
    }

    const float c = counter_ + param_.speed * (static_cast<float>(msec) * 0.001f);
    counter_ = c - static_cast<float>(static_cast<int>(c));
}

float Lfo::GetValue() const
{
    if (param_.depth == 0.0f)
    {
        return 0.0f;
    }
    if (param_.delay > delay_counter_)
    {
        return 0.0f;
    }

    const int p = static_cast<int>(counter_ * 128.0f);
    int s;
    if (p < 32)
    {
        s = util::LfoSine(p);
    }
    else if (p < 64)
    {
        s = util::LfoSine(64 - p);
    }
    else if (p < 96)
    {
        s = -util::LfoSine(p - 64);
    }
    else
    {
        s = -util::LfoSine(128 - p);
    }

    const float v = static_cast<float>(s) * kOneOver127;
    return static_cast<float>(param_.range) * (param_.depth * v);
}

} // namespace threesf::nwsnd
