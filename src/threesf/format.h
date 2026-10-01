// SPDX-License-Identifier: MIT

// The 3SF file format (docs/3sf.md): PSF container, program chunks (MEM, FILE and SND), process and archive
// descriptors, tags.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace threesf
{

// PSF version byte of 3SF. See docs/3sf.md ("Version byte") before changing it.
constexpr uint8_t kPsfVersion3sf = 0x26;

// The sample rate players report, and the rate the `length` and `fade` tags are counted at (docs/3sf.md, "Tags"). The
// DSP's exact output rate is 268111856 / 8192, about 32728.498 Hz.
constexpr int kPlayerSampleRate = 32728;

// Descriptor flags. Version 1 requires bit 0 (DSP required) and rejects all other bits.
constexpr uint32_t kFlagDspRequired = 1;

// Output modes: 0 mono, 1 stereo, 2 surround. Version 1 plays stereo only.
constexpr uint32_t kOutputStereo = 1;

// Tags from a PSF file's [TAG] section. Names are lower-cased; multi-line values are joined with '\n'.
using Tags = std::map<std::string, std::string>;

// One parsed PSF file.
struct PsfFile
{
    uint8_t version = 0;
    std::vector<uint8_t> reserved;
    std::vector<uint8_t> program; // decompressed
    Tags tags;
};

// Parses a PSF file. Returns an error message on failure. Without `with_program` the program isn't decompressed or
// checked, which is enough to read the version, reserved area and tags.
std::optional<std::string> ParsePsf(const std::vector<uint8_t>& data, PsfFile& out, bool with_program = true);

// Offset of the tag area (the end of the program) of a PSF file, or nullopt if the header is invalid.
std::optional<std::size_t> TagAreaOffset(const std::vector<uint8_t>& data);

// The tag area for `tags`: "[TAG]" and name=value lines (a multi-line value becomes one line per line, all with the
// same name). Empty when there are no tags.
std::string FormatTagArea(const Tags& tags);

// Writes a PSF file, with the program compressed by zlib at its highest level. Throws std::runtime_error if zlib fails,
// which only running out of memory can make it do.
std::vector<uint8_t> WritePsf(uint8_t version, const std::vector<uint8_t>& reserved,
                              const std::vector<uint8_t>& program, const Tags& tags);

// Parses a PSF length or fade tag ("seconds", "m:s" or "h:m:s", with '.' or ',' before decimals) into milliseconds.
// Returns -1 if the tag is empty or malformed.
long long ParseTime(const std::string& text);

// Formats milliseconds as a PSF time ("m:ss.fff").
std::string FormatTime(long long ms);

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
    static constexpr uint32_t kVersion = 1;

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

// A MEM chunk: data to load at `address` in the emulated memory.
struct MemChunk
{
    uint32_t address;
    std::vector<uint8_t> data;
};

// A FILE chunk: a file the set carries, such as the sound archive or the DSP firmware.
struct FileChunk
{
    std::string path;
    std::vector<uint8_t> data;
};

// The parameter block of 3SF's game-mode driver, which a game-mode set loads at kAddress (docs/3sf.md, "Driver").
struct DriverBlock
{
    static constexpr uint32_t kAddress = 0x0e000010;
    static constexpr uint32_t kMagic = 0x44465333; // "3SFD"
    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kVersionOffset = 0x04;
    static constexpr uint32_t kOutputModeOffset = 0x10;
};

// Builds a program (docs/3sf.md, "Program: chunks"): the chunks in the order they're added, each padded to a multiple
// of four bytes.
class ProgramBuilder
{
public:
    void AddMemory(uint32_t address, const void* data, std::size_t size);

    void AddMemory(uint32_t address, const std::vector<uint8_t>& data)
    {
        AddMemory(address, data.data(), data.size());
    }

    void AddFile(const std::string& path, const std::vector<uint8_t>& data);

    // Selects the sound to play in archive mode (`SND ` chunk): its nw::snd item id.
    void AddSound(uint32_t sound_id);

    const std::vector<uint8_t>& Data() const
    {
        return program_;
    }

private:
    void Chunk(const char type[4], const std::vector<uint8_t>& payload);

    std::vector<uint8_t> program_;
};

// A decompressed program split into chunks.
struct ProgramChunks
{
    std::vector<MemChunk> memory;
    std::vector<FileChunk> files;
    std::optional<uint32_t> sound; // the last `SND ` chunk
};

// Splits a decompressed program into chunks (unknown types are skipped).
std::optional<std::string> ParseProgram(const std::vector<uint8_t>& program, ProgramChunks& out);

// A complete set: the chunks of a file and its libraries in loading order, the effective descriptor (exactly one of
// `descriptor` and `archive`) and the tags of the top-level file.
struct LoadedSet
{
    std::vector<MemChunk> memory;
    std::vector<FileChunk> files;
    std::optional<ProcessDescriptor> descriptor; // game mode
    std::optional<ArchiveDescriptor> archive;    // archive mode
    std::optional<uint32_t> sound;               // archive mode: the sound to play
    Tags tags;
    std::vector<std::string> sources; // files loaded, in order (for diagnostics)
};

// Reads a file by path, and returns false if it can't be read.
using FileReader = std::function<bool(const std::string& path, std::vector<uint8_t>& data)>;

// Loads a 3SF/mini3SF file with its _lib/_libN chain. Paths in _lib tags are relative to the directory of the file that
// names them. Returns an error for a set a version 1 player can't play: a descriptor that fails its Check(), or a
// game-mode set whose driver block has another version or an output mode other than stereo.
std::optional<std::string> LoadSet(const std::string& path, const FileReader& reader, LoadedSet& out);

// Reads a whole file from disk.
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& data);

} // namespace threesf
