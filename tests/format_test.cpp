// SPDX-License-Identifier: MIT

// Unit tests for the 3SF file format code (src/threesf/format.*): the version byte, the descriptors with their group
// files, effects and effect sets, _lib loading, and the masks of the 3sf_mute and 3sf_silence tags. xsflib's tests
// cover the PSF container, tags and times.

#include "threesf/format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"
#include "xsflib/text.h"

#include "nnsnd/aux_effects.h"

using namespace threesf;
using namespace xsflib::testing;

static void TestVersionByte()
{
    // A file that 3SF writes has version byte 0x26 (docs/3sf.md, "Version byte"). xsflib's psf_test checks the PSF
    // container with another version byte.
    const auto file = WritePsf(kPsfVersion3sf, {}, {}, {});

    XSFLIB_CHECK(file.size() >= 16 && std::memcmp(file.data(), "PSF", 3) == 0 && file[3] == 0x26);
}

static void TestDescriptor()
{
    ProcessDescriptor d;
    d.version = 2;
    d.entry = 0x108000;
    d.stack_size = 0x8000;
    d.priority = 0x2c;
    d.app_memory = 0x06000000;
    d.status_address = 0x0e000018;
    d.flags = 3;

    const auto bytes = d.Serialize();
    const auto back = ProcessDescriptor::Parse(bytes);

    XSFLIB_CHECK(bytes.size() == 32);
    XSFLIB_CHECK(std::memcmp(bytes.data(), "3SFP", 4) == 0);
    XSFLIB_CHECK(back.has_value());
    XSFLIB_CHECK(back->version == d.version && back->entry == d.entry && back->stack_size == d.stack_size &&
                 back->priority == d.priority && back->app_memory == d.app_memory &&
                 back->status_address == d.status_address && back->flags == d.flags);

    // Data without the magic, or too short, isn't a descriptor.
    XSFLIB_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(32, 0)).has_value());
    XSFLIB_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(8, 0)).has_value());
}

