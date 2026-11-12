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
// A sound archive can keep groups of its files outside it, in group files (.bcgrp) that the game loads itself. Archive
// mode finds them by their names in a game's RomFS, or in a loose archive's folder and the folders below it. A rip
// keeps the copies in them that its sequences play. A piece of music whose channels the game plays as separate
// sequences makes one .mini3sf. It starts them together (docs/3sf.md, "Sounds that play together").
//
// Sequences whose data isn't all in the archive and its group files are listed and skipped, and so are the sounds that
// make no sound: those whose volume in the archive is 0, and the sequences that the length analysis finds never sound.
// The inputs are ripped one after another, and one that fails doesn't stop the rest. Dropping files on the program
// gives it the files as inputs.
//
// Some games' code changes a sequence as it starts. It quiets tracks or sets variables, the tempo, the pitch, the banks
// or the sends. Archive mode finds those games by the SHA-1 of the sound archive and the sounds by their labels
// (ntdlib::ctr::snd::StartingSequenceTracks). It writes the start in the mini's tags (WriteSequenceStart), and the
// length analysis plays the sequence so.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/ctr/dsp/quiet_stdout.h"
#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/rip/tool.h"
#include "xsflib/sha1.h"
#include "xsflib/text.h"

#include "cli/firmware_inputs.h"
#include "cli/rip_jobs.h"
#include "nnsnd/aux_effects.h"
#include "rip/effects.h"
#include "rip/game.h"
#include "rip/length.h"
#include "rip/music.h"
#include "rip/rip.h"
#include "threesf/format.h"

using namespace threesf;
using namespace threesf::cli;
namespace fs = std::filesystem;

