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

#include "ntdlib/sar/formats.h"

#include "common/sar_options.h"

namespace threesf::rip
{
namespace
{

// The fewest tracks and notes that make IsMusic take a sequence for music, whatever its label says.
constexpr int kMusicMinTracks = 3;
constexpr int kMusicMinNotes = 50;

// One sequence command, decoded where it starts as the nw::snd model's parser reads it (SequenceTrack::Parse).
struct Command
{
    std::size_t next = 0; // where the next command starts
    bool note = false;
    std::optional<std::size_t> track;  // opentrack: where the track it opens starts
    std::optional<std::size_t> target; // jump or call: where it goes
    bool ends = false;                 // an unconditional jump, ret or fin, which the next command doesn't follow
};

Command DecodeCommand(std::span<const uint8_t> data, std::size_t pc)
{
    // Past the end, a read gives 0xff (fin), as the model's does.
    const auto byte = [&](std::size_t p) -> std::size_t
    {
        return p < data.size() ? data[p] : 0xff;
    };

    // The bytes a variable-length number takes at `p`.
    const auto midi_number_size = [&](std::size_t p)
    {
        std::size_t n = 1;
        while (p + n - 1 < data.size() && (data[p + n - 1] & 0x80))
        {
            n++;
        }

        return n;
    };

    // The prefixes, in the order the parser takes them: a condition, a time argument, and then a random or variable
    // argument, whose size replaces that of the argument the command normally takes.
    Command c;
    std::size_t p = pc;
    std::size_t cmd = byte(p++);
    const bool conditional = cmd == 0xa2;
    if (conditional)
    {
        cmd = byte(p++);
    }

    std::size_t time_size = 0;
    if (cmd >= 0xa3 && cmd <= 0xa5)
    {
        time_size = cmd == 0xa3 ? 2 : (cmd == 0xa4 ? 4 : 1);
        cmd = byte(p++);
    }

    std::size_t prefixed_size = 0;
    if (cmd == 0xa0 || cmd == 0xa1)
    {
        prefixed_size = cmd == 0xa0 ? 4 : 1;
        cmd = byte(p++);
    }

    const auto argument = [&](std::size_t size)
    {
        return prefixed_size != 0 ? prefixed_size : size;
    };

    if (cmd < 0x80) // a note: the key, then the velocity and the length
    {
        c.note = true;
        p += 1 + argument(midi_number_size(p + 1));
    }
    else if (cmd == 0x80 || cmd == 0x81) // wait, program change
    {
        p += argument(midi_number_size(p));
    }
    else if (cmd == 0x88) // opentrack: the track's number and its offset
    {
        c.track = byte(p + 1) << 16 | byte(p + 2) << 8 | byte(p + 3);
        p += 4;
    }
    else if (cmd == 0x89 || cmd == 0x8a) // jump, call
    {
        c.target = byte(p) << 16 | byte(p + 1) << 8 | byte(p + 2);
        c.ends = cmd == 0x89 && !conditional;
        p += 3;
    }
    else if (cmd >= 0xb0 && cmd < 0xe0) // a byte, and the time argument if there is one
    {
        p += argument(1) + time_size;
    }
    else if (cmd >= 0xe0 && cmd < 0xf0) // a 16-bit value
    {
        p += argument(2);
    }
    else if (cmd == 0xf0) // the extended commands: the variable operations take a variable and a 16-bit value
    {
        const std::size_t kind = byte(p++) & 0xf0;
        if (kind == 0x80 || kind == 0x90)
        {
            p += 1 + argument(2);
        }
        else if (kind == 0xe0)
        {
            p += argument(2);
        }
    }
    else if (cmd == 0xfe) // alloctrack
    {
        p += 2;
    }
    else if (cmd == 0xfd || cmd == 0xff) // ret, fin
    {
        c.ends = !conditional;
    }

    c.next = p;

    return c;
}

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
            const Command c = DecodeCommand(data, pc);
            shape.notes += c.note ? 1 : 0;
            if (c.track)
            {
                shape.tracks++;
                todo.push_back(*c.track);
            }

            if (c.target)
            {
                todo.push_back(*c.target);
            }

            if (c.ends)
            {
                break;
            }

            pc = c.next;
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
