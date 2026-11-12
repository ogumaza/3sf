// SPDX-License-Identifier: MIT

// Unit tests for the effects on the aux buses in the 3SF file format (src/threesf/format.* and
// src/nnsnd/aux_effects.*): the effects that bit 2 of the archive descriptor's flags adds, the effect sets of version 3
// with the 3sf_aux tag, and the records that the firmware's reverb and delay give the DSP. format_test covers the rest
// of the format.

#include "nnsnd/aux_effects.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ntdlib/ctr/nnsnd/dsp_abi.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "check_refused.h"
#include "threesf/format.h"

using namespace threesf;
using namespace xsflib::testing;
using threesf_test::CheckRefused;

using Kind = nnsnd::AuxEffect::Kind;

// Whether two lists hold the same effects with the same parameters.
static bool SameEffects(const nnsnd::AuxEffectLists& a, const nnsnd::AuxEffectLists& b)
{
    for (std::size_t bus = 0; bus < a.size(); bus++)
    {
        if (a[bus].size() != b[bus].size())
        {
            return false;
        }

        for (std::size_t i = 0; i < a[bus].size(); i++)
        {
            const nnsnd::AuxEffect& x = a[bus][i];
            const nnsnd::AuxEffect& y = b[bus][i];
            const auto& r = x.reverb;
            const auto& s = y.reverb;
            const auto& d = x.delay;
            const auto& e = y.delay;
            const bool same_reverb =
                r.early_reflection_time == s.early_reflection_time && r.fused_time == s.fused_time &&
                r.pre_delay_time == s.pre_delay_time && r.coloration == s.coloration && r.damping == s.damping &&
                r.filter_sizes == s.filter_sizes && r.early_gain == s.early_gain && r.fused_gain == s.fused_gain &&
                r.enable_surround == s.enable_surround && r.high_pass_damping == s.high_pass_damping;
            const bool same_delay = d.delay_time == e.delay_time && d.feedback == e.feedback &&
                                    d.damping == e.damping && d.enable_surround == e.enable_surround;
            if (x.kind != y.kind || (x.IsReverb() ? !same_reverb : !same_delay))
            {
                return false;
            }
        }
    }

    return true;
}

// What StreetPass Mii Plaza's code writes into the DSP's mixer parameters for nn::snd's DSP reverb and DSP delay, with
// the buffer at physical address 0x20001000: the 26 halfwords of the reverb's record and the 10 of the delay's, and the
// bytes of the buffer that the code asks for. A run of the code of the Plaza's update v14.6.0 in Unicorn gave them,
// from a hand-set state: the wrapper's constructor, the buffer's size, the buffer's set-up, the bus, the parameters and
// the enable (0x2ef1a8, 0x2eec00, 0x2eeb7c, 0x2eecc4, 0x2eeda8 and 0x2eed1c for the reverb, 0x2eeb50, 0x2ee88c,
// 0x2ee808, 0x2ee8cc, 0x2ee9e8 and 0x2ee95c for the delay), with FPSCR 0x03000000. The code also set bit 12 + bus of
// the mixer's dirty word for the reverb and bit 10 + bus for the delay, and wrote the same record for bus B. The first
// four reverbs are those of Flower Town, Warrior's Way, Mii Force and the other three add-on games, and the first two
// delays those of Market Crashers and Slot Car Rivals. The others try the edges.
struct ReverbVector
{
    ntdlib::ctr_fx::ReverbParam param;
    uint32_t buffer_bytes;
    std::array<uint16_t, 26> record;
};

struct DelayVector
{
    ntdlib::ctr_fx::DelayParam param;
    uint32_t buffer_bytes;
    std::array<uint16_t, 10> record;
};

