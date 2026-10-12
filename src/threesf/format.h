// SPDX-License-Identifier: MIT

// The 3SF file format (docs/3sf.md): PSF container, program chunks (MEM, FILE and SND), process and archive
// descriptors, tags.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/program.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/set.h"

namespace threesf
{

// PSF version byte of 3SF. See docs/3sf.md ("Version byte") before changing it.
constexpr uint8_t kPsfVersion3sf = 0x26;

// The sample rate players report, and the rate the `length` and `fade` tags are counted at (docs/3sf.md, "Tags"). The
// DSP's exact output rate is 268111856 / 8192, about 32728.498 Hz.
constexpr int kPlayerSampleRate = 32728;

// Descriptor flags. Version 1 requires bit 0 (DSP required) and rejects all other bits.
constexpr uint32_t kFlagDspRequired = xsflib::psf::kDescriptorFlagDspRequired;

// Output modes: 0 mono, 1 stereo, 2 surround. Version 1 plays stereo only.
constexpr uint32_t kOutputStereo = xsflib::psf::kDescriptorOutputStereo;

// The PSF container that the players share (xsflib::psf): tags, reading and writing PSF files, and the 3sf_var tag's
// variables.
using xsflib::psf::FileReader;
using xsflib::psf::FormatVariables;
using xsflib::psf::ParseVariables;
using xsflib::psf::ReadWholeFile;
using xsflib::psf::Tags;
using xsflib::psf::Variables;
using xsflib::psf::WritePsf;

// The process descriptor: the reserved area of a game-mode set (docs/3sf.md, "Process descriptor").
struct ProcessDescriptor
{
    static constexpr uint32_t kMagic = 0x50465333; // "3SFP"
    static constexpr uint32_t kVersion = 1;

    // The most application memory a 3DS gives a program: 178 MiB, in the New 3DS's largest memory mode.
    static constexpr uint32_t kMaxAppMemory = 0x0B200000;

    static std::optional<ProcessDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    uint32_t version = 1;
    uint32_t entry = 0x100000;
    uint32_t stack_size = 0x4000;
    uint32_t priority = 0x30;
    uint32_t app_memory = 0x04000000;
    uint32_t status_address = 0;
    uint32_t flags = kFlagDspRequired;
};

// Descriptor of an archive-mode set: the sound archive is played by 3SF's model of the SDK sound player on the DSP
// firmware, instead of by the game's code (docs/3sf.md, "Archive mode").
struct ArchiveDescriptor
{
    static constexpr uint32_t kMagic = 0x41465333; // "3SFA"

    static std::optional<ArchiveDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    uint32_t version = 1;
    uint32_t flags = kFlagDspRequired;
    uint32_t output_mode = kOutputStereo;
    std::string archive_path;  // FILE chunk holding the sound archive (.bcsar)
    std::string firmware_path; // FILE chunk holding the DSP firmware (a DSP1 image)
};

// The program builder (xsflib::psf).
using xsflib::psf::ProgramBuilder;

// The parameter block of 3SF's game-mode driver, which a game-mode set loads at kAddress (docs/3sf.md, "Driver").
struct DriverBlock
{
    static constexpr uint32_t kAddress = 0x0e000010;
    static constexpr uint32_t kMagic = 0x44465333; // "3SFD"
    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kVersionOffset = 0x04;
    static constexpr uint32_t kOutputModeOffset = 0x10;
};

// What xsflib::psf's set loader requires of 3SF: its descriptors, version byte and driver block.
struct SetFormat
{
    using ProcessDescriptor = threesf::ProcessDescriptor;
    using ArchiveDescriptor = threesf::ArchiveDescriptor;

    static constexpr uint8_t kPsfVersion = kPsfVersion3sf;
    static constexpr const char* kName = "3SF";
    static constexpr xsflib::psf::DriverBlockLayout kDriverBlock{.address = DriverBlock::kAddress,
                                                                 .magic = DriverBlock::kMagic,
                                                                 .version = DriverBlock::kVersion,
                                                                 .version_offset = DriverBlock::kVersionOffset,
                                                                 .big_endian = false,
                                                                 .output_mode_offset = DriverBlock::kOutputModeOffset,
                                                                 .output_mode = kOutputStereo};
};

// A complete set: the chunks of a file and its libraries in loading order, the effective descriptor (exactly one of
// `descriptor` and `archive`) and the tags of the top-level file.
using LoadedSet = xsflib::psf::LoadedSet<SetFormat>;

// Loads a 3SF/mini3SF file with its _lib/_libN chain. Paths in _lib tags are relative to the directory of the file that
// names them: its path up to the last of `separators`. Returns an error for a set a version 1 player can't play: a
// descriptor that fails its Check(), or a game-mode set whose driver block has another version or an output mode other
// than stereo.
using xsflib::psf::LoadSet;

} // namespace threesf