static void TestLoadSet()
{
    // A .mini3sf and its .3sflib, with the library's process descriptor. xsflib's psf_set_test covers the loader's
    // other rules: the order and paths of a set's files, its tags, a set in an archive, and the errors of a missing
    // library, a _lib cycle and a set of too many files.
    std::map<std::string, std::vector<uint8_t>> fs;
    const FileReader reader = MapReader(fs);
    ProcessDescriptor d;
    d.status_address = 0x1234;
    fs["dir/lib.3sflib"] = WritePsf(kPsfVersion3sf, d.Serialize(), {}, {});
    fs["dir/x.mini3sf"] = WritePsf(kPsfVersion3sf, {}, {}, {{"_lib", "lib.3sflib"}});

    LoadedSet set;
    XSFLIB_CHECK(!LoadSet("dir/x.mini3sf", reader, set));
    XSFLIB_CHECK(set.descriptor && set.descriptor->status_address == 0x1234);

    // Another format's file and a set without a descriptor are errors.
    fs["dir/z.minincsf"] = WritePsf(0x25, {}, {}, {{"_lib", "lib.3sflib"}});
    fs["dir/w.mini3sf"] = WritePsf(kPsfVersion3sf, {}, {}, {{"title", "w"}});

    XSFLIB_CHECK(LoadError<SetFormat>("dir/z.minincsf", reader).find("not a 3SF file") != std::string::npos);
    XSFLIB_CHECK(LoadError<SetFormat>("dir/w.mini3sf", reader).starts_with("no process or archive descriptor"));

    // So is a descriptor with an unsupported version, or with application memory outside the 3DS's range.
    ProcessDescriptor v2;
    v2.version = 2;
    ProcessDescriptor none;
    none.app_memory = 0;
    ProcessDescriptor huge;
    huge.app_memory = ProcessDescriptor::kMaxAppMemory + 0x1000;
    ProcessDescriptor most;
    most.app_memory = ProcessDescriptor::kMaxAppMemory;
    fs["dir/v2.3sflib"] = WritePsf(kPsfVersion3sf, v2.Serialize(), {}, {});
    fs["dir/none.3sflib"] = WritePsf(kPsfVersion3sf, none.Serialize(), {}, {});
    fs["dir/huge.3sflib"] = WritePsf(kPsfVersion3sf, huge.Serialize(), {}, {});
    fs["dir/most.3sflib"] = WritePsf(kPsfVersion3sf, most.Serialize(), {}, {});

    XSFLIB_CHECK(LoadSet("dir/v2.3sflib", reader, set).has_value());
    XSFLIB_CHECK(LoadSet("dir/none.3sflib", reader, set).has_value());
    XSFLIB_CHECK(LoadSet("dir/huge.3sflib", reader, set).has_value());
    XSFLIB_CHECK(!LoadSet("dir/most.3sflib", reader, set));

    // Or flags other than bit 0 alone.
    for (const uint32_t flags : {0u, 3u, 0x80000001u})
    {
        ProcessDescriptor odd;
        odd.flags = flags;
        fs["dir/flags.3sflib"] = WritePsf(kPsfVersion3sf, odd.Serialize(), {}, {});

        XSFLIB_CHECK(LoadSet("dir/flags.3sflib", reader, set).has_value());
    }

    // A driver block of version 1 in stereo plays. One of another version, or with another output mode, doesn't,
    // including when a mini3sf patches the output mode.
    const auto driver_block = [](uint32_t version, uint32_t mode)
    {
        std::vector<uint8_t> block(0x20, 0);
        const uint32_t words[] = {DriverBlock::kMagic, version, 0, 0, mode};
        for (std::size_t i = 0; i < std::size(words); i++)
        {
            xsflib::StoreLe32(block.data() + 4 * i, words[i]);
        }

        ProgramBuilder b;
        b.AddMemory(DriverBlock::kAddress, block);

        return b.Data();
    };

    fs["dir/stereo.3sflib"] = WritePsf(kPsfVersion3sf, d.Serialize(), driver_block(1, kOutputStereo), {});
    fs["dir/mono.3sflib"] = WritePsf(kPsfVersion3sf, d.Serialize(), driver_block(1, 0), {});
    fs["dir/surround.3sflib"] = WritePsf(kPsfVersion3sf, d.Serialize(), driver_block(1, 2), {});
    fs["dir/driver2.3sflib"] = WritePsf(kPsfVersion3sf, d.Serialize(), driver_block(2, kOutputStereo), {});
    {
        ProgramBuilder b;
        b.AddMemory(DriverBlock::kAddress + DriverBlock::kOutputModeOffset, std::vector<uint8_t>{2, 0, 0, 0});
        fs["dir/patched.mini3sf"] = WritePsf(kPsfVersion3sf, {}, b.Data(), {{"_lib", "stereo.3sflib"}});
    }

    XSFLIB_CHECK(!LoadSet("dir/stereo.3sflib", reader, set));
    XSFLIB_CHECK(LoadSet("dir/mono.3sflib", reader, set).has_value());
    XSFLIB_CHECK(LoadSet("dir/surround.3sflib", reader, set).has_value());
    XSFLIB_CHECK(LoadSet("dir/driver2.3sflib", reader, set).has_value());
    XSFLIB_CHECK(LoadSet("dir/patched.mini3sf", reader, set).has_value());
}

