// SPDX-License-Identifier: MIT

// The 3SF file format (see format.h and docs/3sf.md).

#include "threesf/format.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/ctr_fx/effects.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/bytes.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/streaming.h"
#include "xsflib/text.h"

#include "nnsnd/aux_effects.h"

namespace threesf
{
namespace
{

using xsflib::psf::CheckDescriptorFlags;
using xsflib::psf::CheckDescriptorOutputMode;
using xsflib::psf::CheckDescriptorVersion;
using xsflib::psf::PadTo4;
using xsflib::psf::Put16;
using xsflib::psf::Put32;
using xsflib::psf::PutPath;
using xsflib::psf::Read16;
using xsflib::psf::ReadPath;

using nnsnd::AuxEffect;

// The flags of an effect in the archive descriptor (docs/3sf.md, "Effects on the aux buses"). A delay has the first
// alone.
constexpr uint32_t kEffectSurround = 1;
constexpr uint32_t kEffectHighPassDamping = 2;

// The bytes of FCRAM that a player gives the lines of the firmware's delays and reverbs, all together. The archive-mode
// model's FCRAM holds them and the wave archives.
constexpr uint64_t kMaxFirmwareLines = 16 * 1024 * 1024;

// Parses a whole number: decimal, or hex with a 0x prefix, after an optional minus sign. Returns nullopt for anything
// else, white space included.
std::optional<int64_t> ParseNumber(std::string_view text)
{
    const bool negative = text.starts_with('-');
    if (negative)
    {
        text.remove_prefix(1);
    }

    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        text.remove_prefix(2);
    }

    // from_chars takes no sign and no prefix for an unsigned value. Text such as "--1" or "0x-1" fails here.
    uint64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value, base);
    if (text.empty() || error != std::errc() || stop != end || value > 0xffffffffu)
    {
        return std::nullopt;
    }

    return negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
}

// `text` without the white space around it. Text of nothing but white space gives an empty view.
std::string_view TrimSpace(std::string_view text)
{
    constexpr std::string_view kSpace = " \t\r\n";
    const std::size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos)
    {
        return {};
    }

    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

// Parses decimal digits, then an optional '.' and more digits, with white space around them allowed. Returns nullopt
// for anything else. ReadDecimal would also take a sign, a ',' and an exponent.
std::optional<float> ParsePlainDecimal(std::string_view text)
{
    text = TrimSpace(text);
    if (text.empty())
    {
        return std::nullopt;
    }

    const std::size_t point = text.find('.');
    const auto digits = [](std::string_view part)
    {
        return std::ranges::all_of(part, [](char c) { return c >= '0' && c <= '9'; });
    };

    if (point == 0 || !digits(text.substr(0, point)) ||
        (point != std::string_view::npos && (point + 1 == text.size() || !digits(text.substr(point + 1)))))
    {
        return std::nullopt;
    }

    const std::optional<xsflib::DecimalNumber> number = xsflib::ReadDecimal(text);
    if (!number || number->length != text.size())
    {
        return std::nullopt;
    }

    return static_cast<float>(number->value);
}

// The bus's name in messages.
std::string BusName(std::size_t bus)
{
    return bus == 0 ? "A" : "B";
}

void PutFloat(std::vector<uint8_t>& v, float x)
{
    Put32(v, std::bit_cast<uint32_t>(x));
}

// An effect: its kind, then its parameters.
void PutEffect(std::vector<uint8_t>& v, const AuxEffect& effect)
{
    Put32(v, static_cast<uint32_t>(effect.kind));
    if (!AuxEffect::Known(static_cast<uint32_t>(effect.kind)))
    {
        return;
    }

    if (!effect.IsReverb())
    {
        const ntdlib::ctr_fx::DelayParam& d = effect.delay;
        Put32(v, d.delay_time);
        PutFloat(v, d.feedback);
        PutFloat(v, d.damping);
        Put32(v, d.enable_surround ? kEffectSurround : 0);
        return;
    }

    const ntdlib::ctr_fx::ReverbParam& r = effect.reverb;
    Put32(v, r.early_reflection_time);
    Put32(v, r.fused_time);
    Put32(v, r.pre_delay_time);
    PutFloat(v, r.coloration);
    PutFloat(v, r.damping);
    for (const uint32_t size : r.filter_sizes)
    {
        Put32(v, size);
    }

    PutFloat(v, r.early_gain);
    PutFloat(v, r.fused_gain);
    Put32(v, (r.enable_surround ? kEffectSurround : 0) | (r.high_pass_damping ? kEffectHighPassDamping : 0));
}

