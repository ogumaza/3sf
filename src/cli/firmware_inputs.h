// SPDX-License-Identifier: MIT

// 3sfrip's DSP firmware: the firmware files among the inputs, and the firmware that a loose sound archive plays with.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/ctr/dsp/dsp.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/tool.h"

#include "cli/rip_jobs.h"
#include "rip/rip.h"

namespace threesf::cli
{

// Finds a DSP firmware image among `paths` (read with `read`).
std::optional<FirmwareFile> FindFirmware(const std::vector<std::string>& paths, const FileReader& read);

// The DSP firmware for a loose sound archive: the file that --firmware names, or else the firmware file among the
// inputs, or else the first firmware file in the archive's folder.
std::optional<FirmwareFile> LooseArchiveFirmware(const std::string& input, const Options& o);

// Formats the firmware location for messages: its RomFS path or offset in code.bin.
std::string FirmwareSource(const rip::GameFirmware& firmware);

// The bytes at the start of an input that tell a sound archive, a 3DS image and a DSP firmware image apart by their
// magic. A DSP1 image has its magic at 0x100.
inline constexpr std::size_t kHeadSize =
    ntdlib::ctr::dsp::Dsp1Image::kMagicOffset + ntdlib::ctr::dsp::Dsp1Image::kMagic.size();

// Returns true if `head` starts a DSP firmware image (DSP1).
bool IsFirmwareHead(const std::string& head);

// Reads the DSP firmware file at `path` into `firmware`. Returns false, after an error message, if it's a folder, can't
// be read or isn't a DSP1 image.
bool ReadFirmwareFile(const std::string& path, std::vector<uint8_t>& firmware);

// Takes the DSP firmware files out of the inputs, for the sound archives to use. Returns 0, or the exit code.
int TakeFirmwareInputs(std::vector<std::string>& inputs, Options& o);

} // namespace threesf::cli