static void TestArchiveMode()
{
    ArchiveDescriptor d;
    d.version = 2;
    d.flags = 0;
    d.output_mode = 2;
    d.archive_path = "sound/b.bcsar";
    d.firmware_path = "dspaudio.cdc";

    const auto raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    XSFLIB_CHECK(raw.size() % 4 == 0);
    XSFLIB_CHECK(back && back->version == d.version && back->flags == d.flags && back->output_mode == d.output_mode &&
                 back->archive_path == d.archive_path && back->firmware_path == d.firmware_path);
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + 18)).has_value());
    XSFLIB_CHECK(!ProcessDescriptor::Parse(raw).has_value());

    // A player takes versions 1 to 3, with bit 0 of the flags set, bit 1 for group files and bit 2 for effects, in
    // stereo. TestEffectSets checks version 3.
    XSFLIB_CHECK(d.Check().has_value());

    ArchiveDescriptor v1 = d;
    v1.version = 1;
    v1.flags = kFlagDspRequired;
    v1.output_mode = kOutputStereo;
    ArchiveDescriptor v2 = v1;
    v2.version = ArchiveDescriptor::kListVersion;
    ArchiveDescriptor groups = v1;
    groups.flags = kFlagDspRequired | kFlagGroupFiles;

    XSFLIB_CHECK(!v1.Check() && !v2.Check() && !groups.Check());

    for (const uint32_t version : {0u, 4u})
    {
        ArchiveDescriptor odd = v1;
        odd.version = version;

        XSFLIB_CHECK(odd.Check() == "archive descriptor version " + std::to_string(version) +
                                        "; this player supports versions 1 to 3");
    }

    for (const uint32_t flags : {0u, 2u, 9u, 0x21u, 0x80000001u})
    {
        ArchiveDescriptor odd = v1;
        odd.flags = flags;

        XSFLIB_CHECK(odd.Check() == "the archive descriptor's flags are " + xsflib::Hex(flags) +
                                        "; this player requires bit 0 (DSP required) and supports bits 1 (group "
                                        "files), 2 (aux effects) and 4 (timed starts)");
    }

    // Bit 4 marks a set whose files may hold the 3sf_var_change and 3sf_seed tags.
    ArchiveDescriptor timed = v1;
    timed.flags = kFlagDspRequired | kFlagTimedStarts;

    XSFLIB_CHECK(!timed.Check());

    for (const uint32_t mode : {0u, 2u, 3u})
    {
        ArchiveDescriptor odd = v1;
        odd.output_mode = mode;

        XSFLIB_CHECK(odd.Check().has_value());
    }

    const auto raw1 = v1.Serialize();

    // A later descriptor of the other kind decides the mode; the last SND chunk selects the sound.
    ProgramBuilder lib;
    lib.AddFile("sound/b.bcsar", {1, 2, 3});
    lib.AddSound(0x01000005);
    ProgramBuilder mini;
    mini.AddSound(0x01000007);
    ProcessDescriptor game;

    std::map<std::string, std::vector<uint8_t>> fs;
    fs["a/lib.3sflib"] = WritePsf(kPsfVersion3sf, raw1, lib.Data(), {});
    fs["a/x.mini3sf"] = WritePsf(kPsfVersion3sf, {}, mini.Data(), {{"_lib", "lib.3sflib"}});
    fs["a/y.mini3sf"] = WritePsf(kPsfVersion3sf, game.Serialize(), {}, {{"_lib", "lib.3sflib"}});
    fs["a/nosound.3sflib"] = WritePsf(kPsfVersion3sf, raw1, {}, {});
    fs["a/z.mini3sf"] = WritePsf(kPsfVersion3sf, {}, {}, {{"_lib", "nosound.3sflib"}});

    const FileReader reader = MapReader(fs);

    LoadedSet set;
    XSFLIB_CHECK(!LoadSet("a/x.mini3sf", reader, set));
    XSFLIB_CHECK(set.archive.has_value() && !set.descriptor.has_value());
    XSFLIB_CHECK(set.sound == 0x01000007u);
    XSFLIB_CHECK(set.files.size() == 1 && set.files[0].path == "sound/b.bcsar");

    XSFLIB_CHECK(!LoadSet("a/y.mini3sf", reader, set));
    XSFLIB_CHECK(set.descriptor.has_value() && !set.archive.has_value());

    XSFLIB_CHECK(LoadSet("a/z.mini3sf", reader, set).has_value()); // archive mode without a sound
}

static void TestGroupFiles()
{
    // With bit 1 of the flags, the group files' paths follow the firmware's (docs/3sf.md, "Archive descriptor").
    ArchiveDescriptor d;
    d.flags = kFlagDspRequired | kFlagGroupFiles;
    d.archive_path = "Sound.bcsar";
    d.firmware_path = "dspaudio.cdc";
    d.group_paths = {"GROUP_A.bcgrp", "sound/グループ.bcgrp"};

    const auto raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    XSFLIB_CHECK(raw.size() % 4 == 0);
    XSFLIB_CHECK(back && back->flags == d.flags && back->firmware_path == d.firmware_path &&
                 back->group_paths == d.group_paths);
    XSFLIB_CHECK(!back->Check());

    // The list's count and each path take two bytes of length. A descriptor cut short inside the list isn't one.
    ArchiveDescriptor none = d;
    none.group_paths.clear();
    const auto empty_list = none.Serialize();

    XSFLIB_CHECK(raw.size() == 88); // 16 + (2 + 11) + (2 + 12) + 2 + (2 + 13) + (2 + 24), padded to 4
    XSFLIB_CHECK(empty_list.size() == 48 && ArchiveDescriptor::Parse(empty_list)->group_paths.empty());
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + 60)).has_value());

    // Without the flag, there's no list, and the paths aren't written. The bytes are a version 1 descriptor's.
    ArchiveDescriptor plain = d;
    plain.flags = kFlagDspRequired;
    ArchiveDescriptor v1 = plain;
    v1.group_paths.clear();

    XSFLIB_CHECK(plain.Serialize() == v1.Serialize());
    XSFLIB_CHECK(ArchiveDescriptor::Parse(plain.Serialize())->group_paths.empty());
}

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
        const nnsnd::FirmwareReverbParams record = reverb.Record(kBuffer);
        XSFLIB_CHECK(std::memcmp(&record, v.record.data(), sizeof(record)) == 0);
        XSFLIB_CHECK(reverb.BufferBytes() == v.buffer_bytes);
    }

    // The delay's record comes from SoundSystem::SetAuxEffects.
    std::vector<DelayVector> delays(kPlazaDelays.begin(), kPlazaDelays.end());
    delays.push_back(kRoomDelay);
    for (const DelayVector& v : delays)
    {
        const nnsnd::FirmwareDelay delay(v.param);
        const std::array<uint16_t, 10> record = {7,
                                                 1,
                                                 0,
                                                 delay.channels,
                                                 kBuffer >> 16,
                                                 kBuffer & 0xffff,
                                                 static_cast<uint16_t>(delay.frames),
                                                 delay.feedback,
                                                 delay.damping_a,
                                                 delay.damping_b};
        XSFLIB_CHECK(record == v.record && delay.LineBytes() == v.buffer_bytes);
    }
}

