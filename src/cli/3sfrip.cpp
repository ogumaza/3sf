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
//                    in its own directory, and a game uses its own.
//   --archive PATH   archive mode on a game: rip only this sound archive (RomFS path)
//   --only REGEX     rip only sounds whose label matches (ECMAScript regex)
//   --bgm            shortcut for --only '^SEQ_BGM'
//   --no-length      don't analyze lengths (no length/fade tags)
//   --jobs N         length analyses run in parallel (default: the number of CPU threads)
//   --game NAME      game tag (default: the profile's name, or else the name of the image, directory or archive)
//   --artist NAME, --year YEAR, --copyright TEXT, --by NAME (3sfby)
//   --version        print the version
//
// The inputs are ripped one after another, and one that fails doesn't stop the rest. Dropping files on the program
// gives it the files as inputs.

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <new>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "cli/console.h"
#include "common/ascii.h"
#include "common/quiet_stdout.h"
#include "csar/formats.h"
#include "rip/rip.h"

using namespace threesf;
namespace fs = std::filesystem;

namespace
{

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    f.close();

    return !f.fail();
}

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
            const auto archive = csar::SoundArchive::Load(bytes);
            std::size_t sequences = 0, wave_sounds = 0, streams = 0;
            for (const auto& s : archive.Sounds())
            {
                sequences += s.type == csar::SoundType::kSequence;
                wave_sounds += s.type == csar::SoundType::kWaveSound;
                streams += s.type == csar::SoundType::kStream;
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

struct Options
{
    std::regex only{".*"};
    std::string only_text; // the pattern --only (or --bgm) gave, or empty
    std::string out_dir, mode, only_archive, game_name, artist, year, copyright, by = "3sfrip";
    bool list = false, lengths = true;
    unsigned jobs = 0;

    // The firmware --firmware names, and a firmware file among the inputs, which only loose sound archives use.
    std::string firmware_file, input_firmware_file;
    std::vector<uint8_t> firmware, input_firmware;
};

Tags CommonTags(const Options& o, const std::string& game_name, const char* mode)
{
    Tags common;
    common["game"] = game_name;
    if (!o.artist.empty())
    {
        common["artist"] = o.artist;
    }

    if (!o.year.empty())
    {
        common["year"] = o.year;
    }

    if (!o.copyright.empty())
    {
        common["copyright"] = o.copyright;
    }

    common["3sfby"] = o.by;
    common["3sf_mode"] = mode;
    common["utf8"] = "1"; // tags are UTF-8 (PSF convention)

    return common;
}

struct Job
{
    uint32_t index;
    std::string label;
    bool sequence;
    std::optional<rip::Timing> timing;
};

// Length analysis runs every selected sequence through the nw::snd model and the DSP firmware; the sounds are
// independent, so they're spread over several threads.
void AnalyzeLengths(std::vector<Job>& jobs, const std::vector<uint8_t>& archive, const std::vector<uint8_t>& firmware,
                    unsigned threads)
{
    const rip::LengthAnalyzer analyzer(archive, firmware);
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<bool> out_of_memory{false};
    const auto worker = [&]
    {
        for (std::size_t i; (i = next++) < jobs.size();)
        {
            if (jobs[i].sequence)
            {
                try
                {
                    jobs[i].timing = analyzer.Analyze(jobs[i].index);
                }
                catch (const std::bad_alloc&)
                {
                    out_of_memory = true;
                    next = jobs.size(); // the other threads take no more jobs
                    return;
                }

                if (!jobs[i].timing)
                {
                    std::fprintf(stderr, "warning: %s gets no length tag: its length analysis failed\n",
                                 jobs[i].label.c_str());
                }
            }

            const std::size_t n = ++done;
            if (n % 100 == 0 || n == jobs.size())
            {
                std::fprintf(stderr, "  analyzed %zu of %zu\n", n, jobs.size());
            }
        }
    };

    threads = std::max(1u, std::min<unsigned>(threads, static_cast<unsigned>(jobs.size())));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; t++)
    {
        // If another thread can't start (the system is out of threads or memory), the ones already going do the work.
        try
        {
            pool.emplace_back(worker);
        }
        catch (const std::exception&)
        {
            break;
        }
    }

    worker();

    for (auto& t : pool)
    {
        t.join();
    }

    if (out_of_memory)
    {
        throw std::runtime_error("out of memory for the length analysis; try fewer --jobs");
    }
}