// Reads the 32-bit values of an effect's parameters at `pos`, and moves `pos` past them. Returns nothing if `data`
// ends first.
std::optional<std::vector<uint32_t>> ReadWords(const std::vector<uint8_t>& data, std::size_t& pos, std::size_t count)
{
    if (data.size() < pos || data.size() - pos < 4 * count)
    {
        return std::nullopt;
    }

    std::vector<uint32_t> words(count);
    for (uint32_t& word : words)
    {
        word = xsflib::LoadLe32(&data[pos]);
        pos += 4;
    }

    return words;
}

// Reads bus `bus`'s effects at `pos` into `lists`, those of effect set `set`. Returns false if `data` ends first. An
// effect that this player can't play sets d.unsupported_effect, and the effects after it aren't read.
bool ReadEffects(const std::vector<uint8_t>& data, std::size_t& pos, std::size_t set, std::size_t bus,
                 nnsnd::AuxEffectLists& lists, ArchiveDescriptor& d)
{
    const std::string where =
        set == 0 ? "aux bus " + BusName(bus) : "aux bus " + BusName(bus) + " of effect set " + std::to_string(set);
    const std::optional<uint32_t> count = Read16(data, pos);
    if (!count)
    {
        return false;
    }

    for (uint32_t i = 0; i < *count && !d.unsupported_effect; i++)
    {
        const std::optional<std::vector<uint32_t>> kind = ReadWords(data, pos, 1);
        if (!kind)
        {
            return false;
        }

        if (!AuxEffect::Known(kind->front()))
        {
            d.unsupported_effect = "the archive descriptor gives " + where + " an effect of kind " +
                                   std::to_string(kind->front()) + "; this player supports kinds 1 to 7";
            return true;
        }

        AuxEffect effect;
        effect.kind = static_cast<AuxEffect::Kind>(kind->front());
        const std::optional<std::vector<uint32_t>> w = ReadWords(data, pos, effect.IsReverb() ? 11 : 4);
        if (!w)
        {
            return false;
        }

        const uint32_t flags = w->back();
        const uint32_t known = effect.IsReverb() ? kEffectSurround | kEffectHighPassDamping : kEffectSurround;
        if ((flags & ~known) != 0)
        {
            d.unsupported_effect =
                "the archive descriptor gives " + where + (effect.IsReverb() ? " a reverb" : " a delay") +
                " with flags " + xsflib::Hex(flags) + "; this player supports " +
                (effect.IsReverb() ? "bits 0 (surround) and 1 (high-pass damping)" : "bit 0 (surround)");
            return true;
        }

        if (effect.IsReverb())
        {
            ntdlib::ctr_fx::ReverbParam& r = effect.reverb;
            r.early_reflection_time = (*w)[0];
            r.fused_time = (*w)[1];
            r.pre_delay_time = (*w)[2];
            r.coloration = std::bit_cast<float>((*w)[3]);
            r.damping = std::bit_cast<float>((*w)[4]);
            r.filter_sizes = {(*w)[5], (*w)[6], (*w)[7]};
            r.early_gain = std::bit_cast<float>((*w)[8]);
            r.fused_gain = std::bit_cast<float>((*w)[9]);
            r.enable_surround = (flags & kEffectSurround) != 0;
            r.high_pass_damping = (flags & kEffectHighPassDamping) != 0;
        }
        else
        {
            ntdlib::ctr_fx::DelayParam& delay = effect.delay;
            delay.delay_time = (*w)[0];
            delay.feedback = std::bit_cast<float>((*w)[1]);
            delay.damping = std::bit_cast<float>((*w)[2]);
            delay.enable_surround = (flags & kEffectSurround) != 0;
        }

        lists[bus].push_back(effect);
    }

    return true;
}

} // namespace

