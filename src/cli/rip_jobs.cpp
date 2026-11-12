// SPDX-License-Identifier: MIT

// 3sfrip's jobs (see rip_jobs.h).

#include "cli/rip_jobs.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/ctr/snd/starting_tracks.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/cli/rip.h"

#include "rip/length.h"
#include "rip/music.h"
#include "rip/rip.h"
#include "threesf/format.h"

namespace threesf::cli
{

using ntdlib::ctr::kSarOptions;

SequenceStart StartOf(const ntdlib::sar::SoundArchive& archive, std::string_view archive_sha1, uint32_t sound)
{
    if (archive_sha1.empty())
    {
        return {};
    }

    return ntdlib::ctr::snd::StartingSequenceTracks(archive_sha1, archive.Sounds()[sound].name);
}

SequenceStart StartOf(const Job& job, const ntdlib::sar::SoundArchive& archive, std::string_view archive_sha1,
                      uint32_t sound)
{
    return job.start ? *job.start : StartOf(archive, archive_sha1, sound);
}

void AnalyzeLengths(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive, const ArchiveData& data,
                    const std::vector<uint8_t>& firmware, const Variables& variables, unsigned threads)
{
    struct Measure
    {
        uint32_t index;
        bool sequence;
        std::string label; // for the warning of a failed analysis
        SequenceStart start;
        std::optional<rip::Timing> timing;
        uint32_t effect_set = 0;
    };

    std::vector<Measure> measures;
    for (const Job& job : jobs)
    {
        const std::vector<uint32_t> sounds = job.Sounds();
        for (const uint32_t sound : sounds)
        {
            const std::string& label = sounds.size() > 1 ? archive.Sounds()[sound].name : job.label;
            const SequenceStart start = StartOf(job, archive, data.archive_sha1, sound);
            measures.push_back({sound, job.sequence, label, start, std::nullopt, data.EffectSet(start)});
        }
    }

    const rip::LengthAnalyzer analyzer(data.archive, firmware, variables, data.groups, data.effects);
    const auto analyze = [&analyzer, &data](Measure& measure)
    {
        if (!measure.sequence)
        {
            return false;
        }

        measure.timing = analyzer.Analyze(measure.index, measure.start, &data.EffectsOf(measure.effect_set));

        return true;
    };
    xsflib::cli::AnalyzeLengths(measures, threads, analyze);

    std::size_t next = 0;
    for (Job& job : jobs)
    {
        std::vector<std::optional<rip::Timing>> timings;
        for (std::size_t i = job.Sounds().size(); i > 0; i--)
        {
            timings.push_back(measures[next++].timing);
        }

        job.timing = rip::TimingOfPiece(timings);
    }
}

std::size_t DropIncomplete(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive)
{
    const auto incomplete = [&archive](const Job& job)
    {
        const std::vector<uint32_t> sounds = job.Sounds();
        std::string what;
        for (const uint32_t sound : sounds)
        {
            const auto missing = ntdlib::rip::MissingData(archive, sound, kSarOptions);
            if (missing.empty())
            {
                continue;
            }

            std::string list;
            for (std::size_t i = 0; i < missing.size() && i < 3; i++)
            {
                list += (i ? ", " : "") + missing[i];
            }

            if (missing.size() > 3)
            {
                list += ", ...";
            }

            // A piece names each sequence whose data is missing.
            if (sounds.size() > 1)
            {
                list = archive.Sounds()[sound].name + " (" + list + ")";
            }

            what += (what.empty() ? "" : "; ") + list;
        }

        if (what.empty())
        {
            return false;
        }

        std::fprintf(stderr, "skipped %s: not in this archive: %s\n", job.label.c_str(), what.c_str());

        return true;
    };
    return std::erase_if(jobs, incomplete);
}

std::size_t DropMuted(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive)
{
    const auto muted = [&archive](const Job& job)
    {
        const auto audible = [&archive](uint32_t sound)
        {
            return archive.Sounds()[sound].volume != 0;
        };
        if (std::ranges::any_of(job.Sounds(), audible))
        {
            return false;
        }

        std::fprintf(stderr, "skipped %s: it makes no sound (volume 0)\n", job.label.c_str());

        return true;
    };
    return std::erase_if(jobs, muted);
}

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

std::size_t AnalyzeAndDropSilent(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive,
                                 const ArchiveData& data, const std::vector<uint8_t>& firmware,
                                 const Variables& variables, const Options& o)
{
    std::size_t silent = DropMuted(jobs, archive);
    if (o.lengths && !jobs.empty())
    {
        AnalyzeLengths(jobs, archive, data, firmware, variables, o.jobs);
        silent += DropUnheard(jobs);
    }

    return silent;
}

std::string SkippedText(std::size_t missing_data, std::size_t silent)
{
    return xsflib::cli::SkippedText(missing_data, "data outside the archive", silent, "sound");
}

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
            jobs.push_back({static_cast<uint32_t>(i), s.name, sequence, std::nullopt, {}});
        }
    }

    return jobs;
}

