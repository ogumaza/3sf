// SPDX-License-Identifier: MIT

// Unit tests for the ripper library (src/rip).

#include "rip/rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "common/bytes.h"
#include "rip/game.h"

using namespace threesf;

// A well-formed DSP1 image of 0x310 bytes: the header, then one segment of 0x10 bytes.
static std::vector<uint8_t> DspImage()
{
    std::vector<uint8_t> image(0x310);
    std::memcpy(image.data() + 0x100, "DSP1", 4);
    StoreLe32(image.data() + 0x104, 0x310); // the image's size
    image[0x10e] = 1;                       // the number of segments
    StoreLe32(image.data() + 0x120, 0x300); // the segment's offset in the image
    StoreLe32(image.data() + 0x128, 0x10);  // and its size

    return image;
}

// `code` with `image` copied into it at `offset`.
static std::vector<uint8_t> WithImage(std::vector<uint8_t> code, const std::vector<uint8_t>& image, std::size_t offset)
{
    std::copy(image.begin(), image.end(), code.begin() + static_cast<std::ptrdiff_t>(offset));

    return code;
}

// A game's DSP firmware is the .cdc file its code names, then one called dspaudio.cdc, then the first in the RomFS; a
// profile's firmware comes before them all.
static void TestFirmwareCandidates()
{
    rip::GameFiles game;
    const std::string code("\0\0rom:/snd/game.cdc\0", 20);
    game.code.assign(code.begin(), code.end());
    game.romfs_files = {"a/first.cdc", "b/dspaudio.cdc", "snd/game.cdc", "snd/game.bcsar", "c/other.CDC"};

    THREESF_CHECK((rip::FirmwareCandidates(game, nullptr) ==
                   std::vector<std::string>{"snd/game.cdc", "b/dspaudio.cdc", "a/first.cdc", "c/other.CDC"}));

    game.code.clear();
    THREESF_CHECK((rip::FirmwareCandidates(game, nullptr) ==
                   std::vector<std::string>{"b/dspaudio.cdc", "a/first.cdc", "snd/game.cdc", "c/other.CDC"}));

    rip::GameProfile profile{};
    profile.dsp_component = "sound/x.cdc";
    const auto with_profile = rip::FirmwareCandidates(game, &profile);
    THREESF_CHECK(with_profile.size() == 5 && with_profile.front() == "sound/x.cdc");
}

// A DSP1 image linked into a game's code is found by its header, and a "DSP1" that doesn't start a well-formed image
// inside the code is passed over.
static void TestFirmwareInCode()
{
    const std::vector<uint8_t> image = DspImage();
    std::vector<uint8_t> code(0x1000, 0xee);
    std::memcpy(code.data() + 0x10, "DSP1", 4);                            // too near the start to follow a signature
    std::memcpy(code.data() + 0x180, "DSP1", 4);                           // its "size" is 0xeeeeeeee
    const std::vector<uint8_t> at_end(code.begin(), code.begin() + 0x900); // room for the image at 0x5f0, and no more

    THREESF_CHECK(rip::FindFirmwareInCode(WithImage(code, image, 0x800)) == std::size_t{0x800});
    THREESF_CHECK(rip::FindFirmwareInCode(WithImage(at_end, image, 0x5f0)) == std::size_t{0x5f0});
    THREESF_CHECK(!rip::FindFirmwareInCode(code));
    THREESF_CHECK(!rip::FindFirmwareInCode({}));
    THREESF_CHECK(!rip::FindFirmwareInCode(std::vector<uint8_t>(image.begin(), image.begin() + 0x2ff)));

    // Each of these breaks the image: it's bigger than what's left of the code, it has no segments, a segment runs past
    // its end, or a segment starts inside the header.
    const auto broken = [&](std::size_t field, uint32_t value)
    {
        std::vector<uint8_t> bad = image;
        if (field == 0x10e)
        {
            bad[field] = static_cast<uint8_t>(value);
        }
        else
        {
            StoreLe32(bad.data() + field, value);
        }

        return !rip::FindFirmwareInCode(WithImage(code, bad, 0x800));
    };
    THREESF_CHECK(broken(0x104, 0x801));
    THREESF_CHECK(broken(0x10e, 0));
    THREESF_CHECK(broken(0x128, 0x11));
    THREESF_CHECK(broken(0x120, 0x2ff));

    // So does an eleventh segment, even when it and the other ten are well-formed.
    std::vector<uint8_t> eleven = image;
    eleven.resize(0x340);
    StoreLe32(eleven.data() + 0x104, 0x340);
    eleven[0x10e] = 11;
    for (std::size_t i = 1; i <= 10; i++)
    {
        std::copy_n(image.begin() + 0x120, 0x30, eleven.begin() + static_cast<std::ptrdiff_t>(0x120 + i * 0x30));
    }

    THREESF_CHECK(!rip::FindFirmwareInCode(WithImage(code, eleven, 0x800)));
}

// A game's firmware is the first of its RomFS files that's a DSP1 image, and otherwise the image in its code.
static void TestGameFirmware()
{
    rip::GameFiles game;
    const std::vector<uint8_t> image = DspImage();
    std::vector<uint8_t> file = DspImage();
    file[0x300] = 0x5a; // tells the file from the image in the code
    game.code = WithImage(std::vector<uint8_t>(0x1000), image, 0x400);
    game.romfs_files = {"sound/other.cdc", "sound/dspaudio.cdc"};
    game.read_romfs = [&](const std::string& path, std::vector<uint8_t>& data)
    {
        data = path == "sound/dspaudio.cdc" ? file : std::vector<uint8_t>{1, 2, 3};

        return true;
    };

    const auto from_romfs = rip::FindGameFirmware(game, nullptr);
    game.romfs_files = {"sound/other.cdc"};
    const auto from_code = rip::FindGameFirmware(game, nullptr);
    game.code.assign(0x1000, 0);
    const auto from_neither = rip::FindGameFirmware(game, nullptr);

    THREESF_CHECK(from_romfs && from_romfs->romfs_path == "sound/dspaudio.cdc" && from_romfs->data == file);
    THREESF_CHECK(from_code && from_code->romfs_path.empty() && from_code->code_offset == 0x400u &&
                  from_code->data == image);
    THREESF_CHECK(!from_neither);
}

int main()
{
    TestFirmwareCandidates();
    TestFirmwareInCode();
    TestGameFirmware();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("rip tests passed\n");

    return 0;
}
