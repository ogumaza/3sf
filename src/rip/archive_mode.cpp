// SPDX-License-Identifier: MIT

// The rips of archive mode: the library of a sound archive and its firmware, and the minis (see rip.h and docs/3sf.md,
// "Archive mode").

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/psf/streaming.h"

#include "nnsnd/aux_effects.h"
#include "rip/rip.h"
#include "threesf/format.h"

namespace threesf::rip
{
namespace
{

// ntdlib's minis with 3SF's version byte. xsflib's streaming builder writes the same bytes as WritePsf.
struct ArchiveMiniFormat
{
    using ProgramBuilder = xsflib::psf::StreamingProgramBuilder;

    static constexpr uint8_t kPsfVersion = kPsfVersion3sf;
};

} // namespace

std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& firmware_path, const std::vector<uint8_t>& firmware,
                                         const Tags& tags, const std::vector<FileChunk>& groups, uint32_t version,
                                         const nnsnd::AuxEffectLists& effects, bool timed_starts,
                                         const std::vector<nnsnd::AuxEffectLists>& effect_sets)
{
    ArchiveDescriptor descriptor;
    descriptor.version = version;
    descriptor.archive_path = archive_path;
    descriptor.firmware_path = firmware_path;
    if (!groups.empty())
    {
        descriptor.flags |= kFlagGroupFiles;
    }

    // Effect sets go with the effects, in version 3 (docs/3sf.md, "Effect sets").
    if (nnsnd::HasEffects(effects) || !effect_sets.empty())
    {
        descriptor.flags |= kFlagAuxEffects;
        descriptor.aux_effects = effects;
    }
    if (!effect_sets.empty())
    {
        descriptor.version = ArchiveDescriptor::kEffectSetsVersion;
        descriptor.effect_sets = effect_sets;
    }

    if (timed_starts)
    {
        descriptor.flags |= kFlagTimedStarts;
    }

    ProgramBuilder program;
    program.AddFile(archive_path, archive);
    program.AddFile(firmware_path, firmware);
    for (const FileChunk& group : groups)
    {
        descriptor.group_paths.push_back(group.path);
        program.AddFile(group.path, group.data);
    }

    return WritePsf(kPsfVersion3sf, descriptor.Serialize(), program.Data(), tags);
}

std::vector<uint8_t> BuildArchiveMini(uint32_t sound_id, const std::string& lib_name, Tags tags)
{
    return ntdlib::rip::BuildMini<ArchiveMiniFormat>(ntdlib::sar::ItemNumber(sound_id), lib_name, std::move(tags));
}

std::vector<uint8_t> BuildArchiveMini(const std::vector<uint32_t>& sound_ids, const std::string& lib_name, Tags tags)
{
    std::vector<uint32_t> indices;
    for (const uint32_t id : sound_ids)
    {
        indices.push_back(ntdlib::sar::ItemNumber(id));
    }

    return ntdlib::rip::BuildMiniOfSounds<ArchiveMiniFormat>(indices, lib_name, std::move(tags));
}

} // namespace threesf::rip
