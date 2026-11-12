// SPDX-License-Identifier: MIT

// The search for a game's DSP firmware: in its RomFS, or as an image inside its code (see rip.h).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/ctr/dsp/dsp.h"
#include "xsflib/ascii.h"
#include "xsflib/bytes.h"

#include "rip/game.h"
#include "rip/rip.h"

namespace threesf::rip
{
namespace
{

using ntdlib::ctr::dsp::Dsp1Image;

// True if a DSP1 image starts at `start` in `code`, its size and segments lie inside it, and it lies inside `code`.
bool IsDspImageAt(const std::vector<uint8_t>& code, std::size_t start)
{
    if (code.size() - start < Dsp1Image::kHeaderSize)
    {
        return false;
    }

    const uint32_t size = xsflib::LoadLe32(code.data() + start + Dsp1Image::kSizeOffset);
    const unsigned count = code[start + Dsp1Image::kSegmentCountOffset];
    if (size < Dsp1Image::kHeaderSize || size > code.size() - start || count == 0 || count > Dsp1Image::kMaxSegments)
    {
        return false;
    }

    for (unsigned i = 0; i < count; i++)
    {
        const std::size_t record = start + Dsp1Image::kSegmentTableOffset + i * Dsp1Image::kSegmentRecordSize;
        const uint64_t offset = xsflib::LoadLe32(code.data() + record);
        const uint64_t segment_size = xsflib::LoadLe32(code.data() + record + Dsp1Image::kSegmentSizeOffset);
        if (offset < Dsp1Image::kHeaderSize || offset + segment_size > size)
        {
            return false;
        }
    }

    return true;
}

} // namespace

std::vector<std::string> FirmwareCandidates(const GameFiles& game, const GameProfile* profile)
{
    const std::string_view code(reinterpret_cast<const char*>(game.code.data()), game.code.size());
    std::vector<std::string> named, dspaudio, others;
    for (const auto& path : game.romfs_files)
    {
        const std::string lower = xsflib::AsciiLower(path);
        if (!lower.ends_with(".cdc"))
        {
            continue;
        }

        if (code.find("/" + path) != std::string_view::npos)
        {
            named.push_back(path);
        }
        else if (lower == "dspaudio.cdc" || (lower.size() > 13 && lower.ends_with("/dspaudio.cdc")))
        {
            dspaudio.push_back(path);
        }
        else
        {
            others.push_back(path);
        }
    }

    std::vector<std::string> out;
    if (profile)
    {
        out.push_back(profile->firmware);
    }

    for (const auto* list : {&named, &dspaudio, &others})
    {
        out.insert(out.end(), list->begin(), list->end());
    }

    return out;
}

std::optional<std::size_t> FindFirmwareInCode(const std::vector<uint8_t>& code)
{
    const std::string_view bytes(reinterpret_cast<const char*>(code.data()), code.size());
    for (std::size_t magic = bytes.find(Dsp1Image::kMagic, Dsp1Image::kMagicOffset); magic != std::string_view::npos;
         magic = bytes.find(Dsp1Image::kMagic, magic + 1))
    {
        if (IsDspImageAt(code, magic - Dsp1Image::kMagicOffset))
        {
            return magic - Dsp1Image::kMagicOffset;
        }
    }

    return std::nullopt;
}

std::optional<GameFirmware> FindGameFirmware(const GameFiles& game, const GameProfile* profile)
{
    for (const auto& path : FirmwareCandidates(game, profile))
    {
        std::vector<uint8_t> data;
        if (game.read_romfs(path, data) && IsDspFirmware(data))
        {
            return GameFirmware{path, 0, std::move(data)};
        }
    }

    const auto offset = FindFirmwareInCode(game.code);
    if (!offset)
    {
        return std::nullopt;
    }

    const auto begin = game.code.begin() + static_cast<std::ptrdiff_t>(*offset);
    const auto end =
        begin + static_cast<std::ptrdiff_t>(xsflib::LoadLe32(game.code.data() + *offset + Dsp1Image::kSizeOffset));
    return GameFirmware{"", *offset, std::vector<uint8_t>(begin, end)};
}

bool IsDspFirmware(const std::vector<uint8_t>& data)
{
    return data.size() >= Dsp1Image::kHeaderSize &&
           std::memcmp(data.data() + Dsp1Image::kMagicOffset, Dsp1Image::kMagic.data(), Dsp1Image::kMagic.size()) == 0;
}

} // namespace threesf::rip
