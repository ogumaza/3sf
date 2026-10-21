// SPDX-License-Identifier: MIT

// 3sfrip: builds 3SF sets (.3sflib + .mini3sf files) from 3DS games and sound archives.
//
// usage: 3sfrip [options] <input> [<input> ...]
//   <input>          a decrypted ROM image (.3ds/.cci, .cxi or .cia; encrypted images are rejected), an extracted game
//                    directory (exefs/code.bin, exefs/exheader.bin and romfs/), or a sound archive (.bcsar). A DSP
//                    firmware file among the inputs isn't ripped: the sound archives among them use it.
//   -o, --output DIR the directory the rips go in (default: one next to each input, named after it with "_3sf" added)
//   --list           list the sound archives and the game's program ID, then stop
//   --mode game|archive
//                    game mode runs the game's sound code and needs a driver profile for the game
//                    (src/rip/rip.cpp); it's the default when there's one. Archive mode plays a sound archive with
//                    3SF's model of the SDK sound player instead.
//   --firmware FILE  the DSP firmware for archive mode: any 3DS game's dspaudio.cdc, or the dspfirm.cdc that homebrew
//                    dumps from a console. By default, a .bcsar uses a firmware file given along with it or else one
//                    in the archive's directory, and a game uses the firmware the game has.
//   --archive PATH   archive mode on a game: rip only this sound archive (RomFS path)
//   --only REGEX     rip only sounds whose label matches (ECMAScript regex)
//   --bgm            rip only the music (rip::IsMusic), and with --only, only the music whose label matches
//   --no-length      don't analyze lengths (no length/fade tags, and only the sounds at a volume of 0 are found to make
//                    no sound)
//   --var N=V        archive mode: set variable N (0 to 15 the player's, 16 to 31 the global ones) to V before each
//                    sequence starts, and write it in the 3sf_var tag; repeat it for more variables
//   --jobs N         length analyses run in parallel (default: the number of CPU threads, at most 8 in a 32-bit build)
//   --game NAME      game tag (default: the profile's name, or else the name of the image, directory or archive)
//   --artist NAME, --year YEAR, --copyright TEXT, --by NAME (3sfby)
//   --version        print the version
//
// Sequences whose data isn't all in the archive are listed and skipped, and so are the sounds that make no sound: those
// whose volume in the archive is 0, and the sequences that the length analysis finds never sound. The inputs are ripped
// one after another, and one that fails doesn't stop the rest. Dropping files on the program gives it the files as
// inputs.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "xsflib/ascii.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/rip/tool.h"
#include "xsflib/text.h"

#include "common/quiet_stdout.h"
#include "common/sar_options.h"
#include "dsp/dsp.h"
#include "rip/game.h"
#include "rip/length.h"
#include "rip/music.h"
#include "rip/rip.h"
#include "threesf/format.h"

using namespace threesf;
namespace fs = std::filesystem;

namespace
{

using xsflib::rip::InputPath;
using xsflib::rip::NameOfInput;
using xsflib::rip::ReportOutput;
using xsflib::rip::WriteFile;

void ListArchives(const rip::GameFiles& game)
{
    const auto archives = game.SoundArchives();
    std::fprintf(stderr, "%zu sound archive(s):\n", archives.size());
    for (const auto& path : archives)
    {
        std::vector<uint8_t> bytes;
        if (!game.read_romfs(path, bytes))
        {
            std::fprintf(stderr, "  %s (unreadable)\n", path.c_str());
            continue;
        }

        try
        {
            const auto archive = ntdlib::sar::SoundArchive::Load(bytes, kSarOptions);
            std::size_t sequences = 0, wave_sounds = 0, streams = 0;
            for (const auto& s : archive.Sounds())
            {
                sequences += s.type == ntdlib::sar::SoundType::kSequence;
                wave_sounds += s.type == ntdlib::sar::SoundType::kWaveSound;
                streams += s.type == ntdlib::sar::SoundType::kStream;
            }
            std::fprintf(stderr, "  %s: %zu bytes, %zu sequences, %zu wave sounds, %zu streams\n", path.c_str(),
                         bytes.size(), sequences, wave_sounds, streams);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "  %s: %s\n", path.c_str(), e.what());
        }
    }
}

// A DSP firmware file and its bytes.
struct FirmwareFile
{
    std::string path;
    std::vector<uint8_t> bytes;
};

