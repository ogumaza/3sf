// SPDX-License-Identifier: MIT

// The 3SF file format (see format.h and docs/3sf.md).

#include "threesf/format.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ctr_fx/effects.h"
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

// Reads bus `bus`'s effects at `pos` into `d`. Returns false if `data` ends first. An effect that this player can't
// play sets d.unsupported_effect, and the effects after it aren't read.
bool ReadEffects(const std::vector<uint8_t>& data, std::size_t& pos, std::size_t bus, ArchiveDescriptor& d)
{
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
            d.unsupported_effect = "the archive descriptor gives aux bus " + BusName(bus) + " an effect of kind " +
                                   std::to_string(kind->front()) + "; this player supports kinds 1 to 4";
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
                "the archive descriptor gives aux bus " + BusName(bus) +
                (effect.IsReverb() ? " a reverb" : " a delay") + " with flags " + xsflib::Hex(flags) +
                "; this player supports " +
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

        d.aux_effects[bus].push_back(effect);
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

    // The effects of bus A, then those of bus B.
    if (flags & kFlagAuxEffects)
    {
        for (const std::vector<AuxEffect>& effects : aux_effects)
        {
            Put16(v, static_cast<uint32_t>(effects.size()));
            for (const AuxEffect& effect : effects)
            {
                PutEffect(v, effect);
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

    // The effects on the aux buses follow with kFlagAuxEffects.
    if (d.flags & kFlagAuxEffects)
    {
        for (std::size_t bus = 0; bus < d.aux_effects.size() && !d.unsupported_effect; bus++)
        {
            if (!ReadEffects(data, pos, bus, d))
            {
                return std::nullopt;
            }
        }
    }

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    // Version 2 has version 1's flags and output modes.
    if (version != kVersion && version != kListVersion)
    {
        return "archive descriptor version " + std::to_string(version) + "; this player supports versions 1 and 2";
    }

    if (!(flags & kFlagDspRequired) || (flags & ~(kFlagDspRequired | kFlagGroupFiles | kFlagAuxEffects)) != 0)
    {
        return "the archive descriptor's flags are " + xsflib::Hex(flags) +
               "; this player requires bit 0 (DSP required) and supports bits 1 (group files) and 2 (aux effects)";
    }

    if (unsupported_effect)
    {
        return unsupported_effect;
    }

    // The effects' buffers come from the game's memory on a 3DS.
    uint64_t bytes = 0;
    for (const std::vector<AuxEffect>& effects : aux_effects)
    {
        for (const AuxEffect& effect : effects)
        {
            bytes += effect.BufferSamples() * sizeof(int32_t);
            if (bytes > ProcessDescriptor::kMaxAppMemory)
            {
                return "the archive descriptor's effects on the aux buses need more memory for their buffers than the "
                       "178 MiB a 3DS gives a program";
            }
        }
    }

    // The libraries refuse some parameters, and the models with them.
    try
    {
        const nnsnd::AuxEffects models(aux_effects);
    }
    catch (const std::invalid_argument& e)
    {
        return std::string("the archive descriptor's effects on the aux buses: ") + e.what();
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

} // namespace threesf