// Returns the input's absolute path, without the trailing separator of a directory given as "game/" or ".".
fs::path InputPath(const std::string& input)
{
    const fs::path path = fs::absolute(input).lexically_normal();

    return path.has_filename() ? path : path.parent_path();
}

// The name of the game in `input` when no profile gives one: the image's file name without its extension, or the
// directory's name.
std::string NameOfInput(const std::string& input)
{
    const fs::path path = InputPath(input);

    return fs::is_directory(path) ? path.filename().string() : path.stem().string();
}

// Returns the directory that a rip of `input` goes in without -o: one next to the input, named after it with "_3sf"
// added.
fs::path DefaultOutputDir(const std::string& input)
{
    return InputPath(input).parent_path() / (NameOfInput(input) + "_3sf");
}

std::vector<Job> SelectSounds(const csar::SoundArchive& archive, const std::regex& only, bool with_wave_sounds)
{
    std::vector<Job> jobs;
    const auto& sounds = archive.Sounds();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const auto& s = sounds[i];
        const bool sequence = s.type == csar::SoundType::kSequence;
        if (!sequence && !(with_wave_sounds && s.type == csar::SoundType::kWaveSound))
        {
            continue; // streams are out of scope
        }

        if (std::regex_search(s.name, only))
        {
            jobs.push_back({static_cast<uint32_t>(i), s.name, sequence, std::nullopt});
        }
    }

    return jobs;
}

// Writes one .mini3sf per job; `build` makes the file for a sound id and its tags.
int WriteMinis(const std::string& out_dir, rip::FileNames& names, const std::vector<Job>& jobs, const Tags& common,
               const std::function<std::vector<uint8_t>(uint32_t, const Tags&)>& build)
{
    int count = 0;
    for (const Job& job : jobs)
    {
        Tags tags = common;
        tags["title"] = job.label;
        tags["3sf_sound"] = job.label;
        if (job.timing)
        {
            tags["length"] = FormatTime(job.timing->length_ms);
            tags["fade"] = FormatTime(job.timing->fade_ms);
        }

        const std::string name = names.Mini(job.index, job.label);
        if (!WriteFile(fs::path(out_dir) / name, build(0x01000000u | job.index, tags)))
        {
            std::fprintf(stderr, "error: can't write %s\n", name.c_str());
            return -1;
        }

        count++;
        std::fprintf(stderr, "%s%s%s\n", name.c_str(), tags.count("length") ? "  length " : "",
                     tags.count("length") ? tags["length"].c_str() : "");
    }

    return count;
}

