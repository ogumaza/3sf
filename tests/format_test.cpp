// SPDX-License-Identifier: MIT

// Unit tests for the 3SF file format code (src/threesf/format.*): PSF container, tags, times, program chunks, process
// descriptor and _lib loading.

#include "threesf/format.h"

#include <zlib.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "common/bytes.h"

using namespace threesf;

static void TestTimes()
{
    THREESF_CHECK(ParseTime("") == -1);
    THREESF_CHECK(ParseTime("abc") == -1);
    THREESF_CHECK(ParseTime("5") == 5000);
    THREESF_CHECK(ParseTime("1.5") == 1500);
    THREESF_CHECK(ParseTime("1,25") == 1250);
    THREESF_CHECK(ParseTime("2:03") == 123000);
    THREESF_CHECK(ParseTime("2:03.5") == 123500);
    THREESF_CHECK(ParseTime("1:00:00") == 3600000);
    THREESF_CHECK(ParseTime(" 0:10.000 ") == 10000);
    THREESF_CHECK(ParseTime("1:2:3:4") == -1);
    THREESF_CHECK(ParseTime("-1") == -1);
    THREESF_CHECK(ParseTime("nan") == -1);
    THREESF_CHECK(ParseTime("inf") == -1);
    THREESF_CHECK(ParseTime("1e300") == -1);
    THREESF_CHECK(ParseTime("99999999999999999999:00") == -1);

    THREESF_CHECK(FormatTime(0) == "0:00.000");
    THREESF_CHECK(FormatTime(65998) == "1:05.998");
    THREESF_CHECK(FormatTime(600000) == "10:00.000");

    for (long long ms : {0LL, 1LL, 999LL, 59999LL, 61001LL, 3599999LL})
    {
        THREESF_CHECK(ParseTime(FormatTime(ms)) == ms);
    }
}

// Numbers in tags: an optional sign, digits with '.' or ',' before decimals, and an optional exponent, with the values
// that std::strtod gives in the C locale. Text after the number is left unread.
static void TestDecimals()
{
    for (const char* text : {"0", "0.5", "1,25", "-1.5", "+2", ".5", "5.", "123.456", "0.000123", "2.718281828459045",
                             "1e3", "1.5E-3", "1e22", "1e-22", "9007199254740992", "0.1", "0.3", "62.226"})
    {
        std::string c_text = text;
        std::replace(c_text.begin(), c_text.end(), ',', '.');

        const auto number = ReadDecimal(text);

        Check(number && number->length == std::strlen(text) && number->value == std::strtod(c_text.c_str(), nullptr),
              std::string(text) + " reads as std::strtod reads it");
    }

    const auto with_unit = ReadDecimal("0.5dB");
    const auto bare_exponent = ReadDecimal("1e+x");
    const auto hex = ReadDecimal("0x10");
    const auto huge = ReadDecimal("1e999");
    const auto tiny = ReadDecimal("1e-999");

    THREESF_CHECK(with_unit && with_unit->value == 0.5 && with_unit->length == 3);
    THREESF_CHECK(bare_exponent && bare_exponent->value == 1 && bare_exponent->length == 1);
    THREESF_CHECK(hex && hex->value == 0 && hex->length == 1);
    THREESF_CHECK(huge && std::isinf(huge->value));
    THREESF_CHECK(tiny && tiny->value == 0);
    for (const char* text : {"", ".", "-", "+.", "e5", " 1", "nan", "inf", "x1"})
    {
        Check(!ReadDecimal(text), std::string("\"") + text + "\" isn't a number");
    }
}

// Under a locale that writes decimals with a comma, std::strtod stops at '.'. Tags still read the same. The check runs
// only where such a locale is installed.
static void TestDecimalsUnderCommaLocale()
{
    const char* comma_locale = nullptr;
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "de-DE", "German_Germany.1252"})
    {
        if (std::setlocale(LC_NUMERIC, name) && std::strtod("0.5", nullptr) == 0)
        {
            comma_locale = name;
            break;
        }
    }

    if (!comma_locale)
    {
        std::setlocale(LC_NUMERIC, "C");
        std::printf("no locale with decimal commas is installed, so tags weren't read under one\n");
        return;
    }

    const auto volume = ReadDecimal("0.5");
    const long long length = ParseTime("1:02.226");
    std::setlocale(LC_NUMERIC, "C");

    Check(volume && volume->value == 0.5, std::string("a volume of 0.5 reads as 0.5 under ") + comma_locale);
    Check(length == 62226, std::string("a length of 1:02.226 reads as 62,226 ms under ") + comma_locale);
}