namespace
{

using ntdlib::ctr::kSarOptions;
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

// The files that a library holds of a sound archive and its group files (see KeepLibraryFiles).
struct LibraryFiles
{
    std::vector<uint8_t> archive;
    std::vector<rip::FileChunk> groups;
};

// The part of `archive` that the library of `jobs` holds: the files that their sequences play, with the banks that
// their starts give them (StartOf, by the archive's SHA-1 `archive_sha1`), and the group files of `groups` that hold
// copies of them, cut down to those copies (ntdlib::rip::KeepSequenceData and KeepGroupCopies). `archive` is the
// archive of `archive_file`, with the group files of `groups` added. Archive mode's model reads nothing else. After a
// warning, the library holds the whole archive and every group file when the files can't be cut out of the archive. It
// holds the whole group files that hold copies when only the copies can't be cut out of them.
LibraryFiles KeepLibraryFiles(const ntdlib::sar::SoundArchive& archive, const rip::FileChunk& archive_file,
                              const std::vector<rip::FileChunk>& groups, const std::vector<Job>& jobs,
                              std::string_view archive_sha1)
{
    std::vector<uint32_t> sounds;
    std::vector<uint32_t> start_banks;
    for (const Job& job : jobs)
    {
        const std::vector<uint32_t> played = job.Sounds();
        sounds.insert(sounds.end(), played.begin(), played.end());
        for (const uint32_t sound : played)
        {
            if (const SequenceStart start = StartOf(job, archive, archive_sha1, sound); start.banks)
            {
                start_banks.insert(start_banks.end(), start.banks->begin(), start.banks->end());
            }
        }
    }

    std::optional<ntdlib::rip::LibraryArchive> kept = ntdlib::rip::KeepSequenceData(archive, sounds, start_banks);
    if (!kept)
    {
        std::fprintf(stderr,
                     "warning: the files that the sequences play can't be cut out of %s. The library holds the whole "
                     "archive\n",
                     archive_file.path.c_str());
        return {archive_file.data, groups};
    }

    const std::optional<std::vector<std::vector<uint8_t>>> cut =
        ntdlib::rip::KeepGroupCopies(archive, *kept, sounds, start_banks);
    if (!cut)
    {
        std::fprintf(stderr,
                     "warning: the copies that the sequences play can't be cut out of the group files. The library "
                     "holds the group files that hold them whole\n");
    }

    LibraryFiles files{std::move(kept->archive), {}};
    for (std::size_t i = 0; i < kept->group_files.size(); i++)
    {
        const rip::FileChunk& group = groups[kept->group_files[i]];
        files.groups.push_back({group.path, cut ? (*cut)[i] : group.data});
    }

    return files;
}

// Loads the sound archive `archive_file` and adds the group files `groups` to it. Returns nothing after an error
// message if either can't be read. A truncated archive gets a warning.
std::optional<ntdlib::sar::SoundArchive> LoadArchive(const rip::FileChunk& archive_file,
                                                     const std::vector<rip::FileChunk>& groups)
{
    std::optional<ntdlib::sar::SoundArchive> archive;
    try
    {
        archive = ntdlib::sar::SoundArchive::Load(archive_file.data, kSarOptions);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", archive_file.path.c_str(), e.what());
        return std::nullopt;
    }

    if (archive->Truncated())
    {
        std::fprintf(stderr, "warning: %s is truncated: sounds whose data lies past its end are skipped\n",
                     archive_file.path.c_str());
    }

    for (const rip::FileChunk& group : groups)
    {
        try
        {
            archive->AddGroupFile(group.data);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", group.path.c_str(), e.what());
            return std::nullopt;
        }
    }

    return archive;
}

// The entry of a known archive, found by its SHA-1 `sha1`, or nothing. A known archive has its game's effects. They
// take the place of those of the game that holds the archive. Another build of a known archive gets a note. The tables
// of effects don't hold its settings.
const rip::ArchiveEffects* FindKnownArchive(const ntdlib::sar::SoundArchive& archive, const std::string& archive_path,
                                            const std::string& sha1)
{
    const rip::ArchiveEffects* known = rip::FindArchiveEffects(sha1);
    if (known)
    {
        std::fprintf(stderr, "aux effects of %s: %s\n", known->game, rip::DescribeEffects(known->buses).c_str());
    }
    else if (const rip::ArchiveEffects* other = rip::FindOtherBuild(archive, sha1))
    {
        std::fprintf(stderr,
                     "note: %s looks like the sound archive of %s, but not the build that 3sfrip knows (%s). Its rips "
                     "play without that game's effects. A sequence that 3sfrip has no start for in this build plays "
                     "every track\n",
                     archive_path.c_str(), other->game, other->build);
    }

    return known;
}

// What an archive's library declares in its descriptor: the version, whether minis start sequences in later frames,
// and the effect sets.
struct DescriptorChoice
{
    uint32_t version = ArchiveDescriptor::kVersion;
    bool timed_starts = false;
    std::vector<nnsnd::AuxEffectLists> effect_sets;
};

// A set whose minis play several sounds gets version 2 of the archive descriptor. Players of 3SF 1.3.3 and older refuse
// it. A set whose minis change a variable in a later frame, set a track variable, set the random generator, set a tempo
// ratio or pitch, give a sequence other banks, give sends or set tracks' volumes gets flag bit 4 (NeedsTimedStarts).
// Players of 3SF 1.4 refuse it. A set with a sequence that the game plays with an effect for some sequences alone gets
// the game's effects as effect sets, in version 3 (docs/3sf.md, "Effect sets").
DescriptorChoice ChooseDescriptor(const std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive,
                                  const ArchiveData& data)
{
    DescriptorChoice choice;
    bool sets = false;
    for (const Job& job : jobs)
    {
        if (job.sounds.size() > 1)
        {
            choice.version = ArchiveDescriptor::kListVersion;
        }

        for (const uint32_t sound : job.Sounds())
        {
            const SequenceStart start = StartOf(job, archive, data.archive_sha1, sound);
            choice.timed_starts = choice.timed_starts || NeedsTimedStarts(start);
            sets = sets || data.EffectSet(start) != 0;
        }
    }

    if (sets)
    {
        for (const rip::SequenceEffect& game_effect : data.known->game_effects)
        {
            choice.effect_sets.push_back(game_effect.buses);
        }
    }

    return choice;
}

// The mini of `job` in archive mode, with `tags` and the library `lib_name`. The 3sf_sound tag of a piece lists the
// labels of its sequences. The tag of a later state holds the sequence's label, without the state's suffix. The
// 3sf_mute and 3sf_silence tags apply to every sequence of a piece, and so does the 3sf_aux tag of the effect set. A
// piece whose sequences start in different ways gets none of them.
std::vector<uint8_t> BuildJobMini(const Job& job, Tags tags, const ntdlib::sar::SoundArchive& archive,
                                  const ArchiveData& data, const std::string& archive_path, const std::string& lib_name)
{
    if (job.sounds.size() > 1 || job.start)
    {
        tags["3sf_sound"] = SoundLabels(archive, job);
    }

    const std::vector<uint32_t> sounds = job.Sounds();
    const SequenceStart start = StartOf(job, archive, data.archive_sha1, sounds.front());
    const auto same = [&](uint32_t sound)
    {
        return StartOf(job, archive, data.archive_sha1, sound) == start;
    };
    if (std::all_of(sounds.begin(), sounds.end(), same))
    {
        WriteSequenceStart(start, tags);
        if (const uint32_t set = data.EffectSet(start); set != 0)
        {
            tags["3sf_aux"] = std::to_string(set);
        }
    }
    else
    {
        std::fprintf(stderr,
                     "warning: %s: the sequences of %s start in different ways. Its mini starts each of them as the "
                     "archive does\n",
                     archive_path.c_str(), job.label.c_str());
    }

    if (const char* comment = rip::SoundComment(data.archive_sha1, archive.Sounds()[sounds.front()].name);
        comment && sounds.size() == 1)
    {
        tags["comment"] = comment;
    }

    return rip::BuildArchiveMini(SoundIds(job), lib_name, std::move(tags));
}

// Rips one sound archive in archive mode, with the group files `groups` that the game keeps beside it and the
// game's `effects` on the aux buses, adding the number of sounds --only picked to `matched`. Returns the number of
// .mini3sf files written, or -1 on an error.
int RipArchive(const fs::path& out_dir, rip::FileNames& names, const std::string& lib_base,
               const rip::FileChunk& archive_file, const std::vector<rip::FileChunk>& groups,
               const std::string& firmware_path, const std::vector<uint8_t>& firmware, const std::string& game_name,
               const nnsnd::AuxEffectLists& effects, const Options& o, std::size_t& matched)
{
    const std::string& archive_path = archive_file.path;
    const std::optional<ntdlib::sar::SoundArchive> archive = LoadArchive(archive_file, groups);
    if (!archive)
    {
        return -1;
    }

    std::vector<Job> jobs = SelectArchiveSounds(*archive, o);
    matched += jobs.size();

    std::size_t wave_sounds = 0;
    for (const auto& s : archive->Sounds())
    {
        wave_sounds += s.type == ntdlib::sar::SoundType::kWaveSound;
    }

    const std::size_t skipped = DropIncomplete(jobs, *archive);
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(archive_file.data));
    AddArrangements(jobs, *archive, sha1);