// clang-format off
constexpr std::array<ReverbVector, 10> kPlazaReverbs = {{
    {{60, 4000, 100, 0.5f, 0.4f, {3040, 3680, 2080}, 0.6f, 0.4f, false, false},
     111360,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x2000, 0x4c00, 0x2000, 0xb000, 0x2001, 0x0f00, 0x2001,
      0x8200, 0x000c, 0x0014, 0x0013, 0x0017, 0x000d, 0x004c, 0x0033, 0x0040, 0x006d, 0x0069, 0x004c, 0x0033}},
    {{5, 1300, 5, 0.6f, 0.5f, {3040, 3680, 2080}, 0.0f, 0.3f, false, false},
     72960,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x2000, 0x1500, 0x2000, 0x1a00, 0x2000, 0x7900, 0x2000,
      0xec00, 0x0001, 0x0001, 0x0013, 0x0017, 0x000d, 0x0000, 0x0026, 0x004c, 0x004e, 0x0046, 0x0040, 0x0040}},
    {{25, 1400, 5, 0.6f, 0.4f, {3040, 3680, 2080}, 0.3f, 0.3f, false, false},
     78080,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x2000, 0x2900, 0x2000, 0x2e00, 0x2000, 0x8d00, 0x2001,
      0x0000, 0x0005, 0x0001, 0x0013, 0x0017, 0x000d, 0x0026, 0x0026, 0x004c, 0x0050, 0x0049, 0x004c, 0x0033}},
    {{25, 2000, 5, 0.6f, 0.4f, {3040, 3680, 2080}, 0.4f, 0.4f, false, false},
     78080,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x2000, 0x2900, 0x2000, 0x2e00, 0x2000, 0x8d00, 0x2001,
      0x0000, 0x0005, 0x0001, 0x0013, 0x0017, 0x000d, 0x0033, 0x0033, 0x004c, 0x005c, 0x0056, 0x004c, 0x0033}},
    {{0, 0, 0, 0.0f, 0.0f, {3040, 3680, 2080}, 1.0f, 1.0f, false, false},
     72960,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x2000, 0x1500, 0x2000, 0x1a00, 0x2000, 0x7900, 0x2000,
      0xec00, 0x0001, 0x0001, 0x0013, 0x0017, 0x000d, 0x0080, 0x0080, 0x0000, 0x0000, 0x0000, 0x0080, 0x0000}},
    {{1000, 10000, 10, 1.0f, 1.0f, {3040, 3680, 2080}, 0.0f, 0.0f, false, false},
     334080,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x2004, 0x0c00, 0x2004, 0x1600, 0x2004, 0x7500, 0x2004,
      0xe800, 0x00cc, 0x0002, 0x0013, 0x0017, 0x000d, 0x0000, 0x0000, 0x0080, 0x0078, 0x0076, 0x0006, 0x0079}},
    {{60, 4000, 100, 0.5f, 0.4f, {3040, 3680, 2080}, 0.6f, 0.4f, false, true},
     111360,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x2000, 0x4c00, 0x2000, 0xb000, 0x2001, 0x0f00, 0x2001,
      0x8200, 0x000c, 0x0014, 0x0013, 0x0017, 0x000d, 0x004c, 0x0033, 0x0040, 0x006d, 0x0069, 0xffb4, 0xffcd}},
    {{60, 4000, 100, 0.5f, 0.97f, {3040, 3680, 2080}, 0.6f, 0.4f, false, true},
     111360,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x2000, 0x4c00, 0x2000, 0xb000, 0x2001, 0x0f00, 0x2001,
      0x8200, 0x000c, 0x0014, 0x0013, 0x0017, 0x000d, 0x004c, 0x0033, 0x0040, 0x006d, 0x0069, 0xfffa, 0xff87}},
    {{7, 333, 3, 0.123f, 0.777f, {1120, 1440, 4960}, 0.05f, 0.95f, false, false},
     62720,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x2000, 0x1500, 0x2000, 0x1a00, 0x2000, 0x3d00, 0x2000,
      0x6a00, 0x0001, 0x0001, 0x0007, 0x0009, 0x001f, 0x0006, 0x0079, 0x000f, 0x003e, 0x0033, 0x001c, 0x0063}},
    {{200, 1000, 200, 0.5f, 0.4f, {3200, 4800, 2080}, 0.6f, 0.4f, false, false},
     183040,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x2000, 0xd800, 0x2001, 0xa000, 0x2002, 0x0400, 0x2002,
      0x9a00, 0x0028, 0x0028, 0x0014, 0x001e, 0x000d, 0x004c, 0x0033, 0x0040, 0x0041, 0x002e, 0x004c, 0x0033}},
}};