struct Options : xsflib::cli::RipOptions
{
    Variables variables; // --var: set before each sequence starts, and written as the 3sf_var tag

    // The firmware --firmware names, and a firmware file among the inputs, which only loose sound archives use.
    FirmwareFile firmware, input_firmware;
};

struct Job
{
    uint32_t index;
    std::string label;
    bool sequence;
    std::optional<rip::Timing> timing;
};

// Length analysis runs every selected sequence through the nw::snd model and the DSP firmware, with `variables` set
// before each starts. The sounds are independent, and the analysis spreads them over several threads.
void AnalyzeLengths(std::vector<Job>& jobs, const std::vector<uint8_t>& archive, const std::vector<uint8_t>& firmware,
                    const Variables& variables, unsigned threads)
{
    const rip::LengthAnalyzer analyzer(archive, firmware, variables);
    const auto analyze = [&analyzer](Job& job)
    {
        if (!job.sequence)
        {
            return false;
        }

        job.timing = analyzer.Analyze(job.index);

        return true;
    };
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze);
}

// Lists and drops the sequences whose data isn't all in the archive. They would play with missing instruments. Returns
// how many it dropped.
std::size_t DropIncomplete(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive)
{
    const auto incomplete = [&archive](const Job& job)
    {
        const auto missing = rip::MissingData(archive, job.index);
        if (missing.empty())
        {
            return false;
        }

        std::string what;
        for (std::size_t i = 0; i < missing.size() && i < 3; i++)
        {
            what += (i ? ", " : "") + missing[i];
        }

        if (missing.size() > 3)
        {
            what += ", ...";
        }

        std::fprintf(stderr, "skipped %s: not in this archive: %s\n", job.label.c_str(), what.c_str());

        return true;
    };
    return std::erase_if(jobs, incomplete);
}

// Lists and drops the sounds whose volume in the archive is 0. The game's sound library multiplies everything a sound
// plays by that volume. Such a sound therefore makes no sound. Returns how many it dropped.
std::size_t DropMuted(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive)
{
    const auto muted = [&archive](const Job& job)
    {
        if (archive.Sounds()[job.index].volume != 0)
        {
            return false;
        }

        std::fprintf(stderr, "skipped %s: it makes no sound (volume 0)\n", job.label.c_str());

        return true;
    };
    return std::erase_if(jobs, muted);
}

// Lists and drops the sequences that the length analysis found never sound, such as those that play no notes, or play
// them at a volume of 0. Returns how many it dropped.
std::size_t DropUnheard(std::vector<Job>& jobs)
{
    const auto unheard = [](const Job& job)
    {
        if (!job.timing || !job.timing->silent)
        {
            return false;
        }

        std::fprintf(stderr, "skipped %s: it makes no sound\n", job.label.c_str());

        return true;
    };
    return std::erase_if(jobs, unheard);
}

// Drops the sounds that make no sound. DropMuted finds those at a volume of 0. Unless --no-length is given, the length
// analysis of the rest then finds the sequences that never sound. Returns how many it dropped.
std::size_t AnalyzeAndDropSilent(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive,
                                 const std::vector<uint8_t>& archive_bytes, const std::vector<uint8_t>& firmware,
                                 const Variables& variables, const Options& o)
{
    std::size_t silent = DropMuted(jobs, archive);
    if (o.lengths && !jobs.empty())
    {
        AnalyzeLengths(jobs, archive_bytes, firmware, variables, o.jobs);
        silent += DropUnheard(jobs);
    }

    return silent;
}

// "; 3 sounds skipped (2 data outside the archive, 1 no sound)", or nothing when none were skipped.
std::string SkippedText(std::size_t missing_data, std::size_t silent)
{
    return xsflib::cli::SkippedText(missing_data, "data outside the archive", silent, "sound");
}

// Whether --only or --bgm picks sounds, so that picking none is an error.
bool PicksSounds(const Options& o)
{
    return o.bgm || !o.only_text.empty();
}