std::vector<uint8_t> ProcessDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    Put32(v, kMagic);
    Put32(v, version);
    Put32(v, entry);
    Put32(v, stack_size);
    Put32(v, priority);
    Put32(v, app_memory);
    Put32(v, status_address);
    Put32(v, flags);

    return v;
}

std::optional<ProcessDescriptor> ProcessDescriptor::Parse(const std::vector<uint8_t>& data)
{
    if (data.size() < 32 || xsflib::LoadLe32(data.data()) != kMagic)
    {
        return std::nullopt;
    }

    ProcessDescriptor d;
    d.version = xsflib::LoadLe32(&data[4]);
    d.entry = xsflib::LoadLe32(&data[8]);
    d.stack_size = xsflib::LoadLe32(&data[12]);
    d.priority = xsflib::LoadLe32(&data[16]);
    d.app_memory = xsflib::LoadLe32(&data[20]);
    d.status_address = xsflib::LoadLe32(&data[24]);
    d.flags = xsflib::LoadLe32(&data[28]);

    return d;
}

std::optional<std::string> ProcessDescriptor::Check() const
{
    if (auto err = CheckDescriptorVersion("process", version, kVersion))
    {
        return err;
    }

    if (app_memory == 0)
    {
        return "the process descriptor gives the program no application memory";
    }

    if (app_memory > kMaxAppMemory)
    {
        return "the process descriptor asks for " + std::to_string(app_memory) +
               " bytes of application memory, more than the 178 MiB a 3DS gives a program";
    }

    return CheckDescriptorFlags("process", flags, kFlagDspRequired);
}

std::vector<uint8_t> ArchiveDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    Put32(v, kMagic);
    Put32(v, version);
    Put32(v, flags);
    Put32(v, output_mode);

    PutPath(v, archive_path);
    PutPath(v, firmware_path);
    if (flags & kFlagGroupFiles)
    {
        Put16(v, static_cast<uint32_t>(group_paths.size()));
        for (const std::string& path : group_paths)
        {
            PutPath(v, path);
        }
    }

    // The effects of bus A, then those of bus B. Version 3 then gives the further sets in the same way.
    const auto put = [&v](const nnsnd::AuxEffectLists& lists)
    {
        for (const std::vector<AuxEffect>& effects : lists)
        {
            Put16(v, static_cast<uint32_t>(effects.size()));
            for (const AuxEffect& effect : effects)
            {
                PutEffect(v, effect);
            }
        }
    };

    if (flags & kFlagAuxEffects)
    {
        put(aux_effects);
        if (version == kEffectSetsVersion)
        {
            Put16(v, static_cast<uint32_t>(effect_sets.size()));
            for (const nnsnd::AuxEffectLists& lists : effect_sets)
            {
                put(lists);
            }
        }
    }

    PadTo4(v);

    return v;
}