// The 3sf_var tag: assignments separated by commas or white space, in decimal or hex, with signs, the last one for a
// variable winning. Anything malformed or out of range makes the whole tag unreadable.
static void TestVariables()
{
    THREESF_CHECK((ParseVariables("0=1") == Variables{{0, 1}}));
    THREESF_CHECK((ParseVariables("0=1, 17=-1") == Variables{{0, 1}, {17, -1}}));
    THREESF_CHECK((ParseVariables(" 17=-1\n0=1\r\n") == Variables{{0, 1}, {17, -1}}));
    THREESF_CHECK((ParseVariables("0x1f=0x7fff,31=-0x8000") == Variables{{31, -32768}}));
    THREESF_CHECK((ParseVariables("5=2 5=3") == Variables{{5, 3}}));
    THREESF_CHECK((ParseVariables("0=-32768\t31=32767") == Variables{{0, -32768}, {31, 32767}}));

    for (const char* bad : {"", " , ", "0", "=1", "0=", "0 = 1", "32=1", "-1=0", "0=32768", "0=-32769", "0=1x", "0=+1",
                            "0x=1", "a=1", "0=1;1=2", "0=4294967296"})
    {
        Check(!ParseVariables(bad), std::string("'") + bad + "' isn't read");
    }

    THREESF_CHECK(FormatVariables({{17, -1}, {0, 1}}) == "0=1, 17=-1");
    THREESF_CHECK(
        (ParseVariables(FormatVariables({{0, 1}, {16, -5}, {31, 300}})) == Variables{{0, 1}, {16, -5}, {31, 300}}));
}

static void TestPsfRoundTrip()
{
    std::vector<uint8_t> reserved = {1, 2, 3, 4, 5};
    Tags tags = {{"title", "Test"}, {"comment", "line one\nline two"}, {"_lib", "x.3sflib"}};
    std::vector<uint8_t> program(100000);
    for (std::size_t i = 0; i < program.size(); i++)
    {
        program[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    }

    const auto file = WritePsf(kPsfVersion3sf, reserved, program, tags);
    PsfFile psf;
    const auto err = ParsePsf(file, psf);

    THREESF_CHECK(file.size() > 16 && std::memcmp(file.data(), "PSF", 3) == 0 && file[3] == 0x26);
    THREESF_CHECK(!err);
    THREESF_CHECK(psf.version == kPsfVersion3sf);
    THREESF_CHECK(psf.reserved == reserved);
    THREESF_CHECK(psf.program == program);
    THREESF_CHECK(psf.tags == tags);

    // Corruption of the compressed program is detected by the CRC.
    auto bad = file;
    bad[16 + reserved.size() + 10] ^= 0x55;

    THREESF_CHECK(ParsePsf(bad, psf) == "program CRC mismatch");

    // Truncation.
    auto cut = file;
    cut.resize(20);

    THREESF_CHECK(ParsePsf(cut, psf) == "truncated PSF file");
    THREESF_CHECK(ParsePsf(std::vector<uint8_t>{'P', 'S'}, psf) == "not a PSF file");

    // A program whose zlib stream stops short, with a size and CRC that match what's left of it.
    const uint32_t half = LoadLe32(&file[8]) / 2;
    std::vector<uint8_t> short_stream(file.begin(), file.begin() + 16 + reserved.size() + half);
    StoreLe32(&short_stream[8], half);
    StoreLe32(&short_stream[12], static_cast<uint32_t>(crc32(0, &short_stream[16 + reserved.size()], half)));

    THREESF_CHECK(ParsePsf(short_stream, psf) == "truncated program data");
}

static void TestTagParsing()
{
    const std::string kHeader = std::string("PSF") + static_cast<char>(kPsfVersion3sf) + std::string(12, '\0');
    const std::string kTaggedPsf = kHeader +
                                   "[TAG]Title = Hello \nTITLE2=x\ncomment=a\ncomment=b\nartist=me\ncomment=c\n"
                                   "no equals sign\n=empty name\nlength=1:00\r\n";
    std::vector<uint8_t> data(kTaggedPsf.begin(), kTaggedPsf.end());
    PsfFile psf;

    THREESF_CHECK(!ParsePsf(data, psf));
    THREESF_CHECK(psf.tags["title"] == "Hello");
    THREESF_CHECK(psf.tags["title2"] == "x");
    THREESF_CHECK(psf.tags["comment"] == "c"); // a and b join, and the later c replaces them
    THREESF_CHECK(psf.tags["artist"] == "me");
    THREESF_CHECK(psf.tags["length"] == "1:00");
    THREESF_CHECK(psf.tags.count("") == 0);

    // Consecutive lines with the same name join.
    const std::string kRepeatedTagPsf = kHeader + "[TAG]comment=a\ncomment=b\n";
    std::vector<uint8_t> data2(kRepeatedTagPsf.begin(), kRepeatedTagPsf.end());

    THREESF_CHECK(!ParsePsf(data2, psf));
    THREESF_CHECK(psf.tags["comment"] == "a\nb");
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

    THREESF_CHECK(bytes.size() == 32);
    THREESF_CHECK(std::memcmp(bytes.data(), "3SFP", 4) == 0);
    THREESF_CHECK(back.has_value());
    THREESF_CHECK(back->version == d.version && back->entry == d.entry && back->stack_size == d.stack_size &&
                  back->priority == d.priority && back->app_memory == d.app_memory &&
                  back->status_address == d.status_address && back->flags == d.flags);

    // Data without the magic, or too short, isn't a descriptor.
    THREESF_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(32, 0)).has_value());
    THREESF_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(8, 0)).has_value());
}

