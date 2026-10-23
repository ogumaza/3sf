// SPDX-License-Identifier: MIT

// 3SF ripper: builds a .3sflib for a game and one .mini3sf per sound, in game mode (the game's code, for games with a
// driver profile) or archive mode (a sound archive and a DSP firmware, played by 3SF's model of the SDK sound player).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "xsflib/rip/file_names.h"

#include "rip/game.h"
#include "threesf/format.h"

namespace threesf::rip
{

// Game-specific addresses for the driver (see driver/driver.c).
struct GameProfile
{
    const char* name;
    uint64_t program_id;
    uint32_t code_crc32; // CRC-32 of the decompressed code.bin
    uint32_t nnmain;     // patched to branch to the driver
    uint32_t fs_init, rom_size, rom_mount;
    uint32_t sound_create, set_output, heap_create, load_data, play, update;
    uint32_t heap_manager, system_offset, handles_offset;
    uint32_t heap_base, heap_size, device_base, device_size;
    uint32_t handle;         // sound handle slot the driver plays on
    uint32_t data_heap_size; // device memory for one sound's data
    const char* archive;     // RomFS path of the sound archive
    const char* firmware;    // RomFS path of the DSP firmware
};

const std::vector<GameProfile>& KnownProfiles();

// The profile for a game's code, or nullptr if it isn't supported.
const GameProfile* FindProfile(const GameFiles& game);

// The RomFS paths that may hold the game's DSP firmware, best first: the profile's, if there is one, then the .cdc
// files whose paths the game's code names (such as "rom:/sound/dspaudio.cdc"), then any called dspaudio.cdc, then the
// other .cdc files in RomFS order. The first that's a DSP1 image is the game's firmware.
std::vector<std::string> FirmwareCandidates(const GameFiles& game, const GameProfile* profile);

// Finds the offset of a DSP1 image in `code` (decompressed code.bin), or returns nullopt. The whole image must fit
// inside `code`, and every segment must fit inside the image.
std::optional<std::size_t> FindFirmwareInCode(const std::vector<uint8_t>& code);

// A game's DSP firmware: a file in its RomFS, or an image linked into its code.
struct GameFirmware
{
    std::string romfs_path;  // empty for an image in the code
    std::size_t code_offset; // where the image starts in code.bin, for one in the code
    std::vector<uint8_t> data;
};

// Finds a game's DSP firmware: the first of FirmwareCandidates that's a DSP1 image, or else an image linked into the
// game's code. The RomFS comes first because a game can have an image in its code that it doesn't use: Pokemon X has
// one, and loads rom:/sound/dspaudio.cdc. Returns nullopt if the game has neither.
std::optional<GameFirmware> FindGameFirmware(const GameFiles& game, const GameProfile* profile);

// Game mode: builds the .3sflib that plays the sounds at `sounds`, by their indices in the profile's sound archive. In
// the library's copy of the archive, only the files that those sounds play keep their bytes. The rest of the FILE block
// is zeros. The game reads the same bytes in the same places (see LoadedArchive in rip.cpp). Fails (nullopt, with
// `error` set) if a file the profile names is missing from the RomFS.
std::optional<std::vector<uint8_t>> BuildLibrary(const GameFiles& game, const GameProfile& profile,
                                                 const std::vector<uint32_t>& sounds, const Tags& tags,
                                                 std::string* error = nullptr);

// Game mode: a .mini3sf that plays `sound_id` from `lib_name`.
std::vector<uint8_t> BuildMini(uint32_t sound_id, const std::string& lib_name, Tags tags);

// Archive mode: a .3sflib holding the sound archive and the DSP firmware as FILE chunks at the given paths, with an
// archive descriptor.
std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& firmware_path, const std::vector<uint8_t>& firmware,
                                         const Tags& tags);

// Archive mode: a .mini3sf that plays `sound_id` (a sequence) from `lib_name`.
std::vector<uint8_t> BuildArchiveMini(uint32_t sound_id, const std::string& lib_name, Tags tags);

// True if `data` is a DSP1 firmware image (such as a game's dspaudio.cdc).
bool IsDspFirmware(const std::vector<uint8_t>& data);

// Lists the files sequence `index` needs that the archive doesn't hold: the sequence's data, banks or wave archives
// kept in other files, or cut off by truncation. Empty when nothing is missing.
std::vector<std::string> MissingData(const ntdlib::sar::SoundArchive& archive, uint32_t index);

// Filenames for one rip, valid on Windows, macOS and Linux and unique even on case-insensitive filesystems
// (xsflib::rip). 3SF names them "<base>.3sflib" and "<index, four digits> <label>.mini3sf".
using FileNames = xsflib::rip::FileNames;

} // namespace threesf::rip
