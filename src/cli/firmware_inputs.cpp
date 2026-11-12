// SPDX-License-Identifier: MIT

// 3sfrip's DSP firmware (see firmware_inputs.h).

#include "cli/firmware_inputs.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/ctr/dsp/dsp.h"
#include "xsflib/ascii.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/tool.h"

#include "cli/rip_jobs.h"
#include "rip/rip.h"

namespace threesf::cli
{

namespace fs = std::filesystem;

using ntdlib::ctr::dsp::Dsp1Image;
using xsflib::rip::InputPath;

std::optional<FirmwareFile> FindFirmware(const std::vector<std::string>& paths, const FileReader& read)
{
    for (const auto& p : paths)
    {
        const std::string lower = xsflib::AsciiLower(p);
        if (!lower.ends_with(".cdc"))
        {
            continue;
        }

        std::vector<uint8_t> data;
        if (read(p, data) && rip::IsDspFirmware(data))
        {
            return FirmwareFile{p, std::move(data)};
        }
    }

    return std::nullopt;
}

std::optional<FirmwareFile> LooseArchiveFirmware(const std::string& input, const Options& o)
{
    if (!o.firmware.path.empty())
    {
        return o.firmware;
    }

    if (!o.input_firmware.path.empty())
    {
        return o.input_firmware;
    }

    std::vector<std::string> candidates;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(InputPath(input).parent_path(), ec))
    {
        if (e.is_regular_file())
        {
            candidates.push_back(e.path().string());
        }
    }

    std::sort(candidates.begin(), candidates.end());

    return FindFirmware(candidates, ReadWholeFile);
}

std::string FirmwareSource(const rip::GameFirmware& firmware)
{
    if (!firmware.romfs_path.empty())
    {
        return "rom:/" + firmware.romfs_path;
    }

    char offset[32];
    std::snprintf(offset, sizeof offset, "%#zx", firmware.code_offset);

    return std::string("in the game's code (code.bin offset ") + offset + ")";
}

bool IsFirmwareHead(const std::string& head)
{
    return head.size() >= kHeadSize &&
           head.compare(Dsp1Image::kMagicOffset, Dsp1Image::kMagic.size(), Dsp1Image::kMagic) == 0;
}

bool ReadFirmwareFile(const std::string& path, std::vector<uint8_t>& firmware)
{
    std::error_code ec;
    if (fs::is_directory(path, ec))
    {
        std::fprintf(stderr, "error: %s is a folder, not a DSP firmware file such as a game's dspaudio.cdc\n",
                     path.c_str());
        return false;
    }

    if (!ReadWholeFile(path, firmware))
    {
        std::fprintf(stderr, "error: can't read %s\n", path.c_str());
        return false;
    }

    if (!rip::IsDspFirmware(firmware))
    {
        std::fprintf(stderr, "error: %s isn't a DSP firmware image (DSP1)\n", path.c_str());
        return false;
    }

    return true;
}

int TakeFirmwareInputs(std::vector<std::string>& inputs, Options& o)
{
    const auto is_input = [](const std::string& input)
    {
        return !IsFirmwareHead(xsflib::rip::ReadHead(input, kHeadSize));
    };
    const auto firmware_begin = std::stable_partition(inputs.begin(), inputs.end(), is_input);
    const std::vector<std::string> firmware_files(firmware_begin, inputs.end());
    inputs.erase(firmware_begin, inputs.end());

    if (firmware_files.size() + !o.firmware.path.empty() > 1)
    {
        std::fprintf(stderr, "error: give only one DSP firmware file\n");
        return 2;
    }

    if (!firmware_files.empty())
    {
        o.input_firmware.path = firmware_files[0];
        if (!ReadFirmwareFile(o.input_firmware.path, o.input_firmware.bytes))
        {
            return 1;
        }
    }

    if (inputs.empty())
    {
        std::fprintf(stderr,
                     "error: %s is a DSP firmware file, which only plays sound archives; give it together "
                     "with the archives\n",
                     firmware_files[0].c_str());
        return 2;
    }

    return 0;
}

} // namespace threesf::cli