static void TestChunks()
{
    ProgramBuilder b;
    constexpr uint8_t kMemoryBytes[] = {1, 2, 3};
    b.AddMemory(0x100000, kMemoryBytes, sizeof(kMemoryBytes));
    b.AddFile("sound/a.bcsar", std::vector<uint8_t>{9, 8, 7, 6, 5});
    b.AddMemory(0x0e00001c, std::vector<uint8_t>{0xe1, 0, 0, 1});

    ProgramChunks chunks;
    const auto err = ParseProgram(b.Data(), chunks);
    const auto& mem = chunks.memory;
    const auto& files = chunks.files;

    THREESF_CHECK(b.Data().size() % 4 == 0);
    THREESF_CHECK(!err);
    THREESF_CHECK(mem.size() == 2 && files.size() == 1);
    THREESF_CHECK(mem[0].address == 0x100000 && mem[0].data == std::vector<uint8_t>(kMemoryBytes, kMemoryBytes + 3));
    THREESF_CHECK(mem[1].address == 0x0e00001c && mem[1].data.size() == 4);
    THREESF_CHECK(files[0].path == "sound/a.bcsar" && files[0].data == (std::vector<uint8_t>{9, 8, 7, 6, 5}));

    // Unknown chunks are skipped.
    std::vector<uint8_t> prog = {'X', 'X', 'X', 'X', 4, 0, 0, 0, 1, 2, 3, 4};
    prog.insert(prog.end(), b.Data().begin(), b.Data().end());
    ProgramChunks skipped;

    THREESF_CHECK(!ParseProgram(prog, skipped));
    THREESF_CHECK(skipped.memory.size() == 2 && skipped.files.size() == 1);

    // A truncated chunk is an error, even when its size is so large that adding it to the position wraps around on a
    // 32-bit host.
    std::vector<uint8_t> trunc = {'M', 'E', 'M', ' ', 100, 0, 0, 0, 1, 2};
    std::vector<uint8_t> huge = {'M', 'E', 'M', ' ', 0xf8, 0xff, 0xff, 0xff, 1, 2, 3, 4};
    ProgramChunks truncated;

    THREESF_CHECK(ParseProgram(trunc, truncated).has_value());
    THREESF_CHECK(ParseProgram(huge, truncated).has_value());
}

// The error LoadSet gives for `path`, or an empty string if the set loads.
static std::string LoadError(const std::string& path, const FileReader& reader)
{
    LoadedSet set;
    const auto err = LoadSet(path, reader, set);

    return err ? *err : std::string();
}