constexpr std::array<DelayVector, 6> kPlazaDelays = {{
    {{170, 0.4f, 0.75f, true},
     87040,
     {0x0007, 0x0001, 0x0000, 0x0004, 0x2000, 0x1000, 0x0022, 0x0033, 0x0020, 0x0060}},
    {{170, 0.4f, 0.5f, true},
     87040,
     {0x0007, 0x0001, 0x0000, 0x0004, 0x2000, 0x1000, 0x0022, 0x0033, 0x0040, 0x0040}},
    {{170, 0.4f, 0.75f, false},
     43520,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x0022, 0x0033, 0x0020, 0x0060}},
    {{0, 0.0f, 0.0f, false},
     1280,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x0001, 0x0000, 0x0080, 0x0000}},
    {{250, 1.0f, 0.97f, true},
     130560,
     {0x0007, 0x0001, 0x0000, 0x0004, 0x2000, 0x1000, 0x0033, 0x0080, 0x0006, 0x0079}},
    {{33, 0.123f, 0.456f, false},
     7680,
     {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x0006, 0x000f, 0x0045, 0x003a}},
}};

// The records of Happy Home Designer's two effects for a room: the DSP reverb that the ambient sound "ringing" starts
// and the DSP delay that "echoes" starts. A run of the code of the USA release's revision 1 in Unicorn gave them, from
// a hand-set state, with FPSCR 0x03000000 and the buffer at physical address 0x20001000: the reverb's buffer size
// (0x2cb280), SetParam (0x2cb4b0) and enable (0x2cb358), and the delay's line size (0x2cad1c), bus (0x2cad68), SetParam
// (0x2caf50) and enable (0x2cadf8). The records are those that the code handed the mixer (0x2cd190 for the reverb,
// 0x2cd0f4 for the delay), merged by their dirty bits. The same run with the parameters of the Plaza's 16 vectors above
// gave the Plaza's records and sizes.
constexpr ReverbVector kRoomReverb = {
    {50, 8000, 60, 0.85f, 0.91f, {3040, 3680, 2080}, 0.75f, 1.0f, false, false},
    98560,
    {0x0007, 0x0001, 0x0000, 0x0002, 0x2000, 0x1000, 0x2000, 0x4200, 0x2000, 0x7e00, 0x2000, 0xdd00, 0x2001,
     0x5000, 0x000a, 0x000c, 0x0013, 0x0017, 0x000d, 0x0060, 0x0080, 0x006c, 0x0076, 0x0074, 0x000b, 0x0074}};
constexpr DelayVector kRoomDelay = {
    {285, 0.68f, 0.15f, true},
    148480,
    {0x0007, 0x0001, 0x0000, 0x0004, 0x2000, 0x1000, 0x003a, 0x0057, 0x006c, 0x0013}};
// clang-format on

static void TestFirmwareRecords()
{
    constexpr uint32_t kBuffer = 0x20001000;
    std::vector<ReverbVector> reverbs(kPlazaReverbs.begin(), kPlazaReverbs.end());
    reverbs.push_back(kRoomReverb);
    for (const ReverbVector& v : reverbs)
    {
        const nnsnd::FirmwareReverb reverb(v.param);
        const ntdlib::ctr::nnsnd::FirmwareReverbParams record = reverb.Record(kBuffer);
        XSFLIB_CHECK(std::memcmp(&record, v.record.data(), sizeof(record)) == 0);
        XSFLIB_CHECK(reverb.BufferBytes() == v.buffer_bytes);
    }

    std::vector<DelayVector> delays(kPlazaDelays.begin(), kPlazaDelays.end());
    delays.push_back(kRoomDelay);
    for (const DelayVector& v : delays)
    {
        const nnsnd::FirmwareDelay delay(v.param);
        const ntdlib::ctr::nnsnd::FirmwareDelayParams record = delay.Record(kBuffer);
        XSFLIB_CHECK(std::memcmp(&record, v.record.data(), sizeof(record)) == 0);
        XSFLIB_CHECK(delay.LineBytes() == v.buffer_bytes);
    }
}

