// SPDX-License-Identifier: MIT

#include "nwsnd/sequence.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <span>

#include "nwsnd/engine.h"
#include "nwsnd/util.h"

namespace threesf::nwsnd
{
namespace
{

// Float constants exactly as they appear in code.bin's literal pools (nwsnd/util.h has the ones other files use too).
const float kOneOver60000 = std::bit_cast<float>(0x378bcf65u);    // 1/60000 (ms per minute)
const float kArmClock = std::bit_cast<float>(0x4d7fb0ffu);        // 268111856
const float kOneOverArmClock = std::bit_cast<float>(0x3180278du); // 1/268111856
constexpr uint64_t kFrameMilliCycles = 0x4e200000;                // one sound frame (160 samples) in ARM cycles x 1000
constexpr int kParseLimit = 10000;                                // commands per tick (0x31dd08)

} // namespace

// --------------------------------------------------------------------------------------------- SequenceTrack
SequenceTrack::SequenceTrack(SequenceSoundPlayer& player, int index) : player_(player), index_(index)
{
    InitParam();
}

SequenceTrack::~SequenceTrack()
{
    // Detach any channels still pointing at this track.
    for (Channel* ch = channel_list_; ch; ch = ch->next_in_track_)
    {
        ch->callback_ = nullptr;
    }
}

void SequenceTrack::InitParam()
{
    // 0x31e698
    data_ = {};
    current_ = 0;
    has_data_ = false;
    cmp_flag_ = true;
    note_wait_ = true;
    tie_ = false;
    mono_ = false;
    call_depth_ = 0;
    wait_ = 0;
    mute_ = false;
    note_finish_wait_ = false;
    porta_ = false;
    damper_ = false;
    bank_index_ = 0;
    prg_no_ = 0;
    lfo_.Init();
    lfo_type_ = 0;
    sweep_pitch_ = 0.0f;
    vol_.Init(127);
    pan_move_.Init(0);
    span_move_.Init(0);
    volume2_ = 127;
    velocity_range_ = 127;
    bend_.Init(0);
    bend_range_ = 2;
    init_pan_ = 0;
    transpose_ = 0;
    priority_ = 64;
    porta_key_ = 60;
    porta_time_ = 0;
    attack_ = decay_ = sustain_ = release_ = 0xff;
    hold_ = 0xff;
    main_send_ = 127;
    fx_send_ = {0, 0};
    lpf_ = 0.0f;
    biquad_type_ = 0;
    biquad_value_ = 0.0f;
    vars_.fill(-1);
}

void SequenceTrack::SetSeqData(std::span<const uint8_t> data, uint32_t offset)
{
    // 0x186d80
    data_ = data;
    current_ = offset;
    has_data_ = true;
}

void SequenceTrack::Open()
{
    // 0x186d90
    note_finish_wait_ = false;
    call_depth_ = 0;
    wait_ = 0;
    open_ = true;
}

void SequenceTrack::Close()
{
    // 0x31e240
    UpdateChannelParam();

    for (Channel* ch = channel_list_; ch; ch = ch->next_in_track_)
    {
        if (ch->active_)
        {
            ch->Release();
        }
    }

    FreeAllChannel();
    open_ = false;
}

void SequenceTrack::ReleaseAllChannel()
{
    // 0x31ddc4
    UpdateChannelParam();

    for (Channel* ch = channel_list_; ch; ch = ch->next_in_track_)
    {
        if (!ch->active_)
        {
            continue;
        }

        ch->Release();
    }
}

void SequenceTrack::FreeAllChannel()
{
    // 0x31dd0c: detach every channel (Channel 0x320830) and forget them.
    for (Channel* ch = channel_list_; ch; ch = ch->next_in_track_)
    {
        ch->callback_ = nullptr;
    }

    channel_list_ = nullptr;
}

void SequenceTrack::Mute(int mode)
{
    // 0x31e5cc
    switch (mode)
    {
    case 0:
        mute_ = false;
        break;

    case 1:
        mute_ = true;
        break;

    case 2:
        UpdateChannelParam();

        for (Channel* ch = channel_list_; ch; ch = ch->next_in_track_)
        {
            if (ch->active_)
            {
                ch->Release();
            }
        }

        FreeAllChannel();
        mute_ = true;
        break;

    case 3:
        for (Channel* ch = channel_list_; ch;)
        {
            Channel* next = ch->next_in_track_;
            ch->callback_ = nullptr;
            ch->Stop();
            ch = next;
        }

        channel_list_ = nullptr;
        mute_ = true;
        break;

    default:
        break;
    }
}

void SequenceTrack::UpdateChannelLength()
{
    // 0x31e1c4
    if (!open_)
    {
        return;
    }

    for (Channel* ch = channel_list_; ch;)
    {
        Channel* next = ch->next_in_track_;

        if (ch->length_ > 0)
        {
            ch->length_--;
        }

        if (ch->length_ == 0 && ch->env_.GetStatus() != EnvGenerator::Status::kRelease && !damper_)
        {
            ch->NoteOff();
        }

        if (!ch->auto_sweep_)
        {
            ch->UpdateSweep(1);
        }

        ch = next;
    }
}

int SequenceTrack::ParseNextTick(bool do_note_on)
{
    // 0x31dbf4
    if (!open_)
    {
        return 0;
    }

    vol_.Update();
    pan_move_.Update();
    span_move_.Update();
    bend_.Update();

    if (note_finish_wait_)
    {
        if (channel_list_)
        {
            return 1;
        }

        note_finish_wait_ = false;
    }

    if (wait_ > 0)
    {
        wait_--;
        if (wait_ > 0)
        {
            return 1;
        }
    }

    if (!has_data_)
    {
        return 1;
    }

    int count = 0;
    while (wait_ == 0 && !note_finish_wait_)
    {
        if (++count > kParseLimit)
        {
            break;
        }

        if (Parse(do_note_on) == 1)
        {
            return -1;
        }
    }

    return 1;
}

uint8_t SequenceTrack::ReadByte()
{
    if (current_ >= data_.size())
    {
        // Out of the sequence data: behave like FIN.
        current_ = static_cast<uint32_t>(data_.size()) + 1;
        return 0xff;
    }

    return data_[current_++];
}

int32_t SequenceTrack::ReadArg(int type)
{
    // 0x4908d4
    switch (type)
    {
    case kArgU8:
        return ReadByte();

    case kArgS16:
        {
            const uint32_t hi = ReadByte();
            const uint32_t lo = ReadByte();
            return static_cast<int32_t>(((hi << 8) & 0xffff) | lo);
        }

    case kArgVmidi:
        {
            // Past the end of the data ReadByte keeps returning 0xff, so the value stops there too.
            uint32_t v = 0;
            uint8_t b;
            do
            {
                b = ReadByte();
                v = (b & 0x7f) | (v << 7);
            } while ((b & 0x80) && current_ <= data_.size());
            return static_cast<int32_t>(v);
        }

    case kArgRandom:
        {
            const uint32_t h1 = ReadByte();
            const uint32_t l1 = ReadByte();
            const int32_t lo = static_cast<int16_t>(((h1 << 8) & 0xffff) | l1);
            const uint32_t h2 = ReadByte();
            const uint32_t l2 = ReadByte();
            const int32_t hi = static_cast<int16_t>(((h2 << 8) & 0xffff) | l2);
            const uint32_t r = player_.GetEngine().Rng().Next();

            // 32-bit wrapping multiply, arithmetic shift (as the ARM code does).
            const int32_t prod = static_cast<int32_t>(static_cast<uint32_t>((hi - lo) + 1) * r);
            return lo + (prod >> 16);
        }

    case kArgVariable:
        {
            const uint8_t idx = ReadByte();
            int16_t* var = nullptr;
            if (idx < 0x20)
            {
                var = player_.Variable(idx);
            }
            else if (idx < 0x30)
            {
                var = Variable(idx - 0x20);
            }
            else
            {
                return 0;
            }

            return var ? *var : 0;
        }

    default:
        return 0;
    }
}

int SequenceTrack::Parse(bool do_note_on)
{
    // 0x49036c (MmlParser::Parse). Returns 1 on FIN.
    int arg_type = kArgNone;
    int time_type = kArgNone;
    bool cond = true;
    uint32_t cmd = ReadByte();
    if (cmd == 0xa2) // if
    {
        cmd = ReadByte();
        cond = cmp_flag_;
    }

    if (cmd == 0xa3) // time
    {
        time_type = kArgS16;
        cmd = ReadByte();
    }
    else if (cmd == 0xa4) // time random
    {
        time_type = kArgRandom;
        cmd = ReadByte();
    }
    else if (cmd == 0xa5) // time variable
    {
        time_type = kArgVariable;
        cmd = ReadByte();
    }

    if (cmd == 0xa0) // random
    {
        cmd = ReadByte();
        arg_type = kArgRandom;
    }
    else if (cmd == 0xa1) // variable
    {
        cmd = ReadByte();
        arg_type = kArgVariable;
    }

    // A random or variable prefix gives the argument's type; without one, each command reads the type it takes.
    const auto read_arg = [&](int type)
    {
        return ReadArg(arg_type != kArgNone ? arg_type : type);
    };

    if ((cmd & 0x80) == 0)
    {
        // A note: the command byte is the key, and the velocity and length follow.
        const uint8_t velocity = ReadByte();
        const int32_t length = read_arg(kArgVmidi);

        if (!cond)
        {
            return 0;
        }

        const int key = std::clamp(static_cast<int>(transpose_) + static_cast<int>(cmd), 0, 127);
        if (!mute_ && do_note_on)
        {
            NoteOn(key, velocity, length > 0 ? length : -1, tie_);
        }

        if (note_wait_)
        {
            wait_ = length;
            if (length == 0)
            {
                note_finish_wait_ = true;
            }
        }

        return 0;
    }

    switch ((cmd & 0xf0) >> 4)
    {
    case 0x8:
        switch (cmd)
        {
        case 0x80: // wait
            {
                const int32_t w = read_arg(kArgVmidi);
                if (cond)
                {
                    wait_ = w;
                }
                return 0;
            }

        case 0x81: // prg
            {
                const int32_t prg = read_arg(kArgVmidi);
                if (cond)
                {
                    CommandProc(cmd, prg, 0);
                }
                return 0;
            }

        case 0x88: // opentrack
            {
                const uint32_t track_no = ReadByte();
                uint32_t off = ReadByte();
                off = (off << 8) | ReadByte();
                off = (off << 8) | ReadByte();
                if (cond)
                {
                    CommandProc(cmd, static_cast<int32_t>(track_no), static_cast<int32_t>(off));
                }
                return 0;
            }

        case 0x89: // jump
        case 0x8a: // call
            {
                uint32_t off = ReadByte();
                off = (off << 8) | ReadByte();
                off = (off << 8) | ReadByte();
                if (cond)
                {
                    CommandProc(cmd, static_cast<int32_t>(off), 0);
                }
                return 0;
            }

        default:
            return 0;
        }

    case 0x9:
        if (cond)
        {
            CommandProc(cmd, 0, 0);
        }

        return 0;

    case 0xa:
        return 0;

    case 0xb:
    case 0xc:
    case 0xd:
        {
            const uint32_t v = static_cast<uint32_t>(read_arg(kArgU8)) & 0xff;
            int32_t time = 0;
            if (time_type != kArgNone)
            {
                time = ReadArg(time_type);
            }

            if (!cond)
            {
                return 0;
            }

            const int32_t a1 = (cmd == 0xc3 || cmd == 0xc4) ? static_cast<int8_t>(v) : static_cast<int32_t>(v);
            CommandProc(cmd, a1, time);
            return 0;
        }

    case 0xe:
        {
            const int32_t v = static_cast<int16_t>(read_arg(kArgS16));
            if (cond)
            {
                CommandProc(cmd, v, 0);
            }
            return 0;
        }

    case 0xf:
        if (cmd == 0xf0)
        {
            const uint32_t sub = ReadByte();
            const uint32_t kind = sub & 0xf0;
            if (kind == 0x80 || kind == 0x90)
            {
                const uint32_t var_no = ReadByte();
                const int32_t v = static_cast<int16_t>(read_arg(kArgS16));
                if (cond)
                {
                    CommandProc(0xf000 + sub, static_cast<int32_t>(var_no), v);
                }
                return 0;
            }

            if (kind == 0xe0)
            {
                const int32_t v = static_cast<uint16_t>(read_arg(kArgS16));
                if (cond)
                {
                    CommandProc(0xf000 + sub, v, 0);
                }
                return 0;
            }

            return 0;
        }

        if (cmd == 0xfe) // alloctrack (handled at setup)
        {
            ReadByte();
            ReadByte();
            return 0;
        }

        if (cmd == 0xff) // fin
        {
            return cond ? 1 : 0;
        }

        if (cond)
        {
            CommandProc(cmd, 0, 0);
        }

        return 0;

    default:
        return 0;
    }
}

void SequenceTrack::CommandProc(uint32_t cmd, int32_t arg1, int32_t arg2)
{
    // 0x48fad4 (MmlParser::CommandProc)
    if (cmd > 0xff)
    {
        const uint32_t op = cmd & 0xff;
        int16_t* var = nullptr;
        if ((op & 0xf0) == 0x80 || (op & 0xf0) == 0x90)
        {
            if (arg1 < 0x20)
            {
                var = player_.Variable(arg1);
            }
            else if (arg1 < 0x30)
            {
                var = Variable(arg1 - 0x20);
            }
            if (!var)
            {
                return;
            }
        }

        const int32_t v = arg2;
        const int16_t v16 = static_cast<int16_t>(v);
        switch (op)
        {
        case 0x80: // setvar
            *var = v16;
            break;

        case 0x81: // addvar
            *var = static_cast<int16_t>(*var + v16);
            break;

        case 0x82: // subvar
            *var = static_cast<int16_t>(static_cast<uint16_t>(*var) - v16);
            break;

        case 0x83: // mulvar
            *var = static_cast<int16_t>(static_cast<uint16_t>(*var) * v16);
            break;

        case 0x84: // divvar
            if (v != 0)
            {
                *var = static_cast<int16_t>(static_cast<int32_t>(*var) / v16);
            }
            break;

        case 0x85: // shiftvar: ARM register shifts use the low byte of the amount
            {
                if (v >= 0)
                {
                    const uint32_t n = static_cast<uint32_t>(v) & 0xff;
                    *var = static_cast<int16_t>(n >= 32 ? 0u : static_cast<uint32_t>(static_cast<uint16_t>(*var)) << n);
                }
                else
                {
                    const uint32_t n = static_cast<uint32_t>(-v) & 0xff;
                    const int32_t x = *var;
                    *var = static_cast<int16_t>(n >= 32 ? (x < 0 ? -1 : 0) : x >> n);
                }
                break;
            }

        case 0x86: // randvar
            {
                int32_t range = v;
                const bool neg = range < 0;
                if (neg)
                {
                    range = static_cast<int16_t>(-v);
                }

                int32_t r = static_cast<int32_t>(static_cast<uint32_t>(player_.GetEngine().Rng().Next()) *
                                                 static_cast<uint32_t>(range + 1)) >>
                            16;
                if (neg)
                {
                    r = -r;
                }

                *var = static_cast<int16_t>(r);
                break;
            }

        case 0x87: // andvar
            *var = static_cast<int16_t>(static_cast<uint16_t>(*var) & v);
            break;

        case 0x88: // orvar
            *var = static_cast<int16_t>(static_cast<uint16_t>(*var) | v);
            break;

        case 0x89: // xorvar
            *var = static_cast<int16_t>(static_cast<uint16_t>(*var) ^ v);
            break;

        case 0x8a: // notvar
            *var = static_cast<int16_t>(~v);
            break;

        case 0x8b: // modvar
            if (v != 0)
            {
                *var = static_cast<int16_t>(static_cast<int32_t>(*var) % v);
            }
            break;

        case 0x90: // cmp_eq
            cmp_flag_ = *var == v;
            break;

        case 0x91: // cmp_ge
            cmp_flag_ = *var >= v;
            break;

        case 0x92: // cmp_gt
            cmp_flag_ = *var > v;
            break;

        case 0x93: // cmp_le
            cmp_flag_ = *var <= v;
            break;

        case 0x94: // cmp_lt
            cmp_flag_ = *var < v;
            break;

        case 0x95: // cmp_ne
            cmp_flag_ = *var != v;
            break;

        case 0xe0: // userproc: 3SF registers no user procedure
        default:
            break;
        }

        return;
    }

    const uint8_t a8 = static_cast<uint8_t>(arg1);
    switch (cmd)
    {
    case 0x81: // prg
        if (arg1 < 0x10000)
        {
            prg_no_ = static_cast<uint16_t>(arg1);
        }
        break;

    case 0x88: // opentrack
        {
            SequenceTrack* target = player_.GetTrack(arg1);
            if (!target || target == this)
            {
                break;
            }

            target->Close();
            target->SetSeqData(data_, static_cast<uint32_t>(arg2));
            target->Open();
            break;
        }

    case 0x89: // jump
        if (static_cast<uint32_t>(arg1) < current_ && index_ == 0 && player_.on_loop_)
        {
            player_.on_loop_();
        }

        current_ = static_cast<uint32_t>(arg1);
        break;

    case 0x8a: // call
        if (call_depth_ >= 3)
        {
            break;
        }

        call_stack_[call_depth_].address = current_;
        call_stack_[call_depth_].is_loop = false;
        call_depth_++;
        current_ = static_cast<uint32_t>(arg1);
        break;

    case 0xb0:
        player_.timebase_ = a8;
        break;

    case 0xb1:
        hold_ = a8;
        break;

    case 0xb2: // monophonic
        mono_ = arg1 != 0;

        if (arg1 != 0)
        {
            ReleaseAllChannel();
            FreeAllChannel();
        }
        break;

    case 0xb3:
        velocity_range_ = a8;
        break;

    case 0xb4:
        biquad_type_ = a8;
        break;

    case 0xb5:
        biquad_value_ = static_cast<float>(arg1) * kOneOver127;
        break;

    case 0xb6:
        bank_index_ = a8;
        break;

    case 0xbf: // front bypass: each note passes it on to its voice
        front_bypass_ = arg1 != 0;
        break;

    case 0xc0:
        pan_move_.SetTarget(static_cast<int8_t>(arg1 - 0x40), static_cast<int16_t>(arg2));
        break;

    case 0xc1:
        vol_.SetTarget(a8, static_cast<int16_t>(arg2));
        break;

    case 0xc2:
        player_.main_volume_ = a8;
        break;

    case 0xc3:
        transpose_ = static_cast<int8_t>(arg1);
        break;

    case 0xc4:
        bend_.SetTarget(static_cast<int8_t>(arg1), static_cast<int16_t>(arg2));
        break;

    case 0xc5:
        bend_range_ = a8;
        break;

    case 0xc6:
        priority_ = a8;
        break;

    case 0xc7:
        note_wait_ = arg1 != 0;
        break;

    case 0xc8: // tie
        tie_ = arg1 != 0;
        ReleaseAllChannel();
        FreeAllChannel();
        break;

    case 0xc9: // porta
        porta_ = true;
        porta_key_ = static_cast<uint8_t>(static_cast<uint8_t>(transpose_) + arg1);
        break;

    case 0xca:
        lfo_.depth = static_cast<float>(static_cast<uint32_t>(a8)) * 0.0078125f;
        break;

    case 0xcb:
        lfo_.speed = static_cast<float>(static_cast<uint32_t>(a8)) * 0.390625f;
        break;

    case 0xcc:
        lfo_type_ = a8;
        break;

    case 0xcd:
        lfo_.range = a8;
        break;

    case 0xce:
        porta_ = arg1 != 0;
        break;

    case 0xcf:
        porta_time_ = a8;
        break;

    case 0xd0:
        attack_ = a8;
        break;

    case 0xd1:
        decay_ = a8;
        break;

    case 0xd2:
        sustain_ = a8;
        break;

    case 0xd3:
        release_ = a8;
        break;

    case 0xd4: // loop start
        if (call_depth_ < 3)
        {
            call_stack_[call_depth_].address = current_;
            call_stack_[call_depth_].loop_count = a8;
            call_stack_[call_depth_].is_loop = true;
            call_depth_++;
        }
        break;

    case 0xd5:
        volume2_ = a8;
        break;

    case 0xd6: // printvar, which only debug builds of the game implement
        break;

    case 0xd7:
        span_move_.SetTarget(static_cast<int8_t>(arg1), static_cast<int16_t>(arg2));
        break;

    case 0xd8:
        lpf_ = static_cast<float>(arg1 - 0x40) * 0.015625f;
        break;

    case 0xd9:
        fx_send_[0] = a8;
        break;

    case 0xda:
        fx_send_[1] = a8;
        break;

    case 0xdb:
        main_send_ = a8;
        break;

    case 0xdc:
        init_pan_ = static_cast<int8_t>(arg1 - 0x40);
        break;

    case 0xdd:
        Mute(a8);
        break;

    case 0xdf:
        damper_ = a8 >= 0x40;
        break;

    case 0xe0:
        lfo_.delay = static_cast<uint32_t>(arg1 * 5);
        break;

    case 0xe1:
        player_.tempo_ = static_cast<uint16_t>(std::clamp(arg1, 0, 0x3ff));
        break;

    case 0xe3:
        sweep_pitch_ = static_cast<float>(arg1) * 0.015625f;
        break;

    case 0xfb: // envelope reset
        attack_ = decay_ = sustain_ = release_ = 0xff;
        hold_ = 0xff;
        break;

    case 0xfc: // loop end
        {
            if (call_depth_ == 0)
            {
                break;
            }

            CallStackEntry& e = call_stack_[call_depth_ - 1];
            if (!e.is_loop)
            {
                break;
            }

            uint8_t count = e.loop_count;
            if (count == 0 && index_ == 0 && player_.on_loop_)
            {
                player_.on_loop_(); // infinite loop in the main track
            }

            if (count != 0)
            {
                count--;
                if (count == 0)
                {
                    call_depth_--;
                    break;
                }
            }

            e.loop_count = count;
            current_ = e.address;
            break;
        }

    case 0xfd: // return
        while (call_depth_ != 0)
        {
            call_depth_--;
            if (!call_stack_[call_depth_].is_loop)
            {
                current_ = call_stack_[call_depth_].address;
                break;
            }
        }
        break;

    default:
        break;
    }
}

Channel::Callback SequenceTrack::MakeCallback()
{
    return [this](Channel* ch, Channel::CallbackStatus s)
    {
        OnChannelEvent(ch, s);
    };
}

void SequenceTrack::OnChannelEvent(Channel* ch, Channel::CallbackStatus status)
{
    // 0x31e148
    if (status == Channel::CallbackStatus::kStopped || status == Channel::CallbackStatus::kFinish)
    {
        ch->callback_ = nullptr;
    }

    // SequenceSoundPlayer::ChannelCallback (0x3202b8) is empty.
    if (channel_list_ == ch)
    {
        channel_list_ = ch->next_in_track_;
        return;
    }

    for (Channel* p = channel_list_; p; p = p->next_in_track_)
    {
        if (p->next_in_track_ == ch)
        {
            p->next_in_track_ = ch->next_in_track_;
            return;
        }
    }
}

void SequenceTrack::NoteOn(int key, int velocity, int32_t length, bool tie)
{
    // 0x31e2a4
    const int vel = velocity * velocity_range_ / 127;
    const auto tie_volume = [&]
    {
        const float v = static_cast<float>(vel) * kOneOver127;
        return v * v;
    };

    Channel* ch = nullptr;
    if (tie)
    {
        ch = channel_list_;
        if (ch)
        {
            ch->key_ = static_cast<uint8_t>(key);
            ch->init_volume_ = tie_volume();
        }
    }

    if (mono_)
    {
        ch = channel_list_;
        if (ch)
        {
            if (ch->env_.GetStatus() != EnvGenerator::Status::kRelease)
            {
                ch->length_ = length;
                ch->key_ = static_cast<uint8_t>(key);
                ch->init_volume_ = tie_volume();
            }
            else
            {
                ch->Stop();
                ch = nullptr;
            }
        }
    }

    if (!ch)
    {
        NoteOnInfo info;
        info.prg_no = prg_no_;
        info.key = key;
        info.velocity = vel;
        info.length = tie ? -1 : length;
        info.init_pan = init_pan_;
        info.priority = player_.channel_priority_ + priority_;
        info.callback = MakeCallback();
        ch = player_.GetEngine().NoteOn(player_.GetBank(bank_index_), info);
        if (!ch)
        {
            return;
        }

        if (ch->key_group_ != 0)
        {
            for (Channel* c = channel_list_; c; c = c->next_in_track_)
            {
                if (c->key_group_ == ch->key_group_)
                {
                    c->env_.SetRelease(0x7e);
                    c->Release();
                }
            }
        }

        ch->next_in_track_ = channel_list_;
        channel_list_ = ch;
    }

    if (attack_ <= 127)
    {
        ch->env_.SetAttack(attack_);
    }

    if (decay_ <= 127)
    {
        ch->env_.SetDecay(decay_);
    }

    if (sustain_ <= 127)
    {
        ch->env_.SetSustain(sustain_);
    }

    if (release_ <= 127)
    {
        ch->env_.SetRelease(release_);
    }

    if (hold_ <= 127)
    {
        ch->env_.SetHold(hold_);
    }

    float sweep = sweep_pitch_;
    if (porta_)
    {
        sweep = static_cast<float>(static_cast<int>(porta_key_) - key) + sweep;
    }

    if (porta_time_ != 0)
    {
        const int t = porta_time_ * porta_time_;
        const float mag = (0.0f <= sweep) ? sweep : -sweep;
        const int time = static_cast<int>(mag * static_cast<float>(t)) >> 5;
        ch->SetSweepParam(sweep, time * 5, true);
    }
    else
    {
        ch->SetSweepParam(sweep, length, false);
    }

    porta_key_ = static_cast<uint8_t>(key);

    ch->release_priority_fix_ = player_.release_priority_fix_;
    ch->pan_mode_ = player_.pan_mode_;
    ch->pan_curve_ = player_.pan_curve_;
    if (ch->voice_)
    {
        ch->voice_->SetFrontBypass(front_bypass_);
    }
}

void SequenceTrack::UpdateChannelParam()
{
    // 0x31de18
    if (!open_ || !channel_list_)
    {
        return;
    }

    const uint32_t v = vol_.Get();
    const uint32_t level = v * static_cast<uint32_t>(volume2_ * player_.main_volume_);
    float lv = static_cast<float>(level) * kOneOver127Cubed;
    lv = lv * lv;
    const float ch_volume = player_.volume_ * lv;

    const float bend_semitones =
        (static_cast<float>(bend_.Get()) * 0.0078125f) * static_cast<float>(static_cast<uint32_t>(bend_range_));

    float ch_pan = static_cast<float>(pan_move_.Get()) * kOneOver63;
    if (ch_pan > 1.0f)
    {
        ch_pan = 1.0f;
    }
    else if (ch_pan < -1.0f)
    {
        ch_pan = -1.0f;
    }

    float ch_span = static_cast<float>(span_move_.Get()) * kOneOver63;
    if (ch_span > 2.0f)
    {
        ch_span = 2.0f;
    }
    else if (ch_span < 0.0f)
    {
        ch_span = 0.0f;
    }

    const float ch_main_send = static_cast<float>(static_cast<uint32_t>(main_send_)) * kOneOver127 - 1.0f;
    std::array<float, 2> ch_fx{};
    for (int i = 0; i < 2; i++)
    {
        ch_fx[i] = static_cast<float>(static_cast<uint32_t>(fx_send_[i])) * kOneOver127;
    }

    for (Channel* ch = channel_list_; ch; ch = ch->next_in_track_)
    {
        ch->track_volume_ = ch_volume;
        ch->bend_ = bend_semitones;
        ch->track_pan_ = ch_pan;
        ch->track_span_ = ch_span;
        ch->lpf_ = lpf_;
        ch->biquad_type_ = biquad_type_; // Channel::SetBiquadFilter (0x320908)
        ch->biquad_value_ = biquad_value_;
        ch->main_send_ = ch_main_send;
        ch->fx_send_ = ch_fx;
        ch->lfo_.param_ = lfo_;
        ch->lfo_type_ = lfo_type_;
    }
}

// --------------------------------------------------------------------------------------------- SequenceSoundPlayer
SequenceSoundPlayer::SequenceSoundPlayer(Engine& engine, const csar::SoundInfo& info, csar::Sequence sequence,
                                         std::array<const csar::Bank*, 4> banks)
    : engine_(engine),
      sequence_(sequence),
      banks_(banks),
      allocate_track_flags_(info.allocate_track_flags),
      start_offset_(info.start_offset)
{
    // 0x31fe8c / 0x31f1a0 defaults are the member initializers. BasicSound then applies the sound's parameters: the
    // initial volume, which is the volume times the game's 1/127 (0x316b48), the sequence's channel priority and
    // release-priority flag, and the pan mode/curve.
    volume_ = static_cast<float>(info.volume) * kOneOver127;
    channel_priority_ = info.channel_priority;
    release_priority_fix_ = info.release_priority_fix;
    pan_mode_ = info.pan_mode;
    pan_curve_ = info.pan_curve;
    local_vars_.fill(-1);
}

int16_t* SequenceSoundPlayer::Variable(int index)
{
    // 0x3201ec
    if (index < 0)
    {
        return nullptr;
    }

    if (index < 16)
    {
        return &local_vars_[index];
    }

    if (index < 32)
    {
        return &engine_.GlobalVariables()[index - 16];
    }

    return nullptr;
}

void SequenceSoundPlayer::Start()
{
    for (int i = 0; i < 16; i++)
    {
        if (allocate_track_flags_ & (1u << i))
        {
            tracks_[i] = std::make_unique<SequenceTrack>(*this, i);
        }
    }

    if (tracks_[0])
    {
        tracks_[0]->SetSeqData(sequence_.data, start_offset_);
        tracks_[0]->Open();
    }

    started_ = true;
}

void SequenceSoundPlayer::Update()
{
    // 0x3202c8
    if (!started_ || finished_)
    {
        return;
    }

    UpdateTick();

    for (auto& t : tracks_)
    {
        if (t)
        {
            t->UpdateChannelParam();
        }
    }
}

void SequenceSoundPlayer::UpdateTick()
{
    // 0x31ff50: advance the sequence by one sound frame, in ARM-cycle x 1000 fixed point.
    float tpms = static_cast<float>(static_cast<uint32_t>(timebase_ * tempo_)) * kOneOver60000;
    if (tpms == 0.0f)
    {
        return;
    }

    uint64_t rest = kFrameMilliCycles;
    uint64_t next = static_cast<uint64_t>(tick_fraction_ * kArmClock / tpms);
    while (next < rest)
    {
        rest -= next;
        bool any_open = false;
        for (int i = 0; i < 16; i++)
        {
            SequenceTrack* t = tracks_[i].get();
            if (!t)
            {
                continue;
            }

            t->UpdateChannelLength();

            if (t->ParseNextTick(true) < 0)
            {
                t->Close();
                tracks_[i].reset();
                continue; // the freed track reads as closed
            }

            if (t->IsOpen())
            {
                any_open = true;
            }
        }
        if (!any_open)
        {
            started_ = false;
            for (auto& t : tracks_)
            {
                if (t)
                {
                    t->Close();
                    t.reset();
                }
            }
            finished_ = true;
            return;
        }

        tpms = static_cast<float>(static_cast<uint32_t>(timebase_ * tempo_)) * kOneOver60000;
        if (tpms == 0.0f)
        {
            return;
        }

        next = static_cast<uint64_t>(kArmClock / tpms);
    }

    tick_fraction_ = static_cast<float>(next - rest) * tpms * kOneOverArmClock;
}

} // namespace threesf::nwsnd