static void TestAuxEffects()
{
    // With bit 2 of the flags, the effects of bus A, then those of bus B, follow the paths (docs/3sf.md, "Archive
    // descriptor" and "Effects on the aux buses").
    using Kind = nnsnd::AuxEffect::Kind;
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

    // Kind 5, the firmware's delay, has a delay's parameters. A bus has one at most. Its feedback and damping lie from
    // 0 to 1, and the lines of all of them take 16 MiB at most.
    nnsnd::AuxEffect firmware_delay;
    firmware_delay.kind = Kind::kFirmwareDelay;
    firmware_delay.delay = {.delay_time = 270, .feedback = 0.4f, .damping = 0.9f, .enable_surround = false};

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
    using Kind = nnsnd::AuxEffect::Kind;
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
    for (const char* bad : {"", " ", "-1", "65536", "0x1", "1 2", "1.0", "+1", "a"})
    {
        Check(!ParseEffectSet(bad), std::string("\"") + bad + "\" is refused");
    }
    XSFLIB_CHECK(EffectSetOf({{"3sf_aux", "2"}}) == 2u && EffectSetOf({}) == 0u &&
                 EffectSetOf({{"3sf_aux", "x"}}) == 0u);
}

static void TestSoundsThatPlayTogether()
{
    // A SND chunk with a list selects sounds that play together, in its order (docs/3sf.md, "Sounds that play
    // together"). Version 2 of the archive descriptor plays them all. Version 1 plays the first alone.
    ArchiveDescriptor v1;
    v1.archive_path = "Sound.bcsar";
    v1.firmware_path = "dspaudio.cdc";
    ArchiveDescriptor v2 = v1;
    v2.version = ArchiveDescriptor::kListVersion;

    ProgramBuilder lib;
    lib.AddFile("Sound.bcsar", {1, 2, 3});
    lib.AddSound(0x01000005);
    ProgramBuilder list;
    const uint32_t kList[] = {0x01000009, 0x01000007, 0x01000008};
    list.AddSounds(kList);
    ProgramBuilder one;
    one.AddSound(0x01000004);

    std::map<std::string, std::vector<uint8_t>> fs;
    fs["a/v1.3sflib"] = WritePsf(kPsfVersion3sf, v1.Serialize(), lib.Data(), {});
    fs["a/v2.3sflib"] = WritePsf(kPsfVersion3sf, v2.Serialize(), lib.Data(), {});
    fs["a/list1.mini3sf"] = WritePsf(kPsfVersion3sf, {}, list.Data(), {{"_lib", "v1.3sflib"}});
    fs["a/list2.mini3sf"] = WritePsf(kPsfVersion3sf, {}, list.Data(), {{"_lib", "v2.3sflib"}});
    fs["a/one2.mini3sf"] = WritePsf(kPsfVersion3sf, {}, one.Data(), {{"_lib", "v2.3sflib"}});
    fs["a/lib2.mini3sf"] = WritePsf(kPsfVersion3sf, {}, {}, {{"_lib", "v2.3sflib"}});

    const FileReader reader = MapReader(fs);
    const std::vector<uint32_t> kAll = {0x01000009, 0x01000007, 0x01000008};
    LoadedSet set;

    XSFLIB_CHECK(!LoadSet("a/list2.mini3sf", reader, set));
    XSFLIB_CHECK(set.sounds == kAll && SoundsToPlay(set) == kAll);

    XSFLIB_CHECK(!LoadSet("a/list1.mini3sf", reader, set));
    XSFLIB_CHECK(set.sounds == kAll && SoundsToPlay(set) == std::vector<uint32_t>{0x01000009});

    XSFLIB_CHECK(!LoadSet("a/one2.mini3sf", reader, set));
    XSFLIB_CHECK(SoundsToPlay(set) == std::vector<uint32_t>{0x01000004});

    XSFLIB_CHECK(!LoadSet("a/lib2.mini3sf", reader, set));
    XSFLIB_CHECK(SoundsToPlay(set) == std::vector<uint32_t>{0x01000005});

    // A set without an archive descriptor plays no sound of a list.
    set.archive.reset();

    XSFLIB_CHECK(SoundsToPlay(set).empty());
}