std::optional<ArchiveDescriptor> ArchiveDescriptor::Parse(const std::vector<uint8_t>& data)
{
    if (data.size() < 20 || xsflib::LoadLe32(data.data()) != kMagic)
    {
        return std::nullopt;
    }

    ArchiveDescriptor d;
    d.version = xsflib::LoadLe32(&data[4]);
    d.flags = xsflib::LoadLe32(&data[8]);
    d.output_mode = xsflib::LoadLe32(&data[12]);

    std::size_t pos = 16;
    const std::optional<std::string> archive = ReadPath(data, pos);
    const std::optional<std::string> firmware = archive ? ReadPath(data, pos) : std::nullopt;
    if (!firmware)
    {
        return std::nullopt;
    }

    d.archive_path = *archive;
    d.firmware_path = *firmware;

    // The group files' paths follow the firmware's with kFlagGroupFiles.
    if (d.flags & kFlagGroupFiles)
    {
        const std::optional<uint32_t> count = Read16(data, pos);
        if (!count)
        {
            return std::nullopt;
        }

        for (uint32_t i = 0; i < *count; i++)
        {
            std::optional<std::string> path = ReadPath(data, pos);
            if (!path)
            {
                return std::nullopt;
            }

            d.group_paths.push_back(std::move(*path));
        }
    }

    // The effects on the aux buses follow with kFlagAuxEffects. Version 3 then gives the further sets.
    const auto read = [&data, &pos, &d](std::size_t set, nnsnd::AuxEffectLists& lists)
    {
        for (std::size_t bus = 0; bus < lists.size() && !d.unsupported_effect; bus++)
        {
            if (!ReadEffects(data, pos, set, bus, lists, d))
            {
                return false;
            }
        }

        return true;
    };

    if (d.flags & kFlagAuxEffects)
    {
        if (!read(0, d.aux_effects))
        {
            return std::nullopt;
        }

        if (d.version == kEffectSetsVersion && !d.unsupported_effect)
        {
            const std::optional<uint32_t> count = Read16(data, pos);
            if (!count)
            {
                return std::nullopt;
            }

            d.effect_sets.resize(*count);
            for (std::size_t set = 0; set < d.effect_sets.size() && !d.unsupported_effect; set++)
            {
                if (!read(set + 1, d.effect_sets[set]))
                {
                    return std::nullopt;
                }
            }
        }
    }

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    // Versions 2 and 3 have version 1's flags and output modes. Version 3 gives its effect sets with the effects.
    if (version != kVersion && version != kListVersion && version != kEffectSetsVersion)
    {
        return "archive descriptor version " + std::to_string(version) + "; this player supports versions 1 to 3";
    }

    if (version == kEffectSetsVersion && !(flags & kFlagAuxEffects))
    {
        return "archive descriptor version 3 without bit 2 (aux effects) of its flags; version 3 gives its effect "
               "sets with the effects";
    }

    constexpr uint32_t kKnown = kFlagDspRequired | kFlagGroupFiles | kFlagAuxEffects | kFlagTimedStarts;
    if (!(flags & kFlagDspRequired) || (flags & ~kKnown) != 0)
    {
        return "the archive descriptor's flags are " + xsflib::Hex(flags) +
               "; this player requires bit 0 (DSP required) and supports bits 1 (group files), 2 (aux effects) and "
               "4 (timed starts)";
    }

    if (unsupported_effect)
    {
        return unsupported_effect;
    }

    // A set plays one set of effects. The effects' buffers come from the game's memory on a 3DS. The lines of the
    // firmware's delays and reverbs lie in the FCRAM of the archive-mode model, beside the wave archives. The libraries
    // refuse some parameters, and the models with them.
    for (std::size_t set = 0; set <= effect_sets.size(); set++)
    {
        const nnsnd::AuxEffectLists& lists = set == 0 ? aux_effects : effect_sets[set - 1];
        const std::string which = set == 0 ? "the archive descriptor's effects on the aux buses"
                                           : "the archive descriptor's effect set " + std::to_string(set);
        uint64_t bytes = 0;
        uint64_t line_bytes = 0;
        for (const std::vector<AuxEffect>& effects : lists)
        {
            for (const AuxEffect& effect : effects)
            {
                bytes += effect.BufferSamples() * sizeof(int32_t);
                if (bytes > ProcessDescriptor::kMaxAppMemory)
                {
                    return which +
                           (set == 0 ? " need more memory for their buffers" : " needs more memory for its buffers") +
                           " than the 178 MiB a 3DS gives a program";
                }

                line_bytes += effect.InFirmware() ? effect.BufferSamples() * sizeof(int32_t) : 0;
                if (line_bytes > kMaxFirmwareLines)
                {
                    return (set == 0 ? std::string("the archive descriptor's") : which + "'s") +
                           " firmware effects need lines of more than the 16 MiB that a player gives them";
                }
            }
        }

        try
        {
            const nnsnd::AuxEffects models(lists);
        }
        catch (const std::invalid_argument& e)
        {
            return which + ": " + e.what();
        }
    }

    return CheckDescriptorOutputMode("archive", output_mode);
}

std::vector<uint32_t> SoundsToPlay(const LoadedSet& set)
{
    if (!set.archive || set.sounds.empty())
    {
        return {};
    }

    if (set.archive->version == ArchiveDescriptor::kVersion)
    {
        return {set.sounds.front()};
    }

    return set.sounds;
}

uint32_t EffectSetOf(const Tags& tags)
{
    const auto tag = tags.find("3sf_aux");

    return tag != tags.end() ? ParseEffectSet(tag->second).value_or(0) : 0;
}

