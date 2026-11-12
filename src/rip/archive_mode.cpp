// SPDX-License-Identifier: MIT

// The rips of archive mode: the library of a sound archive and its firmware, the minis, and the search for an archive's
// group files (see rip.h and docs/3sf.md, "Archive mode").

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/files.h"
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
    if (sound_ids.size() == 1)
    {
        return BuildArchiveMini(sound_ids.front(), lib_name, std::move(tags));
    }

    ArchiveMiniFormat::ProgramBuilder program;
    program.AddSounds(sound_ids);
    tags["_lib"] = lib_name;

    return program.WritePsf(ArchiveMiniFormat::kPsfVersion, {}, tags);
}

std::optional<FileChunk> FindGroupIn(const std::filesystem::path& folder, const std::string& name)
{
    // A folder that the walk can't open, such as System Volume Information at the root of an NTFS drive, would end the
    // walk without skip_permission_denied.
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator
             it(folder, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec))
    {
        std::error_code type_error;
        if (!it->is_regular_file(type_error) ||
            !ntdlib::sar::IsGroupFileName(xsflib::Utf8(it->path().filename()), name, ntdlib::sar::Flavor::kCtr))
        {
            continue;
        }

        FileChunk group;
        for (const std::filesystem::path& part : it->path().lexically_relative(folder))
        {
            group.path += (group.path.empty() ? "" : "/") + xsflib::Utf8(part);
        }

        if (xsflib::ReadFile(it->path(), group.data))
        {
            return group;
        }
    }

    return std::nullopt;
}

} // namespace threesf::rip