static void TestTrackMasks()
{
    // A mask is 0 to 65535, in decimal or in hex with a 0x prefix, with white space around it (docs/3sf.md, "Tags").
    XSFLIB_CHECK(ParseTrackMask("0xfff0") == 0xfff0);
    XSFLIB_CHECK(ParseTrackMask("0XFFF0") == 0xfff0);
    XSFLIB_CHECK(ParseTrackMask(" 8192\n") == 0x2000);
    XSFLIB_CHECK(ParseTrackMask("0") == 0);
    XSFLIB_CHECK(ParseTrackMask("65535") == 0xffff);
    XSFLIB_CHECK(ParseTrackMask("0x0000ffff") == 0xffff);
    for (const char* bad : {"", " ", "65536", "0x10000", "-1", "+1", "0x", "0x-1", "1 2", "0xfffg", "ff", "1.0"})
    {
        Check(!ParseTrackMask(bad), std::string("\"") + bad + "\" is refused");
    }

    // The tags give the masks. A tag that's missing or doesn't parse quiets no track.
    XSFLIB_CHECK(SequenceStartOf({}) == SequenceStart{});
    XSFLIB_CHECK((SequenceStartOf({{"3sf_mute", "0x8000"}, {"3sf_silence", "15360"}}) ==
                  SequenceStart{.muted = 0x8000, .silenced = 0x3c00}));
    XSFLIB_CHECK(
        (SequenceStartOf({{"3sf_mute", "all"}, {"3sf_silence", "0x2000"}}) == SequenceStart{.silenced = 0x2000}));

    // The ripper writes each mask as 0x and four hex digits, and no tag for an empty mask.
    Tags tags;
    WriteSequenceStart(SequenceStart{.muted = 0xfff0}, tags);
    XSFLIB_CHECK(tags.size() == 1 && tags["3sf_mute"] == "0xfff0");
    WriteSequenceStart(SequenceStart{.muted = 0x8000, .silenced = 0x3c00}, tags);
    XSFLIB_CHECK(tags["3sf_mute"] == "0x8000" && tags["3sf_silence"] == "0x3c00");
    XSFLIB_CHECK((SequenceStartOf(tags) == SequenceStart{.muted = 0x8000, .silenced = 0x3c00}));
    Tags none;
    WriteSequenceStart(SequenceStart{}, none);
    XSFLIB_CHECK(none.empty());
}