std::optional<uint32_t> ParseEffectSet(std::string_view text)
{
    text = TrimSpace(text);
    if (text.empty())
    {
        return std::nullopt;
    }

    uint32_t set = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }

        set = set * 10 + static_cast<uint32_t>(c - '0');
        if (set > 0xffff)
        {
            return std::nullopt;
        }
    }

    return set;
}

const nnsnd::AuxEffectLists* EffectSet(const ArchiveDescriptor& d, uint32_t set)
{
    if (set == 0)
    {
        return &d.aux_effects;
    }

    return set <= d.effect_sets.size() ? &d.effect_sets[set - 1] : nullptr;
}

std::optional<uint16_t> ParseTrackMask(std::string_view text)
{
    text = TrimSpace(text);
    if (text.empty())
    {
        return std::nullopt;
    }

    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        text.remove_prefix(2);
    }

    // from_chars takes no sign and no prefix for an unsigned value. Text such as "+1", "-1" or "0x-1" fails here.
    unsigned value = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value, base);
    if (error != std::errc() || stop != end || value > 0xffff)
    {
        return std::nullopt;
    }

    return static_cast<uint16_t>(value);
}

std::optional<Variables> ParseVariables(std::string_view text)
{
    constexpr std::string_view kSeparators = ", \t\r\n";

    Variables variables;
    std::size_t at = 0;
    while ((at = text.find_first_not_of(kSeparators, at)) != std::string_view::npos)
    {
        const std::size_t end = std::min(text.find_first_of(kSeparators, at), text.size());
        const std::optional<xsflib::Assignment> assignment = xsflib::ParseAssignment(text.substr(at, end - at));
        at = end;
        if (!assignment || assignment->index < 0 || assignment->index > kMaxVariable || assignment->value < INT16_MIN ||
            assignment->value > INT16_MAX)
        {
            return std::nullopt;
        }

        variables[static_cast<int>(assignment->index)] = static_cast<int16_t>(assignment->value);
    }

    if (variables.empty())
    {
        return std::nullopt;
    }

    return variables;
}

std::optional<std::vector<SequenceVariable>> ParseVariableChanges(std::string_view text)
{
    constexpr std::string_view kSeparators = ", \t\r\n";
    std::vector<SequenceVariable> changes;
    while (true)
    {
        const std::size_t first = text.find_first_not_of(kSeparators);
        if (first == std::string_view::npos)
        {
            break;
        }

        text.remove_prefix(first);
        const std::string_view item = text.substr(0, text.find_first_of(kSeparators));
        text.remove_prefix(item.size());
        const std::size_t colon = item.find(':');
        const std::size_t equals = item.find('=');
        if (colon == std::string_view::npos || equals == std::string_view::npos || equals < colon)
        {
            return std::nullopt;
        }

        const std::optional<int64_t> frame = ParseNumber(item.substr(0, colon));
        const std::optional<int64_t> index = ParseNumber(item.substr(colon + 1, equals - colon - 1));
        const std::optional<int64_t> value = ParseNumber(item.substr(equals + 1));
        if (!frame || !index || !value || *frame < 0 || *index < 0 || *index > kMaxVariable || *value < -32768 ||
            *value > 32767)
        {
            return std::nullopt;
        }

        changes.push_back({.index = static_cast<uint8_t>(*index),
                           .value = static_cast<int16_t>(*value),
                           .frame = static_cast<uint32_t>(*frame)});
    }

    if (changes.empty())
    {
        return std::nullopt;
    }

    return changes;
}

std::optional<uint32_t> ParseSeed(std::string_view text)
{
    text = TrimSpace(text);
    if (text.empty())
    {
        return std::nullopt;
    }

    const std::optional<int64_t> seed = ParseNumber(text);
    if (!seed || *seed < 0)
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(*seed);
}

std::optional<float> ParseRatio(std::string_view text)
{
    const std::optional<float> ratio = ParsePlainDecimal(text);
    if (!ratio || !(*ratio >= kMinRatio && *ratio <= kMaxRatio))
    {
        return std::nullopt;
    }

    return ratio;
}