// Rips one sound archive in archive mode, adding the number of sounds --only picked to `matched`. Returns the number of
// .mini3sf files written, or -1 on an error.
int RipArchive(const std::string& out_dir, rip::FileNames& names, const std::string& lib_base,
               const std::string& archive_path, const std::vector<uint8_t>& archive_bytes,
               const std::string& firmware_path, const std::vector<uint8_t>& firmware, const std::string& game_name,
               const Options& o, std::size_t& matched)
{
    std::optional<csar::SoundArchive> archive;
    try
    {
        archive = csar::SoundArchive::Load(archive_bytes);
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

    std::vector<Job> jobs = SelectSounds(*archive, o.only, false);
    matched += jobs.size();

    std::size_t wave_sounds = 0;
    for (const auto& s : archive->Sounds())
    {
        wave_sounds += s.type == csar::SoundType::kWaveSound;
    }

    // Sequences whose data isn't all in the archive would play with missing instruments.
    std::vector<Job> complete;
    std::size_t skipped = 0;
    for (Job& job : jobs)
    {
        const auto missing = rip::MissingData(*archive, job.index);
        if (missing.empty())
        {
            complete.push_back(std::move(job));
            continue;
        }

        skipped++;
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
    }

    if (complete.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip (%zu sequences skipped)\n", archive_path.c_str(), skipped);
        return 0;
    }

    const Tags common = CommonTags(o, game_name, "archive");
    const std::string lib_name = names.Library(lib_base);
    const auto lib = rip::BuildArchiveLibrary(archive_path, archive_bytes, firmware_path, firmware, common);
    fs::create_directories(out_dir);
    if (!WriteFile(fs::path(out_dir) / lib_name, lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return -1;
    }

    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib.size());

    if (o.lengths)
    {
        AnalyzeLengths(complete, archive_bytes, firmware, o.jobs);
    }

    const auto build_mini = [&](uint32_t id, const Tags& tags)
    {
        return rip::BuildArchiveMini(id, lib_name, tags);
    };
    const int count = WriteMinis(out_dir, names, complete, common, build_mini);
    if (count >= 0)
    {
        std::fprintf(stderr,
                     "%s: %d mini3sf files; %zu sequences skipped (data outside the archive); "
                     "%zu wave sounds not ripped (archive mode only plays sequences)\n",
                     archive_path.c_str(), count, skipped, wave_sounds);
    }

    return count;
}

// Finds a DSP firmware image among `paths` (read with `read`).
std::optional<std::pair<std::string, std::vector<uint8_t>>> FindFirmware(const std::vector<std::string>& paths,
                                                                         const FileReader& read)
{
    for (const auto& p : paths)
    {
        const std::string lower = AsciiLower(p);
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".cdc") != 0)
        {
            continue;
        }

        std::vector<uint8_t> data;
        if (read(p, data) && rip::IsDspFirmware(data))
        {
            return std::make_pair(p, std::move(data));
        }
    }

    return std::nullopt;
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
    "dropped on the program.\n"
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
    "  --bgm                rip only the BGMs (--only '^SEQ_BGM')\n"
    "  --no-length          don't work out lengths, so the rips get no length or fade tags\n"
    "  --jobs N             the number of length analyses that run at once (default: the number\n"
    "                       of CPU threads)\n"
    "  --game NAME, --artist NAME, --year YEAR, --copyright TEXT, --by NAME\n"
    "                       tags (the game defaults to the profile's name, or else the input's)\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

// Returns the first 0x104 bytes of a file, which tell a sound archive, a 3DS image and a DSP firmware image apart by
// their magic, or an empty string for a directory or a file that can't be read.
std::string ReadHead(const std::string& path)
{
    std::string head;
    if (fs::is_regular_file(path))
    {
        std::ifstream f(path, std::ios::binary);
        head.resize(0x104);
        f.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(f.gcount()));
    }

    return head;
}

// Returns true if `head` starts a DSP firmware image (DSP1).
bool IsFirmwareHead(const std::string& head)
{
    return head.size() >= 0x104 && head.compare(0x100, 4, "DSP1") == 0;
}

// Reports that --only (or --bgm) picked no sound, which is an error: nothing gets written.
int NoSoundMatches(const Options& o)
{
    std::fprintf(stderr, "error: no sound matches '%s'\n", o.only_text.c_str());

    return 1;
}