static void TestTimedStarts()
{
    // A change is F:N=V, and the tag holds one or more, separated by commas or white space (docs/3sf.md, "Tags").
    using Changes = std::vector<SequenceVariable>;
    XSFLIB_CHECK((ParseVariableChanges("1:16=1") == Changes{{.index = 16, .value = 1, .frame = 1}}));
    XSFLIB_CHECK((ParseVariableChanges(" 0x10:0=-2,\n 5:31=0x7fff ") ==
                  Changes{{.index = 0, .value = -2, .frame = 16}, {.index = 31, .value = 32767, .frame = 5}}));
    XSFLIB_CHECK((ParseVariableChanges("0:17=-32768") == Changes{{.index = 17, .value = -32768, .frame = 0}}));
    XSFLIB_CHECK((ParseVariableChanges("1:47=0") == Changes{{.index = 47, .value = 0, .frame = 1}}));
    for (const char* bad : {"", " , ", "1:16", "16=1", "1=16:1", "1:48=0", "1:16=32768", "-1:16=1", "1:16=1, x",
                            "1:-1=0", "4294967296:0=0", "1:16=1;"})
    {
        Check(!ParseVariableChanges(bad), std::string("\"") + bad + "\" is refused");
    }

    // A seed is 0 to 0xFFFFFFFF, in decimal or hex.
    XSFLIB_CHECK(ParseSeed(" 0x8d9333a4\n") == 0x8d9333a4u);
    XSFLIB_CHECK(ParseSeed("4294967295") == 0xffffffffu);
    for (const char* bad : {"", "-1", "0x100000000", "1 2", "seed"})
    {
        Check(!ParseSeed(bad), std::string("\"") + bad + "\" is refused");
    }

    // The ripper writes a start's variables of frame 0 into 3sf_var, in the place of --var's for the same variable,
    // and the later ones into 3sf_var_change. The seed goes into 3sf_seed as 0x and eight hex digits.
    Tags tags = {{"3sf_var", "0=1, 16=5"}};
    const SequenceStart start{.variables = {{{.index = 16, .value = 0}, {.index = 16, .value = 1, .frame = 1}}},
                              .seed = 0x995e6351};
    WriteSequenceStart(start, tags);
    XSFLIB_CHECK(tags["3sf_var"] == "0=1, 16=0" && tags["3sf_var_change"] == "1:16=1" &&
                 tags["3sf_seed"] == "0x995e6351");
    XSFLIB_CHECK(SequenceStartOf(tags).seed == 0x995e6351u);
    XSFLIB_CHECK((VariableChangesOf(tags) == Changes{{.index = 16, .value = 1, .frame = 1}}));
    XSFLIB_CHECK(NeedsTimedStarts(start));
    XSFLIB_CHECK(!NeedsTimedStarts(SequenceStart{.muted = 1, .variables = {{{.index = 19, .value = 0}}}}));

    // A tag that doesn't parse gives no change and no seed.
    XSFLIB_CHECK(VariableChangesOf({{"3sf_var_change", "1:16"}}).empty());
    XSFLIB_CHECK(!SequenceStartOf({{"3sf_seed", "-5"}}).seed);

    // 3sf_tempo and 3sf_pitch give ratios from 1/16 to 16, as digits with an optional '.' and more digits.
    XSFLIB_CHECK(ParseRatio(" 1.3\n") == 1.3f);
    XSFLIB_CHECK(ParseRatio("1.1625") == 1.1625f);
    XSFLIB_CHECK(ParseRatio("16") == 16.0f && ParseRatio("0.0625") == 0.0625f);
    for (const char* bad : {"", "1.", ".5", "+1", "-1", "1,3", "1e0", "0x1", "16.01", "0.0624", "0", "1 2", "inf"})
    {
        Check(!ParseRatio(bad), std::string("\"") + bad + "\" is refused");
    }

    // The ripper writes each ratio other than 1 as the shortest decimal that reads back as the same float, and only
    // a tempo ratio or pitch other than 1 requires bit 4.
    Tags quick;
    const SequenceStart fast{.tempo_ratio = 1.1625f, .pitch = 1.25f};
    WriteSequenceStart(fast, quick);
    XSFLIB_CHECK(quick.size() == 2 && quick["3sf_tempo"] == "1.1625" && quick["3sf_pitch"] == "1.25");
    XSFLIB_CHECK(SequenceStartOf(quick) == fast);
    Tags hurry;
    WriteSequenceStart(SequenceStart{.tempo_ratio = 1.3f}, hurry);
    XSFLIB_CHECK(hurry.size() == 1 && hurry["3sf_tempo"] == "1.3");
    XSFLIB_CHECK(NeedsTimedStarts(SequenceStart{.tempo_ratio = 1.3f}) &&
                 NeedsTimedStarts(SequenceStart{.pitch = 0.5f}));
    XSFLIB_CHECK(SequenceStartOf({{"3sf_tempo", "17"}, {"3sf_pitch", "x"}}) == SequenceStart{});

    // 3sf_fx_send gives the sends on aux buses A and B, each from 0 to 1. A single value gives bus A's.
    using Sends = std::array<float, 2>;
    XSFLIB_CHECK((ParseSends("0.4") == Sends{0.4f, 0.0f} && ParseSends(" 0.4 ,\n0.25 ") == Sends{0.4f, 0.25f}));
    XSFLIB_CHECK((ParseSends("1,0") == Sends{1.0f, 0.0f}));
    for (const char* bad : {"", ",", "0.4,", ",0.4", "1.5", "-0.1", "0.4,0.2,0.1", "0.4;0.2", "1e0", "x"})
    {
        Check(!ParseSends(bad), std::string("\"") + bad + "\" is refused");
    }

    // The ripper writes both sends when either isn't 0, and they require bit 4. The game's effect writes no tag: the
    // ripper gives it 3sf_aux.
    Tags sent;
    const SequenceStart club{.fx_send = {0.4f, 0.0f}};
    WriteSequenceStart(club, sent);
    XSFLIB_CHECK(sent.size() == 1 && sent["3sf_fx_send"] == "0.4,0");
    XSFLIB_CHECK(SequenceStartOf(sent) == club && NeedsTimedStarts(club));
    Tags effect;
    WriteSequenceStart(SequenceStart{.game_effect = 0}, effect);
    XSFLIB_CHECK(effect.empty() && !NeedsTimedStarts(SequenceStart{.game_effect = 0}));
    XSFLIB_CHECK(SequenceStartOf({{"3sf_fx_send", "2"}}) == SequenceStart{});

    // 3sf_track_volume gives tracks' volumes as N=V, each from 0 to 16. The tracks that it leaves out play at 1.
    TrackVolumes volumes = ntdlib::ctr::snd::kFullTrackVolumes;
    volumes[8] = 0.8f;
    volumes[9] = 0.8f;
    XSFLIB_CHECK(ParseTrackVolumes("8=0.8, 9=0.8") == volumes && ParseTrackVolumes(" 0x8=0.8\n9=0.8 ") == volumes);
    XSFLIB_CHECK(ParseTrackVolumes("15=0")->at(15) == 0.0f && ParseTrackVolumes("0=16")->at(0) == 16.0f);
    for (const char* bad :
         {"", " , ", "8", "8=", "=0.8", "16=1", "-1=1", "8=16.5", "8=-0.1", "8 = 0.8", "8=0.8;9=1", "8=1e0"})
    {
        Check(!ParseTrackVolumes(bad), std::string("\"") + bad + "\" is refused");
    }

    // The ripper writes each track's volume other than 1, and the volumes require bit 4 (Mii Maker's later mixes). The
    // start's volume goes into the volume tag.
    volumes[10] = 0.75f;
    const SequenceStart mix{.silenced = 0xf898, .track_volumes = volumes, .volume = 0.7f};
    Tags mixed;
    WriteSequenceStart(mix, mixed);
    XSFLIB_CHECK(mixed.size() == 3 && mixed["3sf_track_volume"] == "8=0.8, 9=0.8, 10=0.75" &&
                 mixed["3sf_silence"] == "0xf898" && mixed["volume"] == "0.7");
    XSFLIB_CHECK((SequenceStartOf(mixed) == SequenceStart{.silenced = 0xf898, .track_volumes = volumes}));
    XSFLIB_CHECK((NeedsTimedStarts(mix) && !NeedsTimedStarts(SequenceStart{.silenced = 0xf898, .volume = 0.7f})));
    XSFLIB_CHECK((SequenceStartOf({{"3sf_track_volume", "8=17"}}) == SequenceStart{}));
}