std::vector<Job> SelectSounds(const ntdlib::sar::SoundArchive& archive, const Options& o, bool with_wave_sounds)
{
    std::vector<Job> jobs;
    const auto& sounds = archive.Sounds();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const auto& s = sounds[i];
        const bool sequence = s.type == ntdlib::sar::SoundType::kSequence;
        if (!sequence && !(with_wave_sounds && s.type == ntdlib::sar::SoundType::kWaveSound))
        {
            continue; // streams are out of scope
        }

        if (std::regex_search(s.name, o.only) && (!o.bgm || rip::IsMusic(archive, s)))
        {
            jobs.push_back({static_cast<uint32_t>(i), s.name, sequence, std::nullopt});
        }
    }

    return jobs;
}

// The item id of a job's sound: the item type of sounds (1) in the top byte, and the sound's index.
uint32_t SoundId(const Job& job)
{
    return ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, job.index);
}

// Writes the .3sflib `lib` as `lib_name` in `out_dir`, which it makes if it doesn't exist. Returns false after an error
// message if it can't.
bool WriteLibrary(const fs::path& out_dir, const std::string& lib_name, const std::vector<uint8_t>& lib)
{
    fs::create_directories(out_dir);
    if (!WriteFile(out_dir / xsflib::PathFromUtf8(lib_name), lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return false;
    }

    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib.size());

    return true;
}

// Rips one sound archive in archive mode, adding the number of sounds --only picked to `matched`. Returns the number of
// .mini3sf files written, or -1 on an error.
int RipArchive(const fs::path& out_dir, rip::FileNames& names, const std::string& lib_base,
               const std::string& archive_path, const std::vector<uint8_t>& archive_bytes,
               const std::string& firmware_path, const std::vector<uint8_t>& firmware, const std::string& game_name,
               const Options& o, std::size_t& matched)
{
    std::optional<ntdlib::sar::SoundArchive> archive;
    try
    {
        archive = ntdlib::sar::SoundArchive::Load(archive_bytes, kSarOptions);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", archive_path.c_str(), e.what());
        return -1;
    }

    if (archive->Truncated())
    {
        std::fprintf(stderr, "warning: %s is truncated: sounds whose data lies past its end are skipped\n",
                     archive_path.c_str());
    }

    std::vector<Job> jobs = SelectSounds(*archive, o, false);
    matched += jobs.size();

    std::size_t wave_sounds = 0;
    for (const auto& s : archive->Sounds())
    {
        wave_sounds += s.type == ntdlib::sar::SoundType::kWaveSound;
    }

    const std::size_t skipped = DropIncomplete(jobs, *archive);
    const std::size_t silent = AnalyzeAndDropSilent(jobs, *archive, archive_bytes, firmware, o.variables, o);
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", archive_path.c_str(), SkippedText(skipped, silent).c_str());
        return 0;
    }

    const Tags common = xsflib::cli::RipTags(o, game_name, "3sf", "archive");
    const std::string lib_name = names.Library(lib_base);
    const auto lib = rip::BuildArchiveLibrary(archive_path, archive_bytes, firmware_path, firmware, common);
    if (!WriteLibrary(out_dir, lib_name, lib))
    {
        return -1;
    }

    Tags mini_tags = common;
    if (!o.variables.empty())
    {
        mini_tags["3sf_var"] = FormatVariables(o.variables);
    }

    const auto build_mini = [&](const Job& job, const Tags& tags)
    {
        return rip::BuildArchiveMini(SoundId(job), lib_name, tags);
    };
    const int count = xsflib::cli::WriteMinis(out_dir, names, jobs, mini_tags, "3sf_sound", build_mini);
    if (count >= 0)
    {
        std::fprintf(stderr, "%s: %d mini3sf files%s; %zu wave sounds not ripped (archive mode only plays sequences)\n",
                     archive_path.c_str(), count, SkippedText(skipped, silent).c_str(), wave_sounds);
    }

    return count;
}

// Finds a DSP firmware image among `paths` (read with `read`).
std::optional<FirmwareFile> FindFirmware(const std::vector<std::string>& paths, const FileReader& read)
{
    for (const auto& p : paths)
    {
        const std::string lower = xsflib::AsciiLower(p);
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".cdc") != 0)
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

// The DSP firmware for a loose sound archive: the file that --firmware names, or else the firmware file among the
// inputs, or else the first firmware file in the archive's folder.
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

// The file name a rip gives the DSP firmware it carries, unless it's a file from the game's RomFS, which keeps its path
// there.
constexpr char kFirmwareName[] = "dspaudio.cdc";

// Formats the firmware location for messages: its RomFS path or offset in code.bin.
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

