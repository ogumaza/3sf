// SPDX-License-Identifier: MIT

// A game's files for ripping: the decompressed code.bin, the exheader and the RomFS, from an extracted directory or
// from a decrypted ROM image (.3ds/.cci, .cxi or .cia). Encrypted images are rejected.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace threesf::rip
{

// The offsets of the fields of a program's extended header that 3SF reads (3dbrew, "NCCH/Extended Header"). The text,
// the rodata and the data each have an address, a size in pages and a size in bytes.
constexpr std::size_t kExheaderFlags = 0x0d; // bit 0: the ExeFS's .code is compressed
constexpr std::size_t kExheaderTextAddress = 0x10;
constexpr std::size_t kExheaderTextPages = 0x14;
constexpr std::size_t kExheaderStackSize = 0x1c;
constexpr std::size_t kExheaderRodataPages = 0x24;
constexpr std::size_t kExheaderDataAddress = 0x30;
constexpr std::size_t kExheaderDataPages = 0x34;
constexpr std::size_t kExheaderDataSize = 0x38;
constexpr std::size_t kExheaderBssSize = 0x3c;
constexpr std::size_t kExheaderProgramId = 0x200;
constexpr std::size_t kExheaderPriority = 0x20f; // the main thread's

struct GameFiles
{
    uint64_t ProgramId() const;

    // The CRC-32 of code.bin. A GameProfile identifies the code of its game by this value.
    uint32_t CodeCrc32() const;

    // RomFS paths of the sound archives (.bcsar).
    std::vector<std::string> SoundArchives() const;

    std::vector<uint8_t> code;     // code.bin, decompressed
    std::vector<uint8_t> exheader; // the extended header (at least 0x400 bytes)

    // Every RomFS file, as a path without a leading '/' ("sound/xy_sound.bcsar").
    std::vector<std::string> romfs_files;

    // Reads a RomFS file by path (as listed in romfs_files).
    std::function<bool(const std::string& path, std::vector<uint8_t>& data)> read_romfs;
};

// An extracted game: <dir>/exefs/code.bin (decompressed or BLZ-compressed), <dir>/exefs/exheader.bin and <dir>/romfs/
std::optional<std::string> LoadGameDirectory(const std::string& dir, GameFiles& out);

// A directory, or a decrypted ROM image: NCSD (.3ds/.cci), NCCH (.cxi) or CIA. The RomFS of an image is read from it on
// demand.
std::optional<std::string> LoadGame(const std::string& path, GameFiles& out);

// Decompresses 3DS "BLZ" (bottom-up LZSS, as used for a compressed .code). Returns nullopt if the data isn't valid BLZ.
std::optional<std::vector<uint8_t>> BlzDecompress(const std::vector<uint8_t>& data);

} // namespace threesf::rip
