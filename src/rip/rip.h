// SPDX-License-Identifier: MIT

// 3SF ripper: builds a .3sflib for a game and one .mini3sf per sound, in game mode (the game's code, for games with a
// driver profile) or archive mode (a sound archive and a DSP firmware, played by 3SF's model of the SDK sound player).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "csar/formats.h"
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
    const char* dsp_component;
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

// Game mode: builds the .3sflib. Fails (nullopt, with `error` set) if a file the profile names is missing from the
// RomFS.
std::optional<std::vector<uint8_t>> BuildLibrary(const GameFiles& game, const GameProfile& profile, const Tags& tags,
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
std::vector<std::string> MissingData(const csar::SoundArchive& archive, uint32_t index);

// Filenames for one rip, valid on Windows, macOS and Linux and unique even on case-insensitive filesystems.
class FileNames
{
public:
    // "<base>.3sflib".
    std::string Library(const std::string& base);

    // "<index, four digits> <label>.mini3sf".
    std::string Mini(uint32_t index, const std::string& label);

private:
    std::string Unique(const std::string& stem, const std::string& extension);

    std::set<std::string> taken_; // the names given out so far, in lowercase
};

// A sequence's length and fade in milliseconds, and whether the sequence never makes a sound. A silent sequence gets
// the tail alone.
struct Timing
{
    long long length_ms;
    long long fade_ms;
    bool silent = false;
};

// Works out how long sequences play by running them on the nw::snd model and the DSP firmware. Analyze can be called
// from several threads at once.
class LengthAnalyzer
{
public:
    // Analyzes sequences of `archive_bytes` on `dsp_component`, with `variables` set before each sequence starts (see
    // the 3sf_var tag in docs/3sf.md).
    LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const std::vector<uint8_t>& dsp_component,
                   const Variables& variables = {});
    ~LengthAnalyzer();

    // A looping sequence gets two loops and a 10 s fade, and any other sequence runs to its last sound and a tail of
    // 0.5 s, for at most 600 s. docs/3sf.md, "Lengths", has the rules. Returns nullopt when the length can't be worked
    // out (and then no length tag is written), and throws std::bad_alloc when memory runs out.
    std::optional<Timing> Analyze(uint32_t sound_index) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

// What a walk through a sequence's commands reaches from where the sequence starts: every track it opens and every
// branch, without playing it.
struct SequenceShape
{
    int tracks = 1; // the track it starts on, and one for each track it opens
    int notes = 0;  // note commands, each counted once
};

// Walks `data`, a sequence's commands (the payload of its CSEQ's DATA block), from `start`.
SequenceShape WalkSequence(std::span<const uint8_t> data, uint32_t start);

// Whether a sound's label marks it as music: its first two words (the runs of ASCII letters and digits in it, in any
// case) are SEQ and M, or one of its words starts with BGM, or is JIN, JINGLE or FANFARE with or without a number.
bool LabelMarksMusic(std::string_view label);

// Whether a sound is music, as 3sfrip's --bgm picks: its label marks it as music, or it's a sequence that opens at
// least two tracks besides the one it starts on and has at least 50 notes. The second rule finds the music that a
// label doesn't mark, and the music in archives without labels.
bool IsMusic(const csar::SoundArchive& archive, const csar::SoundInfo& sound);

} // namespace threesf::rip