constexpr char kArchiveModeNotice[] =
    "note: in archive mode, 3SF's model of the SDK sound player plays the sounds instead of the\n"
    "      game's code, and the DSP still runs real firmware. For exact rips of a game with a\n"
    "      driver profile, give 3sfrip the decrypted game.\n";

constexpr char kUsage[] =
    "usage: 3sfrip [options] <input> [<input> ...]\n"
    "\n"
    "Rips each input into a .3sflib and a .mini3sf per sound, in a folder next to it that's named\n"
    "after it with \"_3sf\" added. An input is a decrypted 3DS game (.3ds, .cci, .cxi or .cia), an\n"
    "extracted game folder or a sound archive (.bcsar). A sound archive needs a DSP firmware file\n"
    "(.cdc): give one along with the archive, or put one in the archive's folder. Files can also be\n"
    "dropped on the program. Sounds that make no sound, and sequences whose data isn't all in the\n"
    "archive, are listed and skipped.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list a game's sound archives and program ID, then stop\n"
    "  --mode game|archive  game mode runs the game's sound code and needs a driver profile\n"
    "                       for the game, and it's the default when there's one. Archive mode plays\n"
    "                       the sound archives with 3SF's model of the SDK sound player.\n"
    "  --firmware FILE      the DSP firmware for archive mode: any 3DS game's dspaudio.cdc, or the\n"
    "                       dspfirm.cdc that homebrew dumps from a console\n"
    "  --archive PATH       archive mode on a game: rip only this sound archive (its RomFS path)\n"
    "  --only REGEX         rip only the sounds whose label matches\n"
    "  --bgm                rip only the music: the sounds whose labels mark them as BGM, jingles\n"
    "                       or fanfares, and the sequences with 3 tracks or more and 50 notes or\n"
    "                       more. With --only, the music whose label matches.\n"
    "  --no-length          skip length analysis and omit length and fade tags. The analysis also\n"
    "                       finds the sequences that make no sound; without it, only the sounds at\n"
    "                       a volume of 0 are skipped.\n"
    "  --var N=V            archive mode: set variable N (0-15 the player's, 16-31 the global\n"
    "                       ones) to V before each sequence starts, for sequences that wait for the\n"
    "                       game. N and V can be hex (0x...). Repeat it for more variables.\n"
    "  --jobs N             the number of length analyses that run at once (default: the number\n"
    "                       of CPU threads, and at most 8 in a 32-bit build)\n"
    "  --game NAME, --artist NAME, --year YEAR, --copyright TEXT, --by NAME\n"
    "                       tags (the game defaults to the profile's name, or else the input's)\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

// The bytes at the start of an input that tell a sound archive, a 3DS image and a DSP firmware image apart by their
// magic. A DSP1 image has its magic at 0x100.
constexpr std::size_t kHeadSize = dsp::Dsp1Image::kMagicOffset + dsp::Dsp1Image::kMagic.size();

// Returns true if `head` starts a DSP firmware image (DSP1).
bool IsFirmwareHead(const std::string& head)
{
    return head.size() >= kHeadSize &&
           head.compare(dsp::Dsp1Image::kMagicOffset, dsp::Dsp1Image::kMagic.size(), dsp::Dsp1Image::kMagic) == 0;
}

// Reports that --only or --bgm picked no sound, which is an error: nothing gets written.
int NoSoundMatches(const Options& o)
{
    if (!o.bgm)
    {
        std::fprintf(stderr, "error: no sound matches '%s'\n", o.only_text.c_str());
    }
    else if (o.only_text.empty())
    {
        std::fprintf(stderr, "error: --bgm found no music\n");
    }
    else
    {
        std::fprintf(stderr, "error: --bgm found no music that matches '%s'\n", o.only_text.c_str());
    }

    return 1;
}

// The game tag: the value of --game, or else the name in the profile, or else the name of the input.
std::string GameName(const Options& o, const std::string& input, const rip::GameProfile* profile)
{
    if (!o.game_name.empty())
    {
        return o.game_name;
    }

    return profile ? profile->name : NameOfInput(input);
}