// The effects and the descriptor that the tests of bit 2 start from: an nw::snd reverb and a delay on bus A, and
// nn::snd's reverb of 2011 on bus B.
struct AuxEffectsCase
{
    nnsnd::AuxEffect reverb;
    nnsnd::AuxEffect old_reverb;
    ArchiveDescriptor d;
};

static AuxEffectsCase MakeAuxEffectsCase()
{
    nnsnd::AuxEffect reverb;
    reverb.kind = Kind::kNwSndReverb;
    reverb.reverb = {.early_reflection_time = 0,
                     .fused_time = 4600,
                     .pre_delay_time = 1,
                     .coloration = 0.5f,
                     .damping = 0.91f,
                     .filter_sizes = {1120, 1440, 4960},
                     .early_gain = 0.3f,
                     .fused_gain = 0.73f,
                     .enable_surround = true,
                     .high_pass_damping = true};
    nnsnd::AuxEffect delay;
    delay.kind = Kind::kDelay;
    delay.delay = {.delay_time = 128, .feedback = 0.01875f, .damping = 0.2f, .enable_surround = true};
    nnsnd::AuxEffect old_reverb;
    old_reverb.kind = Kind::kNnSndReverb2011;

    ArchiveDescriptor d;
    d.flags = kFlagDspRequired | kFlagAuxEffects;
    d.archive_path = "a.bcsar";
    d.firmware_path = "b.cdc";
    d.aux_effects = {{{reverb, delay}, {old_reverb}}};

    return {reverb, old_reverb, d};
}

// A firmware delay of 270 ms, with a feedback of 0.4 and a damping of 0.9: the delay that Luigi's Mansion of 2018 sets
// up.
static nnsnd::AuxEffect FirmwareDelay()
{
    nnsnd::AuxEffect firmware_delay;
    firmware_delay.kind = Kind::kFirmwareDelay;
    firmware_delay.delay = {.delay_time = 270, .feedback = 0.4f, .damping = 0.9f, .enable_surround = false};

    return firmware_delay;
}

// The layout of the effects in the descriptor, and the messages for effects that Parse reads but Check refuses.
static void TestAuxEffectLayout()
{
    // With bit 2 of the flags, the effects of bus A, then those of bus B, follow the paths (docs/3sf.md, "Archive
    // descriptor" and "Effects on the aux buses").
    const AuxEffectsCase c = MakeAuxEffectsCase();
    const ArchiveDescriptor& d = c.d;

    const auto raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    // 16 + (2 + 7) + (2 + 5) + 2 + (4 + 44) + (4 + 16) + 2 + (4 + 44), padded to 4.
    XSFLIB_CHECK(raw.size() == 152);
    XSFLIB_CHECK(back && back->flags == d.flags && SameEffects(back->aux_effects, d.aux_effects));
    XSFLIB_CHECK(back && !back->Check());

    // The first effect's kind is the u32 after the count, at 16 + 9 + 7 + 2. Its flags end its 44 bytes.
    constexpr std::size_t kKind = 34;
    constexpr std::size_t kFlags = kKind + 4 + 40;
    XSFLIB_CHECK(xsflib::LoadLe32(&raw[kKind]) == 1 && xsflib::LoadLe32(&raw[kFlags]) == 3);

    // An effect of a kind that this player doesn't know, or with flags that the effect doesn't have, gets a message
    // from Check. Parse reads no further.
    std::vector<uint8_t> odd = raw;
    xsflib::StoreLe32(&odd[kKind], 9);
    auto parsed = ArchiveDescriptor::Parse(odd);
    XSFLIB_CHECK(parsed && parsed->aux_effects[0].empty() && parsed->aux_effects[1].empty());
    XSFLIB_CHECK(parsed && parsed->Check() ==
                               "the archive descriptor gives aux bus A an effect of kind 9; this player "
                               "supports kinds 1 to 7");

    odd = raw;
    xsflib::StoreLe32(&odd[kFlags], 7);
    parsed = ArchiveDescriptor::Parse(odd);
    XSFLIB_CHECK(parsed && parsed->Check() ==
                               "the archive descriptor gives aux bus A a reverb with flags 0x7; this "
                               "player supports bits 0 (surround) and 1 (high-pass damping)");

    odd = raw;
    xsflib::StoreLe32(&odd[kFlags + 4 + 16], 2); // the delay's flags
    parsed = ArchiveDescriptor::Parse(odd);
    XSFLIB_CHECK(parsed && parsed->aux_effects[0].size() == 1);
    XSFLIB_CHECK(parsed && parsed->Check() ==
                               "the archive descriptor gives aux bus A a delay with flags 0x2; this "
                               "player supports bit 0 (surround)");

    // A descriptor cut short inside the effects isn't one.
    for (const std::size_t size : {std::size_t{33}, std::size_t{40}, std::size_t{100}, std::size_t{146}})
    {
        XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + size)).has_value());
    }
}

