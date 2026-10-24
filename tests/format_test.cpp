// SPDX-License-Identifier: MIT

// Unit tests for the 3SF file format code (src/threesf/format.*): the version byte, the descriptors with their group
// files and effects, and _lib loading. xsflib's tests cover the PSF container, tags and times.

#include "threesf/format.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
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

    // A player takes versions 1 and 2, with bit 0 of the flags set, bit 1 for group files and bit 2 for effects, in
    // stereo.
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

    for (const uint32_t version : {0u, 3u})
    {
        ArchiveDescriptor odd = v1;
        odd.version = version;

        XSFLIB_CHECK(odd.Check() == "archive descriptor version " + std::to_string(version) +
                                        "; this player supports versions 1 and 2");
    }

    for (const uint32_t flags : {0u, 2u, 9u, 0x80000001u})
    {
        ArchiveDescriptor odd = v1;
        odd.flags = flags;

        XSFLIB_CHECK(odd.Check() == "the archive descriptor's flags are " + xsflib::Hex(flags) +
                                        "; this player requires bit 0 (DSP required) and supports bits 1 (group "
                                        "files) and 2 (aux effects)");
    }

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
                               "supports kinds 1 to 4");

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

int main()
{
    TestArchiveMode();
    TestAuxEffects();
    TestDescriptor();
    TestGroupFiles();
    TestLoadSet();
    TestSoundsThatPlayTogether();
    TestVersionByte();

    return Summary("format");
}