// Rips a loose sound archive in archive mode. Returns the exit code.
int RipLooseArchive(const std::string& input, const fs::path& out_dir, const Options& o, rip::FileNames& names)
{
    const std::string file_name = InputPath(input).filename().string();
    if (o.mode == "game")
    {
        std::fprintf(stderr, "error: %s: game mode needs the game itself, not just its sound archive\n",
                     file_name.c_str());
        return 2;
    }

    if (!o.only_archive.empty())
    {
        std::fprintf(stderr, "error: --archive picks one of a game's sound archives, and %s is one already\n",
                     input.c_str());
        return 2;
    }

    std::vector<uint8_t> archive_bytes;
    if (!ReadWholeFile(input, archive_bytes))
    {
        std::fprintf(stderr, "error: can't read %s\n", input.c_str());
        return 1;
    }

    const std::optional<FirmwareFile> firmware = LooseArchiveFirmware(input, o);

    // Listing doesn't need the firmware, but it says which one a rip would use.
    if (o.list)
    {
        std::fprintf(stderr, "firmware: %s\n",
                     firmware ? firmware->path.c_str() : "none found, so ripping needs --firmware");
        std::fprintf(stderr, "%s: a sound archive (archive mode)\n", input.c_str());
        return 0;
    }

    if (!firmware)
    {
        std::fprintf(stderr,
                     "error: %s: a sound archive doesn't hold the DSP firmware that plays it. Give a firmware\n"
                     "       file along with the archive (or with --firmware), or put one in the archive's folder:\n"
                     "       any 3DS game's dspaudio.cdc, or the dspfirm.cdc that homebrew dumps from a console.\n"
                     "       The rip keeps a copy of it.\n",
                     file_name.c_str());
        return 1;
    }

    std::fprintf(stderr, "firmware: %s\n", firmware->path.c_str());

    std::fprintf(stderr, "%s", kArchiveModeNotice);

    const std::string game_name = GameName(o, input, nullptr);
    std::size_t matched = 0;
    const int count = RipArchive(out_dir, names, game_name, file_name, archive_bytes, kFirmwareName, firmware->bytes,
                                 game_name, o, matched);
    if (count < 0)
    {
        return 1;
    }

    if (matched == 0 && PicksSounds(o))
    {
        return NoSoundMatches(o);
    }

    if (count > 0)
    {
        ReportOutput(out_dir);
    }

    return 0;
}

// Rips every sound archive of a game in archive mode, or the one --archive names. Returns the exit code.
int RipGameInArchiveMode(const std::string& input, const rip::GameFiles& game, const rip::GameProfile* profile,
                         const fs::path& out_dir, const Options& o, rip::FileNames& names)
{
    if (!profile)
    {
        std::fprintf(stderr, "no driver profile for this game: ripping in archive mode\n");
    }

    std::fprintf(stderr, "%s", kArchiveModeNotice);

    std::string firmware_path = kFirmwareName;
    std::vector<uint8_t> firmware = o.firmware.bytes;
    if (firmware.empty())
    {
        if (auto found = rip::FindGameFirmware(game, profile))
        {
            std::fprintf(stderr, "firmware: %s\n", FirmwareSource(*found).c_str());
            if (!found->romfs_path.empty())
            {
                firmware_path = found->romfs_path;
            }

            firmware = std::move(found->data);
        }
    }

    if (firmware.empty())
    {
        std::fprintf(stderr,
                     "error: no DSP firmware (a DSP1 image) in the game's RomFS or code; give one with --firmware\n");
        return 1;
    }

    const auto archives = game.SoundArchives();
    if (!o.only_archive.empty() && std::find(archives.begin(), archives.end(), o.only_archive) == archives.end())
    {
        std::fprintf(stderr, "error: the game has no sound archive %s (--list shows them)\n", o.only_archive.c_str());
        return 1;
    }

    const std::string game_name = GameName(o, input, profile);
    int total = 0;
    std::size_t matched = 0;
    bool failed = false; // an archive that can't be ripped doesn't stop the others
    for (const auto& path : archives)
    {
        if (!o.only_archive.empty() && path != o.only_archive)
        {
            continue;
        }

        std::vector<uint8_t> archive_bytes;
        if (!game.read_romfs(path, archive_bytes))
        {
            std::fprintf(stderr, "error: can't read %s\n", path.c_str());
            failed = true;
            continue;
        }

        const std::string lib_base =
            archives.size() == 1 ? game_name : game_name + " " + fs::path(path).stem().string();
        const int n =
            RipArchive(out_dir, names, lib_base, path, archive_bytes, firmware_path, firmware, game_name, o, matched);
        if (n < 0)
        {
            failed = true;
            continue;
        }

        total += n;
    }

    if (matched == 0 && PicksSounds(o) && !failed)
    {
        return NoSoundMatches(o);
    }

    std::fprintf(stderr, "%s: %d mini3sf files\n", InputPath(input).filename().string().c_str(), total);
    if (total > 0)
    {
        ReportOutput(out_dir);
    }

    return failed ? 1 : 0;
}