static void TestTrackVariables()
{
    // 3sf_var takes the track variables, 32 to 47, beside the player's and the global ones (docs/3sf.md, "Tags"). Each
    // number is as xsflib::psf::ParseVariables takes it.
    XSFLIB_CHECK((ParseVariables("34=1") == Variables{{34, 1}}));
    XSFLIB_CHECK((ParseVariables(" 0=-1,\n 0x2f=0x7fff 16=-32768") == Variables{{0, -1}, {16, -32768}, {47, 32767}}));
    for (const char* bad : {"", " , ", "48=1", "-1=0", "34=32768", "34", "34=1;"})
    {
        Check(!ParseVariables(bad), std::string("\"") + bad + "\" is refused");
    }

    // Super Mario 3D Land's note melodies start with track variable 2 (34) at 1. The ripper writes it into 3sf_var.
    // A player of 3SF 1.4 would ignore that tag. The set therefore requires bit 4.
    const SequenceStart melody{.variables = {{{.index = 34, .value = 1}}}};
    Tags tags;
    WriteSequenceStart(melody, tags);
    XSFLIB_CHECK(tags.size() == 1 && tags["3sf_var"] == "34=1");
    XSFLIB_CHECK(NeedsTimedStarts(melody));
}

static void TestStartBanksAndVolume()
{
    // 3sf_banks gives one to four bank item IDs for the slots in order. 0xFFFFFFFF leaves a slot empty, and so do the
    // slots after the last ID (docs/3sf.md, "Tags").
    constexpr uint32_t kEmpty = 0xffffffff;
    XSFLIB_CHECK((ParseBanks("0x03000001") == std::array<uint32_t, 4>{0x03000001, kEmpty, kEmpty, kEmpty}));
    XSFLIB_CHECK((ParseBanks(" 0x03000002,\n0xffffffff 50331648 ") ==
                  std::array<uint32_t, 4>{0x03000002, kEmpty, 0x03000000, kEmpty}));
    for (const char* bad : {"", " , ", "0x02000000", "0x04000000", "-0x03000000", "0x03000000;", "x",
                            "0x03000000 0x03000000 0x03000000 0x03000000 0x03000000"})
    {
        Check(!ParseBanks(bad), std::string("\"") + bad + "\" is refused");
    }

    // Super Mario 3D Land starts Mario's voices with Luigi's bank in slot 0. The ripper writes each ID up to the last
    // slot with a bank, as 0x and eight hex digits. A player of 3SF 1.4 would play the archive's banks. The set
    // therefore requires bit 4.
    const SequenceStart luigi{.banks = std::array<uint32_t, 4>{0x03000001, kEmpty, kEmpty, kEmpty}};
    Tags voice;
    WriteSequenceStart(luigi, voice);
    XSFLIB_CHECK(voice.size() == 1 && voice["3sf_banks"] == "0x03000001");
    XSFLIB_CHECK(SequenceStartOf(voice) == luigi && NeedsTimedStarts(luigi));
    Tags gap;
    WriteSequenceStart(SequenceStart{.banks = std::array<uint32_t, 4>{kEmpty, 0x0300000d, kEmpty, kEmpty}}, gap);
    XSFLIB_CHECK(gap["3sf_banks"] == "0xffffffff, 0x0300000d");
    XSFLIB_CHECK(!SequenceStartOf({{"3sf_banks", "0x05000000"}}).banks);

    // The ripper writes a start's volume into the standard volume tag, as the shortest decimal that reads back as the
    // same float. Every player applies that tag to its output, so the volume requires no flag. SequenceStartOf leaves
    // the volume at 1: the player doesn't apply it twice.
    Tags quieter;
    WriteSequenceStart(SequenceStart{.volume = 0.55f}, quieter);
    XSFLIB_CHECK(quieter.size() == 1 && quieter["volume"] == "0.55");
    XSFLIB_CHECK(xsflib::psf::TagVolume(quieter) == 0.55);
    XSFLIB_CHECK(!NeedsTimedStarts(SequenceStart{.volume = 0.55f}) && SequenceStartOf(quieter) == SequenceStart{});
    Tags full;
    WriteSequenceStart(SequenceStart{}, full);
    XSFLIB_CHECK(full.empty());
}