// Prints the folder a rip went in, as the last line about the input. The macOS droplet opens the folder from it.
void ReportOutput(const fs::path& out_dir)
{
    std::fprintf(stderr, "output: %s\n", fs::absolute(out_dir).lexically_normal().string().c_str());
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

    // The firmware comes from --firmware, then from a firmware file among the inputs, then from the archive's folder.
    std::string firmware_file = o.firmware_file.empty() ? o.input_firmware_file : o.firmware_file;
    std::vector<uint8_t> firmware = o.firmware_file.empty() ? o.input_firmware : o.firmware;
    if (firmware.empty())
    {
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

        if (auto found = FindFirmware(candidates, ReadWholeFile))
        {
            firmware_file = found->first;
            firmware = std::move(found->second);
        }
    }

    // Listing doesn't need the firmware, but it says which one a rip would use.
    if (o.list)
    {
        std::fprintf(stderr, "firmware: %s\n",
                     firmware.empty() ? "none found, so ripping needs one" : firmware_file.c_str());
        std::fprintf(stderr, "%s: a sound archive (archive mode)\n", input.c_str());
        return 0;
    }

    if (firmware.empty())
    {
        std::fprintf(stderr,
                     "error: %s: a sound archive doesn't hold the DSP firmware that plays it. Give a firmware\n"
                     "       file along with the archive (or with --firmware), or put one in the archive's folder:\n"
                     "       any 3DS game's dspaudio.cdc, or the dspfirm.cdc that homebrew dumps from a console.\n"
                     "       The rip keeps a copy of it.\n",
                     file_name.c_str());
        return 1;
    }

    std::fprintf(stderr, "firmware: %s\n", firmware_file.c_str());

    std::fprintf(stderr, "%s", kArchiveModeNotice);

    const std::string game_name = o.game_name.empty() ? NameOfInput(input) : o.game_name;
    std::size_t matched = 0;
    const int count = RipArchive(out_dir.string(), names, game_name, file_name, archive_bytes, kFirmwareName, firmware,
                                 game_name, o, matched);
    if (count < 0)
    {
        return 1;
    }

    if (matched == 0 && !o.only_text.empty())
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
    std::vector<uint8_t> firmware = o.firmware;
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

    const std::string game_name = o.game_name.empty() ? (profile ? profile->name : NameOfInput(input)) : o.game_name;
    int total = 0;
    std::size_t matched = 0;
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
            return 1;
        }

        const std::string lib_base =
            archives.size() == 1 ? game_name : game_name + " " + fs::path(path).stem().string();
        const int n = RipArchive(out_dir.string(), names, lib_base, path, archive_bytes, firmware_path, firmware,
                                 game_name, o, matched);
        if (n < 0)
        {
            return 1;
        }

        total += n;
    }

    if (matched == 0 && !o.only_text.empty())
    {
        return NoSoundMatches(o);
    }

    std::fprintf(stderr, "%s: %d mini3sf files\n", InputPath(input).filename().string().c_str(), total);
    if (total > 0)
    {
        ReportOutput(out_dir);
    }

    return 0;
}

// Rips the archive a game's driver profile names, running the game's sound code. Returns the exit code.
int RipGameInGameMode(const std::string& input, const rip::GameFiles& game, const rip::GameProfile& profile,
                      const fs::path& out_dir, const Options& o, rip::FileNames& names)
{
    const std::string game_name = o.game_name.empty() ? profile.name : o.game_name;
    std::fprintf(stderr, "game: %s\n", profile.name);
    for (const auto& path : game.SoundArchives())
    {
        if (path != profile.archive)
        {
            std::fprintf(stderr, "note: %s isn't ripped (the profile covers %s; --mode archive rips every archive)\n",
                         path.c_str(), profile.archive);
        }
    }

    std::vector<uint8_t> archive_bytes, cdc;
    if (!game.read_romfs(profile.archive, archive_bytes) || !game.read_romfs(profile.dsp_component, cdc))
    {
        std::fprintf(stderr, "error: can't read %s or %s\n", profile.archive, profile.dsp_component);
        return 1;
    }

    const auto archive = csar::SoundArchive::Load(archive_bytes);
    const Tags common = CommonTags(o, game_name, "game");
    const std::string lib_name = names.Library(game_name);
    std::string error;
    const auto lib = rip::BuildLibrary(game, profile, common, &error);
    if (!lib)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    std::vector<Job> jobs = SelectSounds(archive, o.only, true);
    if (jobs.empty() && !o.only_text.empty())
    {
        return NoSoundMatches(o);
    }

    fs::create_directories(out_dir);
    if (!WriteFile(out_dir / lib_name, *lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return 1;
    }

    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib->size());

    if (o.lengths)
    {
        AnalyzeLengths(jobs, archive_bytes, cdc, o.jobs);
    }

    const auto build_mini = [&](uint32_t id, const Tags& tags)
    {
        return rip::BuildMini(id, lib_name, tags);
    };
    const int count = WriteMinis(out_dir.string(), names, jobs, common, build_mini);
    if (count < 0)
    {
        return 1;
    }

    std::fprintf(stderr, "%s: %d mini3sf files\n", InputPath(input).filename().string().c_str(), count);
    ReportOutput(out_dir);

    return 0;
}