std::optional<std::array<uint32_t, 4>> ParseBanks(std::string_view text)
{
    constexpr std::string_view kSeparators = ", \t\r\n";
    std::array<uint32_t, 4> banks;
    banks.fill(ntdlib::ctr::snd::kNoBank);
    std::size_t count = 0;
    std::size_t at = 0;
    while ((at = text.find_first_not_of(kSeparators, at)) != std::string_view::npos)
    {
        const std::size_t end = std::min(text.find_first_of(kSeparators, at), text.size());
        const std::optional<int64_t> id = ParseNumber(text.substr(at, end - at));
        at = end;
        if (count == banks.size() || !id || *id < 0 ||
            (*id >> 24 != ntdlib::sar::kBankItem && *id != ntdlib::ctr::snd::kNoBank))
        {
            return std::nullopt;
        }

        banks[count++] = static_cast<uint32_t>(*id);
    }

    if (count == 0)
    {
        return std::nullopt;
    }

    return banks;
}

std::optional<std::array<float, 2>> ParseSends(std::string_view text)
{
    std::array<float, 2> sends = {};
    const std::size_t comma = text.find(',');
    const std::size_t count = comma == std::string_view::npos ? 1 : 2;
    for (std::size_t bus = 0; bus < count; bus++)
    {
        const std::string_view part = bus == 0 ? text.substr(0, comma) : text.substr(comma + 1);
        const std::optional<float> send = ParsePlainDecimal(part);
        if (!send || !(*send >= 0.0f && *send <= 1.0f))
        {
            return std::nullopt;
        }

        sends[bus] = *send;
    }

    return sends;
}

std::optional<TrackVolumes> ParseTrackVolumes(std::string_view text)
{
    constexpr std::string_view kSeparators = ", \t\r\n";
    TrackVolumes volumes = ntdlib::ctr::snd::kFullTrackVolumes;
    bool any = false;
    std::size_t at = 0;
    while ((at = text.find_first_not_of(kSeparators, at)) != std::string_view::npos)
    {
        const std::size_t end = std::min(text.find_first_of(kSeparators, at), text.size());
        const std::string_view item = text.substr(at, end - at);
        at = end;
        const std::size_t equals = item.find('=');
        if (equals == std::string_view::npos)
        {
            return std::nullopt;
        }

        const std::optional<int64_t> track = ParseNumber(item.substr(0, equals));
        const std::optional<float> volume = ParsePlainDecimal(item.substr(equals + 1));
        if (!track || *track < 0 || *track >= static_cast<int64_t>(volumes.size()) || !volume ||
            !(*volume <= kMaxRatio))
        {
            return std::nullopt;
        }

        volumes[static_cast<std::size_t>(*track)] = *volume;
        any = true;
    }

    if (!any)
    {
        return std::nullopt;
    }

    return volumes;
}

SequenceStart SequenceStartOf(const Tags& tags)
{
    const auto mask = [&tags](const char* name)
    {
        const auto tag = tags.find(name);
        return tag != tags.end() ? ParseTrackMask(tag->second).value_or(0) : uint16_t{0};
    };

    SequenceStart start{.muted = mask("3sf_mute"), .silenced = mask("3sf_silence")};
    if (const auto tag = tags.find("3sf_seed"); tag != tags.end())
    {
        start.seed = ParseSeed(tag->second);
    }

    if (const auto tag = tags.find("3sf_tempo"); tag != tags.end())
    {
        start.tempo_ratio = ParseRatio(tag->second).value_or(1.0f);
    }

    if (const auto tag = tags.find("3sf_pitch"); tag != tags.end())
    {
        start.pitch = ParseRatio(tag->second).value_or(1.0f);
    }

    if (const auto tag = tags.find("3sf_banks"); tag != tags.end())
    {
        start.banks = ParseBanks(tag->second);
    }

    if (const auto tag = tags.find("3sf_fx_send"); tag != tags.end())
    {
        start.fx_send = ParseSends(tag->second).value_or(std::array<float, 2>{});
    }

    if (const auto tag = tags.find("3sf_track_volume"); tag != tags.end())
    {
        start.track_volumes = ParseTrackVolumes(tag->second).value_or(ntdlib::ctr::snd::kFullTrackVolumes);
    }

    return start;
}

