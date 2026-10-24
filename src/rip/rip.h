// SPDX-License-Identifier: MIT

// 3SF ripper: builds a .3sflib for a game and one .mini3sf per sound, in game mode (the game's code, for games with a
// driver profile) or archive mode (a sound archive and a DSP firmware, played by 3SF's model of the SDK sound player).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "xsflib/psf/program.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/length.h"

#include "nnsnd/aux_effects.h"
#include "rip/game.h"
#include "threesf/format.h"

namespace threesf::rip
{

// A reverb that the driver starts on aux bus A before it plays the sound, as the game does (see driver/driver.c).
// `start` is 0 for a game without one.
struct ReverbProfile
{
    uint32_t manager_pointer; // where the game keeps the pointer to its sound manager object
    uint32_t start;           // start(gfl heap, parameters): starts the reverb on aux bus A
    // nw::snd's FxReverb parameters, as the driver passes them. The block's pointer to its filter sizes (word 5) is
    // left 0. The driver points it at `filter_sizes`.
    std::array<uint32_t, 9> parameters;
    std::array<uint32_t, 3> filter_sizes; // in samples
};

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
    ReverbProfile reverb;
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

// A file of a set: its path and its data.
using FileChunk = xsflib::psf::FileChunk;

// Archive mode: a .3sflib holding the sound archive, the DSP firmware and the group files `groups` beside the archive,
// as FILE chunks at the given paths, with an archive descriptor. The descriptor lists the group files, with
// kFlagGroupFiles, when there are any. It gives `effects` on the aux buses, with kFlagAuxEffects, when there are any.
// The descriptor has version `version`: ArchiveDescriptor::kListVersion when a mini of the set plays several sounds.
std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& firmware_path, const std::vector<uint8_t>& firmware,
                                         const Tags& tags, const std::vector<FileChunk>& groups = {},
                                         uint32_t version = ArchiveDescriptor::kVersion,
                                         const nnsnd::AuxEffectLists& effects = {});

// Archive mode: a .mini3sf that plays `sound_id` (a sequence) from `lib_name`.
std::vector<uint8_t> BuildArchiveMini(uint32_t sound_id, const std::string& lib_name, Tags tags);

// Archive mode: a .mini3sf that plays the sequences `sound_ids` together from `lib_name`, started in that order
// (docs/3sf.md, "Sounds that play together"). The library's descriptor must have ArchiveDescriptor::kListVersion when
// `sound_ids` holds more than one sound. A list of one sound gives the mini of that sound alone.
std::vector<uint8_t> BuildArchiveMini(const std::vector<uint32_t>& sound_ids, const std::string& lib_name, Tags tags);

// A piece of music whose channels a game plays as separate sequences, started together (docs/3sf.md, "Sounds that play
// together"): the piece's name, and the sequences by their indices in the archive, in the order in which the game
// starts them.
struct Piece
{
    std::string name;
    std::vector<uint32_t> sounds;
};

// The pieces of `archive` whose channels are separate sequences. Mighty Gunvolt Burst plays each piece of its music as
// five sequences, one for each channel of the NES's sound chip. It starts them together: the DPCM channel, the noise,
// the two pulse channels and the triangle. Their labels are the piece's name followed by _DPCM, _NOISE, _P1, _P2 and
// _TRI. A piece is five sequences labelled with one name and those suffixes. The pieces come in the order of their
// first sounds.
std::vector<Piece> FindPieces(const ntdlib::sar::SoundArchive& archive);

// A sequence's length and fade in milliseconds, and whether it never makes a sound (xsflib/rip/length.h).
using Timing = xsflib::rip::Timing;

// The timing of sequences that play together, from the timing that the length analysis gives each sequence alone: the
// latest end, and the longest fade. Sequences that never make a sound don't count, unless all are silent. Without the
// timing of one of the sequences, there is none.
std::optional<Timing> TimingOfPiece(const std::vector<std::optional<Timing>>& timings);

// True if `data` is a DSP1 firmware image (such as a game's dspaudio.cdc).
bool IsDspFirmware(const std::vector<uint8_t>& data);

// Lists the files sequence `index` needs that the archive doesn't hold: the sequence's data, banks or wave archives
// kept in other files, or cut off by truncation. Empty when nothing is missing.
std::vector<std::string> MissingData(const ntdlib::sar::SoundArchive& archive, uint32_t index);

// Filenames for one rip, valid on Windows, macOS and Linux and unique even on case-insensitive filesystems
// (xsflib::rip). 3SF names them "<base>.3sflib" and "<index, four digits> <label>.mini3sf".
using FileNames = xsflib::rip::FileNames;

} // namespace threesf::rip