// The parameters that a library refuses, and the limit on the memory of the effects' buffers.
static void TestRefusedAuxEffects()
{
    const AuxEffectsCase c = MakeAuxEffectsCase();
    const nnsnd::AuxEffect& reverb = c.reverb;
    const ArchiveDescriptor& d = c.d;

    // Parameters that the library refuses: nn::snd's reverbs process two channels, and nn::snd's of 2011 has no
    // high-pass damping. A coloration above 1 is out of range for every library.
    for (const Kind kind : {Kind::kNnSndReverb, Kind::kNnSndReverb2011})
    {
        ArchiveDescriptor refused = d;
        refused.aux_effects[1][0].kind = kind;
        refused.aux_effects[1][0].reverb = reverb.reverb;

        XSFLIB_CHECK(refused.Check() ==
                     "the archive descriptor's effects on the aux buses: the reverb's parameters are out of range");
    }

    // nw::snd's reverb of 2011 takes four channels, but it has no high-pass damping either.
    ArchiveDescriptor plaza = d;
    plaza.aux_effects[0][0].kind = Kind::kNwSndReverb2011;
    XSFLIB_CHECK(plaza.Check() ==
                 "the archive descriptor's effects on the aux buses: the reverb's parameters are out of range");
    plaza.aux_effects[0][0].reverb.high_pass_damping = false;
    const auto plaza_back = ArchiveDescriptor::Parse(plaza.Serialize());
    XSFLIB_CHECK(!plaza.Check());
    XSFLIB_CHECK(plaza_back && SameEffects(plaza_back->aux_effects, plaza.aux_effects));

    ArchiveDescriptor refused = d;
    refused.aux_effects[0][0].reverb.coloration = 1.5f;
    XSFLIB_CHECK(refused.Check().has_value());

    // The effects' buffers can't take more than the 178 MiB that a 3DS gives a program. A delay of 1,000 s on four
    // channels needs 499 MiB.
    ArchiveDescriptor large = d;
    large.aux_effects[0][1].delay.delay_time = 1000000;
    XSFLIB_CHECK(large.Check() ==
                 "the archive descriptor's effects on the aux buses need more memory for their "
                 "buffers than the 178 MiB a 3DS gives a program");
}