std::vector<SequenceVariable> VariableChangesOf(const Tags& tags)
{
    const auto tag = tags.find("3sf_var_change");
    if (tag == tags.end())
    {
        return {};
    }

    return ParseVariableChanges(tag->second).value_or(std::vector<SequenceVariable>{});
}

void WriteSequenceStart(const SequenceStart& start, Tags& tags)
{
    const auto write = [&tags](const char* name, uint16_t mask)
    {
        if (mask != 0)
        {
            char text[8];
            std::snprintf(text, sizeof(text), "0x%04x", static_cast<unsigned>(mask));
            tags[name] = text;
        }
    };

    write("3sf_mute", start.muted);
    write("3sf_silence", start.silenced);

    // The variables of frame 0 join 3sf_var. The later ones go into 3sf_var_change, in the start's order.
    Variables variables;
    if (const auto tag = tags.find("3sf_var"); tag != tags.end())
    {
        variables = ParseVariables(tag->second).value_or(Variables{});
    }

    std::string changes;
    for (const SequenceVariable& variable : start.variables)
    {
        if (variable.index == SequenceVariable::kNone)
        {
            continue;
        }

        if (variable.frame == 0)
        {
            variables[variable.index] = variable.value;
            continue;
        }

        changes += (changes.empty() ? "" : ", ") + std::to_string(variable.frame) + ":" +
                   std::to_string(variable.index) + "=" + std::to_string(variable.value);
    }

    if (!variables.empty())
    {
        tags["3sf_var"] = FormatVariables(variables);
    }
    if (!changes.empty())
    {
        tags["3sf_var_change"] = changes;
    }

    if (start.seed)
    {
        char text[12];
        std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned>(*start.seed));
        tags["3sf_seed"] = text;
    }

    // The shortest decimal that reads back as the same float, in every locale: 1.3f gives "1.3". The volume tag reads
    // the same text.
    const auto decimal = [](float value)
    {
        char text[48];
        const auto [end, error] = std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed);

        return error == std::errc() ? std::string(text, end) : std::string();
    };

    const auto ratio = [&tags, &decimal](const char* name, float value)
    {
        if (value != 1.0f)
        {
            tags[name] = decimal(value);
        }
    };

    ratio("3sf_tempo", start.tempo_ratio);
    ratio("3sf_pitch", start.pitch);
    ratio("volume", start.volume);

    if (start.banks)
    {
        // The slots up to the last one with a bank. Banks that are all empty write one empty slot.
        std::size_t used = start.banks->size();
        while (used > 1 && (*start.banks)[used - 1] == ntdlib::ctr::snd::kNoBank)
        {
            used--;
        }

        std::string text;
        for (std::size_t slot = 0; slot < used; slot++)
        {
            char id[12];
            std::snprintf(id, sizeof(id), "0x%08x", static_cast<unsigned>((*start.banks)[slot]));
            text += (slot == 0 ? "" : ", ") + std::string(id);
        }
        tags["3sf_banks"] = text;
    }

    if (start.fx_send[0] != 0.0f || start.fx_send[1] != 0.0f)
    {
        tags["3sf_fx_send"] = decimal(start.fx_send[0]) + "," + decimal(start.fx_send[1]);
    }

    std::string volumes;
    for (std::size_t track = 0; track < start.track_volumes.size(); track++)
    {
        if (start.track_volumes[track] != 1.0f)
        {
            volumes +=
                (volumes.empty() ? "" : ", ") + std::to_string(track) + "=" + decimal(start.track_volumes[track]);
        }
    }
    if (!volumes.empty())
    {
        tags["3sf_track_volume"] = volumes;
    }
}

bool NeedsTimedStarts(const SequenceStart& start)
{
    // An older player ignores a 3sf_var tag with a track variable, as it ignores a later frame's variable.
    const auto later = [](const SequenceVariable& variable)
    {
        return variable.index != SequenceVariable::kNone && (variable.frame != 0 || variable.index >= 32);
    };

    return start.seed.has_value() || std::ranges::any_of(start.variables, later) || start.tempo_ratio != 1.0f ||
           start.pitch != 1.0f || start.banks.has_value() || start.fx_send[0] != 0.0f || start.fx_send[1] != 0.0f ||
           start.track_volumes != ntdlib::ctr::snd::kFullTrackVolumes;
}

} // namespace threesf
