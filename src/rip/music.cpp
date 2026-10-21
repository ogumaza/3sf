// SPDX-License-Identifier: MIT

// The ripper's choice of music (see music.h).

#include "rip/music.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/sar/formats.h"

#include "common/sar_options.h"

namespace threesf::rip
{
namespace
{

// The fewest tracks and notes that make IsMusic take a sequence for music, whatever its label says.
constexpr int kMusicMinTracks = 3;
constexpr int kMusicMinNotes = 50;

// The label's words: the runs of ASCII letters and digits in it, in capitals. Unlike std::isalnum and std::toupper,
// this doesn't depend on the C locale.
std::vector<std::string> LabelWords(std::string_view label)
{
    std::vector<std::string> words;
    std::string word;
    for (char c : label)
    {
        if (c >= 'a' && c <= 'z')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }

        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        {
            word += c;
        }
        else if (!word.empty())
        {
            words.push_back(word);
            word.clear();
        }
    }

    if (!word.empty())
    {
        words.push_back(word);
    }

    return words;
}

// Whether `word` is `name`, or `name` and a number.
bool IsNumberedWord(std::string_view word, std::string_view name)
{
    if (!word.starts_with(name))
    {
        return false;
    }

    const std::string_view rest = word.substr(name.size());
    return std::all_of(rest.begin(), rest.end(), [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

SequenceShape WalkSequence(std::span<const uint8_t> data, uint32_t start)
{
    using ntdlib::commands::Kind;

    const ntdlib::nwsnd::Grammar grammar = ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::CtrPolicy>();
    SequenceShape shape;
    std::vector<bool> seen(data.size());
    std::vector<std::size_t> todo{start};
    while (!todo.empty())
    {
        std::size_t pc = todo.back();
        todo.pop_back();
        while (pc < data.size() && !seen[pc])
        {
            seen[pc] = true;
            const std::optional<ntdlib::nwsnd::DecodedCommand> read =
                ntdlib::nwsnd::DecodeCommand(data, static_cast<uint32_t>(pc), grammar);
            if (!read)
            {
                break;
            }

            const ntdlib::commands::Command& c = read->command;
            shape.notes += c.kind == Kind::kNote ? 1 : 0;
            shape.tracks += c.kind == Kind::kOpenTrack ? 1 : 0;
            if (ntdlib::commands::HasTarget(c.kind))
            {
                todo.push_back(c.target);
            }

            if (!ntdlib::nwsnd::FallsThrough(c, grammar))
            {
                break;
            }

            pc += read->size;
        }
    }

    return shape;
}

bool LabelMarksMusic(std::string_view label)
{
    const std::vector<std::string> words = LabelWords(label);
    if (words.size() >= 2 && words[0] == "SEQ" && words[1] == "M")
    {
        return true;
    }

    for (const std::string& word : words)
    {
        if (word.starts_with("BGM") || IsNumberedWord(word, "JIN") || IsNumberedWord(word, "JINGLE") ||
            IsNumberedWord(word, "FANFARE"))
        {
            return true;
        }
    }

    return false;
}

bool IsMusic(const ntdlib::sar::SoundArchive& archive, const ntdlib::sar::SoundInfo& sound)
{
    if (LabelMarksMusic(sound.name))
    {
        return true;
    }

    if (sound.type != ntdlib::sar::SoundType::kSequence)
    {
        return false;
    }

    // A sequence that doesn't parse isn't music here; ripping it reports the problem.
    try
    {
        const ntdlib::sar::Sequence sequence =
            ntdlib::sar::Sequence::Parse(archive.FileData(sound.file_id), kSarOptions);
        const SequenceShape shape = WalkSequence(sequence.data, sound.start_offset);
        return shape.tracks >= kMusicMinTracks && shape.notes >= kMusicMinNotes;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

} // namespace threesf::rip
