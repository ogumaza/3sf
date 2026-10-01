// SPDX-License-Identifier: MIT

// nw::snd EnvGenerator and Lfo (CTR), modelled on code.bin 0x3196e8..0x31985c, 0x17f5d4, 0x181394 (envelope) and
// 0x17a1d0, 0x1811c8, 0x181434, 0x321064 (LFO).

#pragma once

#include <cstdint>

namespace threesf::nwsnd
{

class EnvGenerator
{
public:
    enum class Status : uint8_t
    {
        kAttack = 0,
        kHold = 1,
        kDecay = 2,
        kSustain = 3,
        kRelease = 4
    };

    // 0x3196e8
    void Initialize();

    // 0x3197a8: back to the attack, from -90.4 dB (as Channel::Start does).
    void Reset()
    {
        status_ = Status::kAttack;
        value_ = kInitDecibel * 10.0f;
    }

    void SetAttack(int attack); // 0x31985c
    void SetDecay(int decay);   // 0x3197e4

    void SetSustain(int sustain)
    {
        sustain_ = static_cast<uint8_t>(sustain);
    }

    void SetHold(int hold);       // 0x3197c4
    void SetRelease(int release); // 0x319730

    void SetStatus(Status s)
    {
        status_ = s;
    }

    Status GetStatus() const
    {
        return status_;
    }

    // Level in dB (0x181394).
    float GetValue() const;

    // Advances by msec milliseconds (0x17f5d4).
    void Update(int msec);

private:
    // The level an envelope starts from, in dB.
    static constexpr float kInitDecibel = -90.4f;

    static float CalcRelease(int release);

    Status status_ = Status::kAttack;
    float value_ = 0.0f; // 0.1 dB units
    float decay_ = 65535.0f;
    float release_ = 65535.0f;
    float attack_ = 0.0f;
    uint16_t hold_ = 0;
    uint16_t hold_counter_ = 0;
    uint8_t sustain_ = 127;
};

struct LfoParam
{
    void Init() // 0x17a1d0
    {
        *this = LfoParam{};
    }

    float depth = 0.0f;  // 0..1 (MML mod_depth / 128)
    float speed = 6.25f; // Hz (MML mod_speed * 100/256)
    uint32_t delay = 0;  // ms (MML mod_delay * 5)
    uint8_t range = 1;   // semitones for pitch LFO
};

class Lfo
{
public:
    void Reset() // 0x321064
    {
        delay_counter_ = 0;
        counter_ = 0.0f;
    }

    void Update(int msec);  // 0x1811c8
    float GetValue() const; // 0x181434

    LfoParam param_;

private:
    uint32_t delay_counter_ = 0;
    float counter_ = 0.0f;
};

} // namespace threesf::nwsnd