// Rips a game image or directory, in game mode when it has a driver profile and archive mode otherwise, unless --mode
// says. Returns the exit code.
int RipGame(const std::string& input, const fs::path& out_dir, const Options& o, rip::FileNames& names)
{
    rip::GameFiles game;
    if (auto err = rip::LoadGame(input, game))
    {
        std::fprintf(stderr, "error: %s\n", err->c_str());
        return 1;
    }

    const uint32_t crc = static_cast<uint32_t>(crc32(0, game.code.data(), static_cast<uInt>(game.code.size())));
    std::fprintf(stderr, "program id %016llx, code.bin %zu bytes, CRC-32 %08x\n",
                 static_cast<unsigned long long>(game.ProgramId()), game.code.size(), crc);
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
    if (mode == "game" && (!o.only_archive.empty() || !o.firmware_file.empty()))
    {
        std::fprintf(stderr, "error: --archive and --firmware are for archive mode (--mode archive)\n");
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

// Reads the options and inputs. Returns 0 to go on, -1 when there's nothing more to do (after --help or --version), or
// the exit code.
int ParseArgs(int argc, char** argv, Options& o, std::vector<std::string>& inputs)
{
    std::string only;
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];

        bool missing_value = false;
        const auto next = [&]() -> std::string
        {
            if (i + 1 >= argc)
            {
                missing_value = true;
                return "";
            }

            return argv[++i];
        };

        if (a == "-h" || a == "--help")
        {
            std::fputs(kUsage, stdout);
            return -1;
        }
        else if (a == "--version")
        {
            std::puts("3sfrip " THREESF_VERSION);
            return -1;
        }
        else if (a == "-o" || a == "--output")
        {
            o.out_dir = next();
        }
        else if (a == "--list")
        {
            o.list = true;
        }
        else if (a == "--mode")
        {
            o.mode = next();
        }
        else if (a == "--firmware")
        {
            o.firmware_file = next();
        }
        else if (a == "--archive")
        {
            o.only_archive = next();
        }
        else if (a == "--only")
        {
            only = next();
        }
        else if (a == "--bgm")
        {
            only = "^SEQ_BGM";
        }
        else if (a == "--no-length")
        {
            o.lengths = false;
        }
        else if (a == "--jobs")
        {
            const std::string v = next();
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || *end || n < 1 || n > 1024)
            {
                std::fprintf(stderr, "error: --jobs takes a number of threads from 1 to 1024, not '%s'\n", v.c_str());
                return 2;
            }

            o.jobs = static_cast<unsigned>(n);
        }
        else if (a == "--game")
        {
            o.game_name = next();
        }
        else if (a == "--artist")
        {
            o.artist = next();
        }
        else if (a == "--year")
        {
            o.year = next();
        }
        else if (a == "--copyright")
        {
            o.copyright = next();
        }
        else if (a == "--by")
        {
            o.by = next();
        }
        else if (a.size() > 1 && a[0] == '-')
        {
            std::fprintf(stderr, "error: unknown option %s (see --help)\n", a.c_str());
            return 2;
        }
        else
        {
            inputs.push_back(a);
        }

        if (missing_value)
        {
            std::fprintf(stderr, "error: %s needs a value\n", a.c_str());
            return 2;
        }
    }

    if (inputs.empty())
    {
        std::fprintf(stderr, "%s", kUsage);
        return 2;
    }

    if (!o.mode.empty() && o.mode != "game" && o.mode != "archive")
    {
        std::fprintf(stderr, "error: --mode is game or archive\n");
        return 2;
    }

    // A pattern that doesn't compile is reported now, before anything is written.
    if (!only.empty())
    {
        try
        {
            o.only = std::regex(only);
            o.only_text = only;
        }
        catch (const std::regex_error& e)
        {
            std::fprintf(stderr, "error: --only: %s\n", e.what());
            return 2;
        }
    }

    if (o.jobs == 0)
    {
        o.jobs = std::max(1u, std::thread::hardware_concurrency());
    }

    return 0;
}