    const rip::ArchiveEffects* known = FindKnownArchive(*archive, archive_path, sha1);
    const nnsnd::AuxEffectLists& buses = known ? known->buses : effects;
    const ArchiveData data = {archive_file.data, archive->GroupFiles(), buses, sha1, known};
    const std::size_t silent = AnalyzeAndDropSilent(jobs, *archive, data, firmware, o.variables, o);
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", archive_path.c_str(), SkippedText(skipped, silent).c_str());
        return 0;
    }

    const DescriptorChoice descriptor = ChooseDescriptor(jobs, *archive, data);
    const LibraryFiles kept = KeepLibraryFiles(*archive, archive_file, groups, jobs, sha1);
    const Tags common = xsflib::cli::RipTags(o, game_name, "3sf", "archive");
    const auto lib =
        rip::BuildArchiveLibrary(archive_path, kept.archive, firmware_path, firmware, common, kept.groups,
                                 descriptor.version, buses, descriptor.timed_starts, descriptor.effect_sets);
    const std::string lib_name = names.LibraryIn(out_dir, lib_base, lib);
    if (!WriteLibrary(out_dir, lib_name, lib))
    {
        return -1;
    }

    Tags mini_tags = common;
    if (!o.variables.empty())
    {
        mini_tags["3sf_var"] = FormatVariables(o.variables);
    }

    const auto build_mini = [&](const Job& job, Tags tags)
    {
        return BuildJobMini(job, std::move(tags), *archive, data, archive_path, lib_name);
    };
    const int count = xsflib::cli::WriteMinis(out_dir, names, jobs, mini_tags, "3sf_sound", build_mini);
    if (count >= 0)
    {
        std::fprintf(stderr, "%s: %d mini3sf files%s; %zu wave sounds not ripped (archive mode only plays sequences)\n",
                     archive_path.c_str(), count, SkippedText(skipped, silent).c_str(), wave_sounds);
    }

    return count;
}

// The group file `name` anywhere in the game's RomFS, found by its file name in any case, at its RomFS path, or
// nothing.
std::optional<rip::FileChunk> FindGameGroup(const rip::GameFiles& game, const std::string& name)
{
    for (const std::string& path : game.romfs_files)
    {
        // The file name follows the last slash, or is the whole path.
        const std::string file_name = path.substr(path.rfind('/') + 1);
        rip::FileChunk group{path, {}};
        if (ntdlib::sar::IsGroupFileName(file_name, name, ntdlib::sar::Flavor::kCtr) &&
            game.read_romfs(path, group.data))
        {
            return group;
        }
    }

    return std::nullopt;
}