static void TestPlaybackTags()
{
    // Each tag that the ripper writes for a start, and 3sf_aux, sets how a set plays. The plugins keep them when the
    // tags are edited (kPlaybackTags). The volume tag is a PSF tag. The plugins keep it anyway.
    SequenceStart start{.muted = 0x0001,
                        .silenced = 0x0002,
                        .variables = {{{.index = 0, .value = 1}, {.index = 1, .value = 2, .frame = 3}}},
                        .seed = 4,
                        .tempo_ratio = 1.5f,
                        .pitch = 0.5f,
                        .volume = 0.5f,
                        .banks = std::array<uint32_t, 4>{0x03000000, 0xffffffff, 0xffffffff, 0xffffffff},
                        .fx_send = {0.5f, 0.0f}};
    start.track_volumes[0] = 0.5f;
    Tags tags = {{"3sf_aux", "1"}};

    WriteSequenceStart(start, tags);

    XSFLIB_CHECK(tags.size() == kPlaybackTags.size() + 1 && tags.contains("volume"));
    for (const char* name : kPlaybackTags)
    {
        Check(tags.contains(name), std::string(name) + " is a tag that the ripper writes");
    }
}

int main()
{
    TestArchiveMode();
    TestAuxEffects();
    TestDescriptor();
    TestFirmwareRecords();
    TestEffectSets();
    TestGroupFiles();
    TestLoadSet();
    TestPlaybackTags();
    TestSoundsThatPlayTogether();
    TestStartBanksAndVolume();
    TestTimedStarts();
    TestTrackMasks();
    TestTrackVariables();
    TestVersionByte();

    return Summary("format");
}