std::vector<Job> SelectArchiveSounds(const ntdlib::sar::SoundArchive& archive, const Options& o)
{
    const auto& sounds = archive.Sounds();
    const std::vector<rip::Piece> pieces = rip::FindPieces(archive);
    std::vector<const rip::Piece*> piece_of(sounds.size(), nullptr);
    for (const rip::Piece& piece : pieces)
    {
        for (const uint32_t sound : piece.sounds)
        {
            piece_of[sound] = &piece;
        }
    }

    std::vector<Job> jobs;
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        // Streams are out of scope, and archive mode only plays sequences. A piece's job comes at its first sound.
        const auto index = static_cast<uint32_t>(i);
        const rip::Piece* piece = piece_of[i];
        if (sounds[i].type != ntdlib::sar::SoundType::kSequence || (piece && piece->sounds.front() != index))
        {
            continue;
        }

        const std::string& label = piece ? piece->name : sounds[i].name;
        const std::vector<uint32_t> members = piece ? piece->sounds : std::vector<uint32_t>{index};
        bool matches = std::regex_search(label, o.only);
        bool music = false;
        for (const uint32_t sound : members)
        {
            matches = matches || std::regex_search(sounds[sound].name, o.only);
            music = music || rip::IsMusic(archive, sounds[sound]);
        }

        if (matches && (!o.bgm || music))
        {
            jobs.push_back({index, label, true, std::nullopt, members});
        }
    }

    return jobs;
}

void AddArrangements(std::vector<Job>& jobs, const ntdlib::sar::SoundArchive& archive, std::string_view archive_sha1)
{
    std::vector<Job> all;
    for (Job& job : jobs)
    {
        const bool alone = job.sounds.size() <= 1;
        const std::string name = archive.Sounds()[job.index].name;
        all.push_back(std::move(job));
        if (!alone)
        {
            continue;
        }

        const Job piece = all.back();
        for (const auto& arrangement : ntdlib::ctr::snd::SequenceArrangements(archive_sha1, name))
        {
            Job state = piece;
            state.label += arrangement.suffix;
            state.start = arrangement.start;
            all.push_back(std::move(state));
        }

        const std::vector<uint32_t>& banks = archive.Sounds()[piece.index].banks;
        if (banks.empty())
        {
            continue;
        }

        for (const auto& arrangement : ntdlib::ctr::snd::BankArrangements(archive_sha1, banks.front()))
        {
            Job state = piece;
            state.label += arrangement.suffix;
            state.start = StartOf(archive, archive_sha1, piece.index);
            state.start->banks = arrangement.start.banks;
            all.push_back(std::move(state));
        }
    }

    jobs = std::move(all);
}

std::vector<uint32_t> SoundIds(const Job& job)
{
    std::vector<uint32_t> ids;
    for (const uint32_t sound : job.Sounds())
    {
        ids.push_back(ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, sound));
    }

    return ids;
}

std::string SoundLabels(const ntdlib::sar::SoundArchive& archive, const Job& job)
{
    std::string labels;
    for (const uint32_t sound : job.Sounds())
    {
        labels += (labels.empty() ? "" : ", ") + archive.Sounds()[sound].name;
    }

    return labels;
}

uint32_t SoundId(const Job& job)
{
    return ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, job.index);
}

} // namespace threesf::cli