static void TestLoadSet()
{
    // dir/lib.3sflib holds the descriptor and the base memory. dir/sub/x.mini3sf loads it with _lib=../lib.3sflib, and
    // loads extra.3sf from its own directory with _lib2.
    std::map<std::string, std::vector<uint8_t>> fs;
    ProcessDescriptor d;
    d.status_address = 0x1234;

    {
        ProgramBuilder b;
        b.AddMemory(0x1000, std::vector<uint8_t>{1, 1, 1, 1});
        b.AddFile("f", std::vector<uint8_t>{1});
        fs["dir/lib.3sflib"] = WritePsf(kPsfVersion3sf, d.Serialize(), b.Data(), {{"game", "G"}});
    }

    {
        ProgramBuilder b;
        b.AddMemory(0x1000, std::vector<uint8_t>{2});
        fs["dir/sub/x.mini3sf"] =
            WritePsf(kPsfVersion3sf, {}, b.Data(),
                     {{"_lib", "../lib.3sflib"}, {"_lib2", "extra.3sf"}, {"title", "X"}, {"length", "1:00"}});
    }

    {
        ProgramBuilder b;
        b.AddMemory(0x1000, std::vector<uint8_t>{3});
        fs["dir/sub/extra.3sf"] = WritePsf(kPsfVersion3sf, {}, b.Data(), {{"title", "extra"}});
    }

    const FileReader reader = [&](const std::string& path, std::vector<uint8_t>& out)
    {
        auto it = fs.find(std::filesystem::path(path).lexically_normal().generic_string());
        if (it == fs.end())
        {
            return false;
        }

        out = it->second;

        return true;
    };

    LoadedSet set;
    const auto err = LoadSet("dir/sub/x.mini3sf", reader, set);

    THREESF_CHECK(!err);
    if (err || set.sources.size() != 3 || set.memory.size() != 3)
    {
        std::fprintf(stderr, "LoadSet: %s\n", err ? err->c_str() : "wrong result");
        failures++;
        return;
    }

    THREESF_CHECK(set.sources[0] == "dir/sub/../lib.3sflib");
    THREESF_CHECK(set.sources[1] == "dir/sub/x.mini3sf");
    THREESF_CHECK(set.sources[2] == "dir/sub/extra.3sf");
    THREESF_CHECK(set.memory[0].data[0] == 1 && set.memory[1].data[0] == 2 && set.memory[2].data[0] == 3);
    THREESF_CHECK(set.files.size() == 1);
    THREESF_CHECK(set.descriptor && set.descriptor->status_address == 0x1234);
    THREESF_CHECK(set.tags["title"] == "X" && set.tags["length"] == "1:00" && set.tags.count("game") == 0);

    // An empty _lib2 names no library, like an empty _lib, and _lib3 still loads.
    {
        ProgramBuilder b;
        b.AddMemory(0x1000, std::vector<uint8_t>{4});
        fs["dir/sub/v.mini3sf"] =
            WritePsf(kPsfVersion3sf, {}, b.Data(), {{"_lib", "../lib.3sflib"}, {"_lib2", ""}, {"_lib3", "extra.3sf"}});
    }

    LoadedSet gap;
    THREESF_CHECK(!LoadSet("dir/sub/v.mini3sf", reader, gap));
    THREESF_CHECK(gap.sources.size() == 3 && gap.sources[2] == "dir/sub/extra.3sf");

    // A missing library, a wrong PSF version and a set without a descriptor are errors.
    fs["dir/y.mini3sf"] = WritePsf(kPsfVersion3sf, {}, {}, {{"_lib", "missing.3sflib"}});
    fs["dir/z.minincsf"] = WritePsf(0x25, {}, {}, {{"_lib", "lib.3sflib"}});
    fs["dir/w.mini3sf"] = WritePsf(kPsfVersion3sf, {}, {}, {{"title", "w"}});

    THREESF_CHECK(LoadError("dir/y.mini3sf", reader) == "can't read dir/missing.3sflib");
    THREESF_CHECK(LoadError("dir/z.minincsf", reader).find("not a 3SF file") != std::string::npos);
    THREESF_CHECK(LoadError("dir/w.mini3sf", reader).starts_with("no process or archive descriptor"));

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

    THREESF_CHECK(LoadSet("dir/v2.3sflib", reader, set).has_value());
    THREESF_CHECK(LoadSet("dir/none.3sflib", reader, set).has_value());
    THREESF_CHECK(LoadSet("dir/huge.3sflib", reader, set).has_value());
    THREESF_CHECK(!LoadSet("dir/most.3sflib", reader, set));

    // Or flags other than bit 0 alone.
    for (const uint32_t flags : {0u, 3u, 0x80000001u})
    {
        ProcessDescriptor odd;
        odd.flags = flags;
        fs["dir/flags.3sflib"] = WritePsf(kPsfVersion3sf, odd.Serialize(), {}, {});
        THREESF_CHECK(LoadSet("dir/flags.3sflib", reader, set).has_value());
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

    THREESF_CHECK(!LoadSet("dir/stereo.3sflib", reader, set));
    THREESF_CHECK(LoadSet("dir/mono.3sflib", reader, set).has_value());
    THREESF_CHECK(LoadSet("dir/surround.3sflib", reader, set).has_value());
    THREESF_CHECK(LoadSet("dir/driver2.3sflib", reader, set).has_value());
    THREESF_CHECK(LoadSet("dir/patched.mini3sf", reader, set).has_value());

    // A _lib cycle ends with an error instead of recursing forever.
    fs["dir/c1.3sf"] = WritePsf(kPsfVersion3sf, {}, {}, {{"_lib", "c2.3sf"}});
    fs["dir/c2.3sf"] = WritePsf(kPsfVersion3sf, {}, {}, {{"_lib", "c1.3sf"}});

    THREESF_CHECK(LoadError("dir/c1.3sf", reader) == "_lib nesting too deep");

    // So does a set that names more files than a set can have. Each of these names the next one ten times, which would
    // take 10^10 loads.
    for (int level = 0; level < 10; level++)
    {
        Tags tags;
        for (int n = 2; n <= 11; n++)
        {
            tags["_lib" + std::to_string(n)] = "f" + std::to_string(level + 1) + ".3sf";
        }

        fs["dir/f" + std::to_string(level) + ".3sf"] = WritePsf(kPsfVersion3sf, {}, {}, tags);
    }

    fs["dir/f10.3sf"] = WritePsf(kPsfVersion3sf, {}, {}, {});

    THREESF_CHECK(LoadError("dir/f0.3sf", reader) == "more than 64 files in the set");
}

static void TestLoadSetInArchive()
{
    // foobar2000's path for a file in an archive: unpack://zip|<length>|<the archive's path>|<its path in the archive>.
    const std::string archive = "unpack://zip|23|file://C:\\music\\set.zip|";
    std::map<std::string, std::vector<uint8_t>> fs;
    fs[archive + "set.3sflib"] = WritePsf(kPsfVersion3sf, ProcessDescriptor{}.Serialize(), {}, {});
    fs[archive + "x.mini3sf"] = WritePsf(kPsfVersion3sf, {}, {}, {{"_lib", "set.3sflib"}});

    const FileReader reader = [&](const std::string& path, std::vector<uint8_t>& out)
    {
        auto it = fs.find(path);
        if (it == fs.end())
        {
            return false;
        }

        out = it->second;

        return true;
    };

    LoadedSet set;
    const auto err = LoadSet(archive + "x.mini3sf", reader, set, "/\\|");
    const std::string without = LoadError(archive + "x.mini3sf", reader);

    // With | as a separator, the library is found in the archive, and without it, beside the archive.
    THREESF_CHECK(!err && set.sources.size() == 2 && set.sources[0] == archive + "set.3sflib");
    THREESF_CHECK(without == "can't read unpack://zip|23|file://C:\\music\\set.3sflib");
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

    THREESF_CHECK(raw.size() % 4 == 0);
    THREESF_CHECK(back && back->version == d.version && back->flags == d.flags && back->output_mode == d.output_mode &&
                  back->archive_path == d.archive_path && back->firmware_path == d.firmware_path);
    THREESF_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + 18)).has_value());
    THREESF_CHECK(!ProcessDescriptor::Parse(raw).has_value());

    // A player only takes version 1, with bit 0 of the flags set and no other, in stereo.
    THREESF_CHECK(d.Check().has_value());

    ArchiveDescriptor v1 = d;
    v1.version = 1;
    v1.flags = kFlagDspRequired;
    v1.output_mode = kOutputStereo;

    THREESF_CHECK(!v1.Check());

    for (const uint32_t flags : {0u, 3u, 0x80000001u})
    {
        ArchiveDescriptor odd = v1;
        odd.flags = flags;
        THREESF_CHECK(odd.Check().has_value());
    }

    for (const uint32_t mode : {0u, 2u, 3u})
    {
        ArchiveDescriptor odd = v1;
        odd.output_mode = mode;
        THREESF_CHECK(odd.Check().has_value());
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

    const FileReader reader = [&](const std::string& path, std::vector<uint8_t>& out)
    {
        auto it = fs.find(path);
        if (it == fs.end())
        {
            return false;
        }

        out = it->second;

        return true;
    };

    LoadedSet set;

    THREESF_CHECK(!LoadSet("a/x.mini3sf", reader, set));
    THREESF_CHECK(set.archive.has_value() && !set.descriptor.has_value());
    THREESF_CHECK(set.sound == 0x01000007u);
    THREESF_CHECK(set.files.size() == 1 && set.files[0].path == "sound/b.bcsar");

    THREESF_CHECK(!LoadSet("a/y.mini3sf", reader, set));
    THREESF_CHECK(set.descriptor.has_value() && !set.archive.has_value());

    THREESF_CHECK(LoadSet("a/z.mini3sf", reader, set).has_value()); // archive mode without a sound
}

int main()
{
    TestTimes();
    TestDecimals();
    TestDecimalsUnderCommaLocale();
    TestVariables();
    TestArchiveMode();
    TestPsfRoundTrip();
    TestTagParsing();
    TestDescriptor();
    TestChunks();
    TestLoadSet();
    TestLoadSetInArchive();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("format tests passed\n");

    return 0;
}