// Rips the archive a game's driver profile names, running the game's sound code. Returns the exit code.
int RipGameInGameMode(const std::string& input, const rip::GameFiles& game, const rip::GameProfile& profile,
                      const fs::path& out_dir, const Options& o, rip::FileNames& names)
{
    const std::string game_name = GameName(o, input, &profile);
    std::fprintf(stderr, "game: %s\n", profile.name);
    for (const auto& path : game.SoundArchives())
    {
        if (path != profile.archive)
        {
            std::fprintf(stderr, "note: %s isn't ripped (the profile covers %s; --mode archive rips every archive)\n",
                         path.c_str(), profile.archive);
        }
    }

    std::vector<uint8_t> archive_bytes, firmware;
    if (!game.read_romfs(profile.archive, archive_bytes) || !game.read_romfs(profile.firmware, firmware))
    {
        std::fprintf(stderr, "error: can't read %s or %s\n", profile.archive, profile.firmware);
        return 1;
    }

    const auto archive = ntdlib::sar::SoundArchive::Load(archive_bytes, kSarOptions);
    const Tags common = xsflib::cli::RipTags(o, game_name, "3sf", "game");
    const std::string lib_name = names.Library(game_name);
    std::string error;
    const auto lib = rip::BuildLibrary(game, profile, common, &error);
    if (!lib)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    std::vector<Job> jobs = SelectSounds(archive, o, true);
    if (jobs.empty() && PicksSounds(o))
    {
        return NoSoundMatches(o);
    }

    const std::size_t silent = AnalyzeAndDropSilent(jobs, archive, archive_bytes, firmware, {}, o);
    const std::string input_name = InputPath(input).filename().string();
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", input_name.c_str(), SkippedText(0, silent).c_str());
        return 0;
    }

    if (!WriteLibrary(out_dir, lib_name, *lib))
    {
        return 1;
    }

    const auto build_mini = [&](const Job& job, const Tags& tags)
    {
        return rip::BuildMini(SoundId(job), lib_name, tags);
    };
    const int count = xsflib::cli::WriteMinis(out_dir, names, jobs, common, "3sf_sound", build_mini);
    if (count < 0)
    {
        return 1;
    }

    std::fprintf(stderr, "%s: %d mini3sf files%s\n", input_name.c_str(), count, SkippedText(0, silent).c_str());
    ReportOutput(out_dir);

    return 0;
}

// Rips a game image or directory. Uses game mode if a driver profile exists, otherwise archive mode, unless --mode
// overrides the choice. Returns the exit code.
int RipGame(const std::string& input, const fs::path& out_dir, const Options& o, rip::FileNames& names)
{
    rip::GameFiles game;
    if (auto err = rip::LoadGame(input, game))
    {
        std::fprintf(stderr, "error: %s\n", err->c_str());
        return 1;
    }

    std::fprintf(stderr, "program id %016llx, code.bin %zu bytes, CRC-32 %08x\n",
                 static_cast<unsigned long long>(game.ProgramId()), game.code.size(), game.CodeCrc32());
    const rip::GameProfile* profile = rip::FindProfile(game);
    if (o.list)
    {
        ListArchives(game);
        const auto firmware = rip::FindGameFirmware(game, profile);
        std::fprintf(stderr, "firmware: %s\n",
                     firmware ? FirmwareSource(*firmware).c_str() : "none found, so archive mode needs --firmware");
        if (profile)
        {
            std::fprintf(stderr, "profile: %s (game mode rips %s)\n", profile->name, profile->archive);
        }
        else
        {
            std::fprintf(stderr, "no driver profile for this game: archive mode only\n");
        }

        return 0;
    }

    const std::string mode = o.mode.empty() ? (profile ? "game" : "archive") : o.mode;
    if (mode == "game" && (!o.only_archive.empty() || !o.firmware.path.empty() || !o.variables.empty()))
    {
        std::fprintf(stderr, "error: --archive, --firmware and --var are for archive mode (--mode archive)\n");
        return 2;
    }

    if (mode == "game" && !profile)
    {
        std::fprintf(stderr, "error: no driver profile for this game (known: ");
        for (std::size_t i = 0; i < rip::KnownProfiles().size(); i++)
        {
            std::fprintf(stderr, "%s%s", i ? ", " : "", rip::KnownProfiles()[i].name);
        }
        std::fprintf(stderr, "); --mode archive rips its sound archives instead\n");
        return 1;
    }

    if (mode == "archive")
    {
        return RipGameInArchiveMode(input, game, profile, out_dir, o, names);
    }

    return RipGameInGameMode(input, game, *profile, out_dir, o, names);
}

