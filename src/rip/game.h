// SPDX-License-Identifier: MIT

// A game's files for ripping: the decompressed code.bin, the exheader and the RomFS, from an extracted directory or
// from a decrypted ROM image (.3ds/.cci, .cxi or .cia). Encrypted images are rejected.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace threesf::rip
{

struct GameFiles
{
    uint64_t ProgramId() const;

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