// The firmware's delay: the limits that Check sets, and the delay's set-up for the DSP.
static void TestFirmwareDelay()
{
    const AuxEffectsCase c = MakeAuxEffectsCase();
    const nnsnd::AuxEffect& old_reverb = c.old_reverb;
    const ArchiveDescriptor& d = c.d;

    // Kind 5, the firmware's delay, has a delay's parameters. A bus has one at most. Its feedback and damping lie from
    // 0 to 1, and the lines of all of them take 16 MiB at most.
    const nnsnd::AuxEffect firmware_delay = FirmwareDelay();

    ArchiveDescriptor firmware = d;
    firmware.aux_effects = {{{firmware_delay}, {old_reverb, firmware_delay}}};
    const auto firmware_back = ArchiveDescriptor::Parse(firmware.Serialize());
    XSFLIB_CHECK(firmware_back && SameEffects(firmware_back->aux_effects, firmware.aux_effects));
    XSFLIB_CHECK(firmware_back && !firmware_back->Check());

    ArchiveDescriptor twice = firmware;
    twice.aux_effects[0].push_back(firmware_delay);
    XSFLIB_CHECK(twice.Check() ==
                 "the archive descriptor's effects on the aux buses: a bus with two of the firmware's delays");

    ArchiveDescriptor loud = firmware;
    loud.aux_effects[0][0].delay.feedback = 1.5f;
    XSFLIB_CHECK(loud.Check() ==
                 "the archive descriptor's effects on the aux buses: the firmware's delay takes a "
                 "feedback and a damping from 0 to 1");

    // 33,000 ms on four channels: 6,751 frames of 2,560 bytes, over 16 MiB.
    ArchiveDescriptor long_line = firmware;
    long_line.aux_effects[0][0].delay = {
        .delay_time = 33000, .feedback = 0.4f, .damping = 0.9f, .enable_surround = true};
    XSFLIB_CHECK(
        long_line.Check() ==
        "the archive descriptor's firmware effects need lines of more than the 16 MiB that a player gives them");

    // Luigi's Mansion of 2018 sets up 270 ms, a feedback of 0.4 and a damping of 0.9 (code.bin 0x3a0930) as 55 frames,
    // 51, 12 and 115 (0x22d8cc). A damping above 0.95 counts as 0.95.
    const nnsnd::FirmwareDelay set_up(firmware_delay.delay);
    XSFLIB_CHECK(set_up.channels == 2 && set_up.frames == 55 && set_up.feedback == 51 && set_up.damping_a == 12 &&
                 set_up.damping_b == 115 && set_up.LineBytes() == 70400);

    const nnsnd::FirmwareDelay damped({.delay_time = 0, .feedback = 1.0f, .damping = 1.0f, .enable_surround = true});
    XSFLIB_CHECK(damped.channels == 4 && damped.frames == 1 && damped.feedback == 128 && damped.damping_a == 6 &&
                 damped.damping_b == 121);
}

// The firmware's reverb: the limits that Check sets.
static void TestFirmwareReverb()
{
    const AuxEffectsCase c = MakeAuxEffectsCase();
    const ArchiveDescriptor& d = c.d;
    const nnsnd::AuxEffect firmware_delay = FirmwareDelay();

    // Kind 7, the firmware's reverb, has a reverb's parameters. A bus has one at most, beside one of the firmware's
    // delays. It has the high-pass damping but no surround, and it takes filters of whole frames, one at least.
    nnsnd::AuxEffect firmware_reverb;
    firmware_reverb.kind = Kind::kFirmwareReverb;
    firmware_reverb.reverb.high_pass_damping = true;

    ArchiveDescriptor dsp = d;
    dsp.aux_effects = {{{firmware_reverb}, {firmware_reverb, firmware_delay}}};
    const auto dsp_back = ArchiveDescriptor::Parse(dsp.Serialize());
    XSFLIB_CHECK(dsp_back && SameEffects(dsp_back->aux_effects, dsp.aux_effects));
    XSFLIB_CHECK(dsp_back && !dsp_back->Check());

    ArchiveDescriptor two_reverbs = dsp;
    two_reverbs.aux_effects[0].push_back(firmware_reverb);
    XSFLIB_CHECK(two_reverbs.Check() ==
                 "the archive descriptor's effects on the aux buses: a bus with two of the firmware's reverbs");

    ArchiveDescriptor surround = dsp;
    surround.aux_effects[0][0].reverb.enable_surround = true;
    XSFLIB_CHECK(surround.Check() ==
                 "the archive descriptor's effects on the aux buses: the firmware's reverb has no surround");

    ArchiveDescriptor bright = dsp;
    bright.aux_effects[1][0].reverb.early_gain = 1.25f;
    XSFLIB_CHECK(bright.Check() ==
                 "the archive descriptor's effects on the aux buses: the firmware's reverb takes a "
                 "coloration, a damping and gains from 0 to 1");

    ArchiveDescriptor partial = dsp;
    partial.aux_effects[0][0].reverb.filter_sizes[1] = 3690;
    XSFLIB_CHECK(partial.Check() ==
                 "the archive descriptor's effects on the aux buses: the firmware's reverb takes "
                 "filters of whole frames, one at least");

    // 33,000 ms of early reflections and of pre-delay: 13,557 frames of 1,280 bytes with the filters, over 16 MiB.
    ArchiveDescriptor long_reverb = dsp;
    long_reverb.aux_effects[0][0].reverb.early_reflection_time = 33000;
    long_reverb.aux_effects[0][0].reverb.pre_delay_time = 33000;
    XSFLIB_CHECK(
        long_reverb.Check() ==
        "the archive descriptor's firmware effects need lines of more than the 16 MiB that a player gives them");
}

