// SPDX-License-Identifier: MIT

// 3sfrip's options and jobs: the sounds that a rip picks, the length analysis of each, and the jobs that a rip drops
// because they'd play with missing data or make no sound.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "xsflib/cli/rip.h"

#include "nnsnd/aux_effects.h"
#include "rip/effects.h"
#include "rip/length.h"
#include "threesf/format.h"

namespace threesf::cli
{

// A DSP firmware file: its path, and its bytes.
struct FirmwareFile
{
    std::string path;
    std::vector<uint8_t> bytes;
};

// 3sfrip's options: xsflib's options of a ripper, and the variables and firmware of archive mode.
struct Options : xsflib::cli::RipOptions
{
    Variables variables; // --var: set before each sequence starts, and written as the 3sf_var tag

    // The firmware --firmware names, and a firmware file among the inputs, which only loose sound archives use.
    FirmwareFile firmware, input_firmware;
};

// A mini that a rip writes: its sound, its label, whether the sound is a sequence, and its timing.
struct Job
{
    uint32_t index;
    std::string label;
    bool sequence;
    std::optional<rip::Timing> timing;

    // Archive mode: the sequences that the mini plays, in the order in which they start. A piece whose channels the
    // game plays as separate sequences has all of them (see rip::FindPieces), the first at `index`. Empty in game mode.
    std::vector<uint32_t> sounds;

    // Archive mode: the start of a later state of the sequence, in place of the sequence's start (see AddArrangements).
    // Empty for the sequence itself.
    std::optional<SequenceStart> start = std::nullopt;

    // The sounds that the mini plays: `index` alone in game mode.
    std::vector<uint32_t> Sounds() const
    {
        return sounds.empty() ? std::vector<uint32_t>{index} : sounds;
    }
};

// What a sound archive is played with: its bytes, the group files beside it that the rip found, and the effects on the
// aux buses.
struct ArchiveData
{
    const std::vector<uint8_t>& archive;
    const std::vector<std::vector<uint8_t>>& groups;
    nnsnd::AuxEffectLists effects = {};

    // Archive mode: the archive's SHA-1 in lowercase hex. StartOf finds how a known game starts each sequence by it.
    // Empty in game mode. Game mode's players ignore the tags of a start.
    std::string archive_sha1 = {};

    // Archive mode: the known archive's entry, whose game effects some sequences play with in place of `effects`.
    const rip::ArchiveEffects* known = nullptr;

    // The effect set that a sequence started as `start` plays (rip::EffectSetOf).
    uint32_t EffectSet(const SequenceStart& start) const
    {
        return rip::EffectSetOf(known, start.game_effect);
    }

    // The effects of effect set `set`.
    const nnsnd::AuxEffectLists& EffectsOf(uint32_t set) const
    {
        return set == 0 ? effects : known->game_effects[set - 1].buses;
    }
};

// How a known game starts sound `sound` of `archive`, by the archive's SHA-1 `archive_sha1`. The archive's start for an
// empty SHA-1.
SequenceStart StartOf(const ntdlib::sar::SoundArchive& archive, std::string_view archive_sha1, uint32_t sound);

// How `job` starts its sound `sound`: as the job's later state does, or else as StartOf gives.
SequenceStart StartOf(const Job& job, const ntdlib::sar::SoundArchive& archive, std::string_view archive_sha1,
                      uint32_t sound);

// Length analysis runs every selected sequence through the nw::snd model and the DSP firmware, with `variables` set
// before each starts, and with the start that StartOf gives. The sounds are independent, and the analysis spreads them
// over several threads. Each sequence of a piece is measured alone. The piece then gets the latest end and the longest
// fade (rip::TimingOfPiece).
void AnalyzeLengths(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive, const ArchiveData& data,
                    const std::vector<uint8_t>& firmware, const Variables& variables, unsigned threads);

// Lists and drops the sequences whose data isn't all in the archive and its group files, and the pieces with such a
// sequence. They would play with missing instruments or channels. Returns how many it dropped.
std::size_t DropIncomplete(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive);

// Lists and drops the sounds whose volume in the archive is 0, and the pieces whose sequences all have a volume of 0.
// The game's sound library multiplies everything a sound plays by that volume. Such a sound therefore makes no sound.
// Returns how many it dropped.
std::size_t DropMuted(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive);

// Lists and drops the sequences that the length analysis found never sound, such as those that play no notes, or play
// them at a volume of 0. Returns how many it dropped.
std::size_t DropUnheard(std::vector<Job>& jobs);

// Drops the sounds that make no sound. DropMuted finds those at a volume of 0. Unless --no-length is given, the length
// analysis of the rest then finds the sequences that never sound. Returns how many it dropped.
std::size_t AnalyzeAndDropSilent(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive,
                                 const ArchiveData& data, const std::vector<uint8_t>& firmware,
                                 const Variables& variables, const Options& o);

// "; 3 sounds skipped (2 data outside the archive, 1 no sound)", or nothing when none were skipped.
std::string SkippedText(std::size_t missing_data, std::size_t silent);

// Whether --only or --bgm picks sounds, so that picking none is an error.
bool PicksSounds(const Options& o);

std::vector<Job> SelectSounds(const ntdlib::sar::SoundArchive& archive, const Options& o, bool with_wave_sounds);

// Archive mode: the sequences that --only and --bgm pick. A piece whose channels the game plays as separate sequences
// makes one job (see rip::FindPieces). --only picks it by its name or by any of its sequences' labels, and --bgm by any
// of its sequences.
std::vector<Job> SelectArchiveSounds(const ntdlib::sar::SoundArchive& archive, const Options& o);

// Archive mode: adds a job after each sequence's job for each later state that a known game moves the sequence to
// (ntdlib::ctr::snd::SequenceArrangements), then for each state in which the game gives it other banks
// (ntdlib::ctr::snd::BankArrangements). Its label is the sequence's label followed by the state's suffix. A state of
// other banks keeps the rest of the sequence's start. A piece of several sequences gets none.
void AddArrangements(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive, std::string_view archive_sha1);

// The item IDs of the sounds that a job's mini plays: the item type of sounds (1) in the top byte, and each sound's
// index.
std::vector<uint32_t> SoundIds(const Job& job);

// The labels of the sounds that a job's mini plays, joined with ", ".
std::string SoundLabels(const ntdlib::sar::SoundArchive& archive, const Job& job);

// The item id of a job's sound: the item type of sounds (1) in the top byte, and the sound's index.
uint32_t SoundId(const Job& job);

} // namespace threesf::cli