// The file name a rip gives the DSP firmware it carries, unless it's a file from the game's RomFS, which keeps its
// path there.
constexpr char kFirmwareName[] = "dspaudio.cdc";

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
    "dropped on the program. An archive's group files (.bcgrp) are found in the game, or in the\n"
    "archive's folder and the folders below it. Sounds that make no sound, and sequences whose data\n"
    "isn't all in the archive and its group files, are listed and skipped.\n"
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

    rip::FileChunk archive_file{file_name, {}};
    if (!ReadWholeFile(input, archive_file.data))
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

    // The group files are found by their names in the archive's folder or a folder below it.
    const fs::path folder = InputPath(input).parent_path();
    const auto find_group = [&folder](const std::string& name)
    {
        return rip::FindGroupIn(folder, name);
    };
    const std::vector<rip::FileChunk> groups =
        ntdlib::rip::FindGroups<rip::FileChunk>(archive_file.data, find_group, kSarOptions);

    std::fprintf(stderr, "%s", kArchiveModeNotice);

    const std::string game_name = GameName(o, input, nullptr);
    std::size_t matched = 0;
    const int count = RipArchive(out_dir, names, game_name, archive_file, groups, kFirmwareName, firmware->bytes,
                                 game_name, {}, o, matched);
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

// The DSP firmware that archive mode plays a game's archives with: the file that --firmware names, or else the game's
// firmware (rip::FindGameFirmware). A rip names the firmware by its RomFS path, or else kFirmwareName. Returns nothing
// after an error message if there's none.
std::optional<FirmwareFile> ChooseGameFirmware(const rip::GameFiles& game, const rip::GameProfile* profile,
                                               const Options& o)
{
    FirmwareFile firmware{kFirmwareName, o.firmware.bytes};
    if (firmware.bytes.empty())
    {
        if (auto found = rip::FindGameFirmware(game, profile))
        {
            std::fprintf(stderr, "firmware: %s\n", FirmwareSource(*found).c_str());
            if (!found->romfs_path.empty())
            {
                firmware.path = found->romfs_path;
            }

            firmware.bytes = std::move(found->data);
        }
    }

    if (firmware.bytes.empty())
    {
        std::fprintf(stderr,
                     "error: no DSP firmware (a DSP1 image) in the game's RomFS or code; give one with --firmware\n");
        return std::nullopt;
    }

    return firmware;
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

    const std::optional<FirmwareFile> firmware = ChooseGameFirmware(game, profile, o);
    if (!firmware)
    {
        return 1;
    }

    const auto archives = game.SoundArchives();
    if (!o.only_archive.empty() && std::find(archives.begin(), archives.end(), o.only_archive) == archives.end())
    {
        std::fprintf(stderr, "error: the game has no sound archive %s (--list shows them)\n", o.only_archive.c_str());
        return 1;
    }

    // The effects that the game puts on the aux buses go into each rip (rip::FindGameEffects).
    const rip::GameEffects* found_effects = rip::FindGameEffects(game);
    const nnsnd::AuxEffectLists effects = found_effects ? found_effects->buses : nnsnd::AuxEffectLists{};
    if (found_effects)
    {
        std::fprintf(stderr, "aux effects of %s: %s\n", found_effects->game, rip::DescribeEffects(effects).c_str());
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

        rip::FileChunk archive_file{path, {}};
        if (!game.read_romfs(path, archive_file.data))
        {
            std::fprintf(stderr, "error: can't read %s\n", path.c_str());
            failed = true;
            continue;
        }

        const auto find_group = [&game](const std::string& name)
        {
            return FindGameGroup(game, name);
        };
        const std::vector<rip::FileChunk> groups =
            ntdlib::rip::FindGroups<rip::FileChunk>(archive_file.data, find_group, kSarOptions);
        const std::string lib_base =
            archives.size() == 1 ? game_name : game_name + " " + fs::path(path).stem().string();
        const int n = RipArchive(out_dir, names, lib_base, archive_file, groups, firmware->path, firmware->bytes,
                                 game_name, effects, o, matched);
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

// Game mode with a reverb (rip::GameProfile::reverb): the reverb rings on after each sequence that sends to it. The
// model doesn't play the reverb. Each such job gets the timing of a render of the library `lib` as a player plays
// it (rip::RingOut). The renders spread over several threads.
void RingOutReverb(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive, const std::vector<uint8_t>& lib,
                   unsigned threads)
{
    struct Ring
    {
        Job* job;
        std::string label; // for the warning of a failed render
        std::optional<rip::Timing> timing;
    };

    std::vector<Ring> rings;
    for (Job& job : jobs)
    {
        if (job.sequence && job.timing && rip::SendsToBusA(archive, archive.Sounds()[job.index]))
        {
            rings.push_back({&job, job.label, std::nullopt});
        }
    }

    if (rings.empty())
    {
        return;
    }

    std::fprintf(stderr, "reverb: rendering %zu sounds that send to aux bus A, for their lengths\n", rings.size());
    const auto ring_out = [&lib](Ring& ring)
    {
        ring.timing = rip::RingOut(lib, SoundId(*ring.job), *ring.job->timing);

        return true;
    };
    xsflib::cli::AnalysisSettings settings;
    settings.progress_every = 10;
    settings.progress = "rendered";
    xsflib::cli::AnalyzeLengths(rings, threads, ring_out, settings);

    for (const Ring& ring : rings)
    {
        ring.job->timing = ring.timing;
    }
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
    std::vector<Job> jobs = SelectSounds(archive, o, true);
    if (jobs.empty() && PicksSounds(o))
    {
        return NoSoundMatches(o);
    }

    const std::size_t silent =
        AnalyzeAndDropSilent(jobs, archive, {archive_bytes, archive.GroupFiles()}, firmware, {}, o);
    const std::string input_name = InputPath(input).filename().string();
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", input_name.c_str(), SkippedText(0, silent).c_str());
        return 0;
    }

    // The library's archive holds the files that the ripped sounds play.
    std::vector<uint32_t> sounds;
    for (const Job& job : jobs)
    {
        sounds.push_back(job.index);
    }

    std::string error;
    const auto lib = rip::BuildLibrary(game, profile, sounds, common, &error);
    if (!lib)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    const std::string lib_name = names.LibraryIn(out_dir, game_name, *lib);
    if (!WriteLibrary(out_dir, lib_name, *lib))
    {
        return 1;
    }

    if (o.lengths && profile.reverb.start != 0)
    {
        RingOutReverb(jobs, archive, *lib, o.jobs);
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

// Takes 3sfrip's options that the frame doesn't have: --firmware and --var. Returns -1 for another option, 0 when
// it took the option, or 2 after an error.
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

// Rips one input. Returns the exit code.
int RipInput(const std::string& input, const Options& o, xsflib::cli::FileNameRegistries& names)
{
    // Other consoles' sound archives, by ntdlib's kinds of archive.
    using Kind = ntdlib::sar::SoundArchiveKind;
    constexpr std::array<std::pair<Kind, const char*>, 5> kOtherArchives = {{
        {Kind::kFsarBig, "a Wii U or Switch sound archive (FSAR)"},
        {Kind::kFsarLittle, "a Wii U or Switch sound archive (FSAR)"},
        {Kind::kFsarUnknown, "a Wii U or Switch sound archive (FSAR)"},
        {Kind::kRsar, "a Wii sound archive (RSAR)"},
        {Kind::kSdat, "a DS sound archive (SDAT)"},
    }};
    const std::string head = xsflib::rip::ReadHead(input, kHeadSize);
    const Kind kind = ntdlib::sar::SoundArchiveKindOf(head);
    for (const auto& [other, what] : kOtherArchives)
    {
        if (kind == other)
        {
            std::fprintf(stderr, "error: %s is %s; 3SF rips 3DS sound archives (CSAR, .bcsar)\n", input.c_str(), what);
            return 1;
        }
    }

    const fs::path out_dir = o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_3sf") : fs::path(o.out_dir);
    rip::FileNames& out_names = xsflib::cli::NamesIn(names, out_dir, ".3sflib", ".mini3sf");
    if (kind == Kind::kCsar)
    {
        return RipLooseArchive(input, out_dir, o, out_names);
    }

    return RipGame(input, out_dir, o, out_names);
}

// Runs before the first input: reads the firmware that --firmware names and takes the firmware files out of the
// inputs. Returns 0, or the exit code.
int BeforeInputs(std::vector<std::string>& inputs, Options& o)
{
    ntdlib::ctr::SilenceStdout(); // Teakra reports unmodelled MMIO accesses on stdout

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
