// SPDX-License-Identifier: MIT

// Unit tests for the 3SF file format code (src/threesf/format.*): the version byte, process descriptor and _lib
// loading. xsflib's tests cover the PSF container, tags and times.

#include "threesf/format.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

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
            for (int b = 0; b < 4; b++)
            {
                block[i * 4 + b] = static_cast<uint8_t>(words[i] >> (8 * b));
            }
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

    // A player only takes version 1, with bit 0 of the flags set and no other, in stereo.
    XSFLIB_CHECK(d.Check().has_value());

    ArchiveDescriptor v1 = d;
    v1.version = 1;
    v1.flags = kFlagDspRequired;
    v1.output_mode = kOutputStereo;

    XSFLIB_CHECK(!v1.Check());

    for (const uint32_t flags : {0u, 3u, 0x80000001u})
    {
        ArchiveDescriptor odd = v1;
        odd.flags = flags;

        XSFLIB_CHECK(odd.Check().has_value());
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

int main()
{
    TestArchiveMode();
    TestDescriptor();
    TestLoadSet();
    TestVersionByte();

    return Summary("format");
}
