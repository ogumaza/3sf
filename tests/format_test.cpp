// SPDX-License-Identifier: MIT

// Unit tests for the 3SF file format code (src/threesf/format.*): the version byte, the process descriptor, the archive
// descriptor with its group files, _lib loading, sounds that play together, and the tags of a sequence's start.
// aux_effects_test covers the effects on the aux buses and the effect sets. xsflib's tests cover the PSF container,
// tags and times.

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

#include "check_refused.h"

using namespace threesf;
using namespace xsflib::testing;
using threesf_test::CheckRefused;

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
    // stereo. aux_effects_test checks version 3.
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
    CheckRefused(ParseTrackMask, {"", " ", "65536", "0x10000", "-1", "+1", "0x", "0x-1", "1 2", "0xfffg", "ff", "1.0"});

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

// The changes of variables that 3sf_var_change gives.
using Changes = std::vector<SequenceVariable>;

// 3sf_var_change: the changes of variables after the start.
static void TestVariableChanges()
{
    // A change is F:N=V, and the tag holds one or more, separated by commas or white space (docs/3sf.md, "Tags").
    XSFLIB_CHECK((ParseVariableChanges("1:16=1") == Changes{{.index = 16, .value = 1, .frame = 1}}));
    XSFLIB_CHECK((ParseVariableChanges(" 0x10:0=-2,\n 5:31=0x7fff ") ==
                  Changes{{.index = 0, .value = -2, .frame = 16}, {.index = 31, .value = 32767, .frame = 5}}));
    XSFLIB_CHECK((ParseVariableChanges("0:17=-32768") == Changes{{.index = 17, .value = -32768, .frame = 0}}));
    XSFLIB_CHECK((ParseVariableChanges("1:47=0") == Changes{{.index = 47, .value = 0, .frame = 1}}));
    CheckRefused(ParseVariableChanges, {"", " , ", "1:16", "16=1", "1=16:1", "1:48=0", "1:16=32768", "-1:16=1",
                                        "1:16=1, x", "1:-1=0", "4294967296:0=0", "1:16=1;"});
}

// 3sf_seed: the random generator's state.
static void TestSeed()
{
    // A seed is 0 to 0xFFFFFFFF, in decimal or hex.
    XSFLIB_CHECK(ParseSeed(" 0x8d9333a4\n") == 0x8d9333a4u);
    XSFLIB_CHECK(ParseSeed("4294967295") == 0xffffffffu);
    CheckRefused(ParseSeed, {"", "-1", "0x100000000", "1 2", "seed"});
}

// The tags that the ripper writes for a start's variables and seed: 3sf_var, 3sf_var_change and 3sf_seed.
static void TestWrittenVariablesAndSeed()
{
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
}

// 3sf_tempo and 3sf_pitch: the tempo ratio and the pitch.
static void TestTempoAndPitch()
{
    // 3sf_tempo and 3sf_pitch give ratios from 1/16 to 16, as digits with an optional '.' and more digits.
    XSFLIB_CHECK(ParseRatio(" 1.3\n") == 1.3f);
    XSFLIB_CHECK(ParseRatio("1.1625") == 1.1625f);
    XSFLIB_CHECK(ParseRatio("16") == 16.0f && ParseRatio("0.0625") == 0.0625f);
    CheckRefused(ParseRatio, {"", "1.", ".5", "+1", "-1", "1,3", "1e0", "0x1", "16.01", "0.0624", "0", "1 2", "inf"});

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
}

// 3sf_fx_send: the sends on the aux buses.
static void TestFxSends()
{
    // 3sf_fx_send gives the sends on aux buses A and B, each from 0 to 1. A single value gives bus A's.
    using Sends = std::array<float, 2>;
    XSFLIB_CHECK((ParseSends("0.4") == Sends{0.4f, 0.0f} && ParseSends(" 0.4 ,\n0.25 ") == Sends{0.4f, 0.25f}));
    XSFLIB_CHECK((ParseSends("1,0") == Sends{1.0f, 0.0f}));
    CheckRefused(ParseSends, {"", ",", "0.4,", ",0.4", "1.5", "-0.1", "0.4,0.2,0.1", "0.4;0.2", "1e0", "x"});

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
}

// 3sf_track_volume: the volumes of the tracks.
static void TestTrackVolumes()
{
    // 3sf_track_volume gives tracks' volumes as N=V, each from 0 to 16. The tracks that it leaves out play at 1.
    TrackVolumes volumes = ntdlib::ctr::snd::kFullTrackVolumes;
    volumes[8] = 0.8f;
    volumes[9] = 0.8f;
    XSFLIB_CHECK(ParseTrackVolumes("8=0.8, 9=0.8") == volumes && ParseTrackVolumes(" 0x8=0.8\n9=0.8 ") == volumes);
    XSFLIB_CHECK(ParseTrackVolumes("15=0")->at(15) == 0.0f && ParseTrackVolumes("0=16")->at(0) == 16.0f);
    CheckRefused(ParseTrackVolumes,
                 {"", " , ", "8", "8=", "=0.8", "16=1", "-1=1", "8=16.5", "8=-0.1", "8 = 0.8", "8=0.8;9=1", "8=1e0"});

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
    CheckRefused(ParseVariables, {"", " , ", "48=1", "-1=0", "34=32768", "34", "34=1;"});

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
    CheckRefused(ParseBanks, {"", " , ", "0x02000000", "0x04000000", "-0x03000000", "0x03000000;", "x",
                              "0x03000000 0x03000000 0x03000000 0x03000000 0x03000000"});

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
    TestDescriptor();
    TestGroupFiles();
    TestLoadSet();
    TestPlaybackTags();
    TestSoundsThatPlayTogether();
    TestStartBanksAndVolume();
    TestVariableChanges();
    TestSeed();
    TestWrittenVariablesAndSeed();
    TestTempoAndPitch();
    TestFxSends();
    TestTrackVolumes();
    TestTrackMasks();
    TestTrackVariables();
    TestVersionByte();

    return Summary("format");
}