// Takes 3sfrip's options that the frame doesn't have: --firmware and --var. Returns -1 for another option, 0 when it
// took the option, or 2 after an error.
int ParseOption(const std::string& a, const std::function<std::string()>& next, Options& o)
{
    if (a == "--firmware")
    {
        o.firmware.path = next();
        return 0;
    }

    if (a != "--var")
    {
        return -1;
    }

    const std::string v = next();
    const std::optional<xsflib::Assignment> assignment = xsflib::ParseAssignment(v);
    if (!assignment || assignment->index < 0 || assignment->index > 31 || assignment->value < INT16_MIN ||
        assignment->value > INT16_MAX)
    {
        std::fprintf(stderr, "error: --var takes N=V, with N from 0 to 31 and V from -32768 to 32767, not '%s'\n",
                     v.c_str());
        return 2;
    }

    o.variables[static_cast<int>(assignment->index)] = static_cast<int16_t>(assignment->value);

    return 0;
}

// Reads the DSP firmware file at `path` into `firmware`. Returns false, after an error message, if it's a folder, can't
// be read or isn't a DSP1 image.
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

// Takes the DSP firmware files out of the inputs, for the sound archives to use. Returns 0, or the exit code.
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

// Rips one input. Returns the exit code.
int RipInput(const std::string& input, const Options& o, xsflib::cli::FileNameRegistries& names)
{
    // Other consoles' sound archives, by their magic.
    constexpr std::array<std::pair<const char*, const char*>, 3> kOtherArchives = {{
        {"FSAR", "a Wii U or Switch sound archive (FSAR)"},
        {"RSAR", "a Wii sound archive (RSAR)"},
        {"SDAT", "a DS sound archive (SDAT)"},
    }};
    const std::string head = xsflib::rip::ReadHead(input, kHeadSize);
    for (const auto& [magic, kind] : kOtherArchives)
    {
        if (head.starts_with(magic))
        {
            std::fprintf(stderr, "error: %s is %s; 3SF rips 3DS sound archives (CSAR, .bcsar)\n", input.c_str(), kind);
            return 1;
        }
    }

    const fs::path out_dir = o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_3sf") : fs::path(o.out_dir);
    rip::FileNames& out_names = xsflib::cli::NamesIn(names, out_dir, ".3sflib", ".mini3sf");
    if (head.starts_with("CSAR"))
    {
        return RipLooseArchive(input, out_dir, o, out_names);
    }

    return RipGame(input, out_dir, o, out_names);
}

// Runs before the first input: reads the firmware that --firmware names and takes the firmware files out of the inputs.
// Returns 0, or the exit code.
int BeforeInputs(std::vector<std::string>& inputs, Options& o)
{
    SilenceStdout(); // Teakra reports unmodelled MMIO accesses on stdout

    if (!o.firmware.path.empty() && !ReadFirmwareFile(o.firmware.path, o.firmware.bytes))
    {
        return 1;
    }

    return TakeFirmwareInputs(inputs, o);
}

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::RipTool<Options> tool;
    tool.name = "3sfrip";
    tool.version = THREESF_VERSION;
    tool.usage = kUsage;
    tool.requires_a_value = "needs a value";
    tool.mode_error = "--mode is game or archive";
    // Each analysis emulates 64 MiB of FCRAM. A 32-bit build has 2 GiB of address space and runs at most 8.
    tool.max_jobs_32bit = 8;
    tool.parse_option = ParseOption;
    tool.before_inputs = BeforeInputs;
    tool.rip_input = RipInput;

    return xsflib::cli::RunRipTool(tool, argc, argv);
}