// A descriptor without bit 2, and one with bit 2 and no effects.
static void TestNoAuxEffects()
{
    const AuxEffectsCase c = MakeAuxEffectsCase();
    const ArchiveDescriptor& d = c.d;

    // Without the flag, the effects aren't written, and the bytes are those of a descriptor without them.
    ArchiveDescriptor plain = d;
    plain.flags = kFlagDspRequired;
    ArchiveDescriptor none = plain;
    none.aux_effects = {};

    XSFLIB_CHECK(plain.Serialize() == none.Serialize());

    // An empty list on both buses has a count of 0 for each.
    ArchiveDescriptor empty = d;
    empty.aux_effects = {};
    const auto empty_raw = empty.Serialize();
    const auto empty_back = ArchiveDescriptor::Parse(empty_raw);

    XSFLIB_CHECK(empty_raw.size() == 36 && empty_back && !empty_back->Check());
    XSFLIB_CHECK(empty_back && empty_back->aux_effects[0].empty() && empty_back->aux_effects[1].empty());
}

static void TestEffectSets()
{
    // Version 3 gives further sets of effects after those of bit 2 (docs/3sf.md, "Effect sets"). A mini's 3sf_aux tag
    // picks one. Set 0 is the descriptor's aux effects.
    nnsnd::AuxEffect reverb;
    reverb.kind = Kind::kNnSndReverb;
    reverb.reverb = {.early_reflection_time = 10,
                     .fused_time = 1200,
                     .pre_delay_time = 40,
                     .coloration = 0.1f,
                     .damping = 0.6f,
                     .filter_sizes = {160, 320, 480},
                     .early_gain = 0.5f,
                     .fused_gain = 0.4f};
    nnsnd::AuxEffect delay;
    delay.kind = Kind::kDelay;
    delay.delay = {.delay_time = 128, .feedback = 0.25f, .damping = 0.2f};

    ArchiveDescriptor d;
    d.version = ArchiveDescriptor::kEffectSetsVersion;
    d.flags = kFlagDspRequired | kFlagAuxEffects;
    d.archive_path = "a.bcsar";
    d.firmware_path = "b.cdc";
    d.effect_sets = {{{{reverb}, {}}}, {{{}, {delay}}}};

    const auto raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    // 16 + (2 + 7) + (2 + 5), set 0's counts (2 + 2), the number of sets (2), set 1: 2 + (4 + 44) + 2, and set 2:
    // 2 + 2 + (4 + 16), padded to 4.
    XSFLIB_CHECK(raw.size() == 116);
    XSFLIB_CHECK(xsflib::LoadLe16(&raw[36]) == 2 && xsflib::LoadLe32(&raw[40]) == 2);
    XSFLIB_CHECK(back && back->version == 3 && back->flags == d.flags && !back->Check());
    XSFLIB_CHECK(back && back->aux_effects[0].empty() && back->aux_effects[1].empty());
    XSFLIB_CHECK(back && back->effect_sets.size() == 2 && SameEffects(back->effect_sets[0], d.effect_sets[0]) &&
                 SameEffects(back->effect_sets[1], d.effect_sets[1]));

    // EffectSet finds a set by its number. A number that the descriptor doesn't give finds none.
    XSFLIB_CHECK(EffectSet(d, 0) == &d.aux_effects && EffectSet(d, 2) == &d.effect_sets[1] && !EffectSet(d, 3));

    // A descriptor cut short inside the sets isn't one.
    for (const std::size_t size : {std::size_t{37}, std::size_t{39}, std::size_t{60}, std::size_t{110}})
    {
        XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + size)).has_value());
    }

    // Version 3 gives its sets with the effects. A set's effect that this player can't play, or whose buffers take too
    // much memory, gets a message that names the set.
    ArchiveDescriptor bare = d;
    bare.flags = kFlagDspRequired;
    XSFLIB_CHECK(bare.Check() ==
                 "archive descriptor version 3 without bit 2 (aux effects) of its flags; version 3 "
                 "gives its effect sets with the effects");

    std::vector<uint8_t> odd = raw;
    xsflib::StoreLe32(&odd[40], 9);
    const auto parsed = ArchiveDescriptor::Parse(odd);
    XSFLIB_CHECK(parsed && parsed->Check() ==
                               "the archive descriptor gives aux bus A of effect set 1 an effect of kind 9; this "
                               "player supports kinds 1 to 7");

    ArchiveDescriptor large = d;
    large.effect_sets[1][1][0].delay.delay_time = 2000000;
    XSFLIB_CHECK(large.Check() ==
                 "the archive descriptor's effect set 2 needs more memory for its buffers than the "
                 "178 MiB a 3DS gives a program");

    ArchiveDescriptor refused = d;
    refused.effect_sets[0][0][0].reverb.coloration = 1.5f;
    XSFLIB_CHECK(refused.Check() == "the archive descriptor's effect set 1: the reverb's parameters are out of range");

    // Versions 1 and 2 have no sets. Their bytes are those that players of 3SF 1.4 read, and they load as before.
    for (const uint32_t version : {ArchiveDescriptor::kVersion, ArchiveDescriptor::kListVersion})
    {
        ArchiveDescriptor older = d;
        older.version = version;
        older.aux_effects = {{{delay}, {}}};
        ArchiveDescriptor plain = older;
        plain.effect_sets = {};

        const auto older_raw = older.Serialize();
        const auto older_back = ArchiveDescriptor::Parse(older_raw);
        XSFLIB_CHECK(older_raw == plain.Serialize() && older_raw.size() == 56);
        XSFLIB_CHECK(older_back && !older_back->Check() && older_back->effect_sets.empty() &&
                     SameEffects(older_back->aux_effects, older.aux_effects));
    }

    // The 3sf_aux tag gives the set: 0 to 65535, in decimal, with white space around it allowed. A mini without the
    // tag, or with one that doesn't parse, plays set 0.
    XSFLIB_CHECK(ParseEffectSet(" 1\n") == 1u && ParseEffectSet("65535") == 65535u && ParseEffectSet("007") == 7u);
    CheckRefused(ParseEffectSet, {"", " ", "-1", "65536", "0x1", "1 2", "1.0", "+1", "a"});
    XSFLIB_CHECK(EffectSetOf({{"3sf_aux", "2"}}) == 2u && EffectSetOf({}) == 0u &&
                 EffectSetOf({{"3sf_aux", "x"}}) == 0u);
}

int main()
{
    TestAuxEffectLayout();
    TestRefusedAuxEffects();
    TestFirmwareDelay();
    TestFirmwareReverb();
    TestNoAuxEffects();
    TestFirmwareRecords();
    TestEffectSets();

    return Summary("aux effects");
}