// Takes the DSP firmware files out of the inputs, for the sound archives to use. Returns 0, or the exit code.
int TakeFirmwareInputs(std::vector<std::string>& inputs, Options& o)
{
    const auto is_input = [](const std::string& input)
    {
        return !IsFirmwareHead(ReadHead(input));
    };
    const auto firmware_begin = std::stable_partition(inputs.begin(), inputs.end(), is_input);
    const std::vector<std::string> firmware_files(firmware_begin, inputs.end());
    inputs.erase(firmware_begin, inputs.end());

    if (firmware_files.size() + !o.firmware_file.empty() > 1)
    {
        std::fprintf(stderr, "error: give only one DSP firmware file\n");
        return 2;
    }

    if (!firmware_files.empty())
    {
        o.input_firmware_file = firmware_files[0];
        if (!ReadWholeFile(o.input_firmware_file, o.input_firmware) || !rip::IsDspFirmware(o.input_firmware))
        {
            std::fprintf(stderr, "error: %s isn't a DSP firmware image (DSP1)\n", o.input_firmware_file.c_str());
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
int RipInput(const std::string& input, const Options& o, std::map<fs::path, rip::FileNames>& names)
{
    const std::string head = ReadHead(input);
    if (head.starts_with("FSAR"))
    {
        std::fprintf(stderr,
                     "error: %s is a Wii U or Switch sound archive (FSAR); 3SF rips 3DS sound archives "
                     "(CSAR, .bcsar)\n",
                     input.c_str());
        return 1;
    }

    // Share the filename registry for rips in the same folder to prevent overwrites.
    const fs::path out_dir = o.out_dir.empty() ? DefaultOutputDir(input) : fs::path(o.out_dir);
    rip::FileNames& out_names = names[fs::absolute(out_dir).lexically_normal()];
    if (head.starts_with("CSAR"))
    {
        return RipLooseArchive(input, out_dir, o, out_names);
    }

    return RipGame(input, out_dir, o, out_names);
}

int Run(int argc, char** argv)
{
    Options o;
    std::vector<std::string> inputs;
    if (const int parsed = ParseArgs(argc, argv, o, inputs))
    {
        return parsed < 0 ? 0 : parsed;
    }

    // An input that isn't there stops everything before anything is written. A dropped file is always there, so this
    // catches mistyped paths, and an output given the old way, after the input.
    for (std::size_t i = 0; i < inputs.size(); i++)
    {
        std::error_code ec;
        if (!fs::exists(inputs[i], ec))
        {
            std::fprintf(stderr, "error: %s: no such file or folder%s\n", inputs[i].c_str(),
                         i ? " (-o names the output folder)" : "");
            return 1;
        }
    }

    SilenceStdout(); // Teakra reports unmodelled MMIO accesses on stdout

    if (!o.firmware_file.empty())
    {
        if (!ReadWholeFile(o.firmware_file, o.firmware) || !rip::IsDspFirmware(o.firmware))
        {
            std::fprintf(stderr, "error: %s isn't a DSP firmware image (DSP1)\n", o.firmware_file.c_str());
            return 1;
        }
    }

    if (const int taken = TakeFirmwareInputs(inputs, o))
    {
        return taken;
    }

    // Rip each input once, continuing after errors. Return the highest exit code.
    std::map<fs::path, rip::FileNames> names;
    std::set<fs::path> done;
    int result = 0;
    for (std::size_t i = 0; i < inputs.size(); i++)
    {
        if (inputs.size() > 1)
        {
            std::fprintf(stderr, "%s%s:\n", i ? "\n" : "", InputPath(inputs[i]).filename().string().c_str());
        }

        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(inputs[i], ec);
        if (!done.insert(ec ? InputPath(inputs[i]) : canonical).second)
        {
            std::fprintf(stderr, "skipped, since it's given more than once\n");
            continue;
        }

        try
        {
            result = std::max(result, RipInput(inputs[i], o, names));
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", inputs[i].c_str(), e.what());
            result = std::max(result, 1);
        }
    }

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    SetUpConsole();

    // Report unhandled errors, including allocation failures.
    int result = 1;
    try
    {
        result = Run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
    }

    KeepConsoleOpen();

    return result;
}
