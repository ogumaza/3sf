// SPDX-License-Identifier: MIT

// The 3SF file format (see format.h and docs/3sf.md).

#include "threesf/format.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/streaming.h"

namespace threesf
{
namespace
{

using xsflib::psf::CheckDescriptorFlags;
using xsflib::psf::CheckDescriptorHeader;
using xsflib::psf::CheckDescriptorVersion;
using xsflib::psf::PadTo4;
using xsflib::psf::Put32;
using xsflib::psf::PutPath;
using xsflib::psf::ReadPath;

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

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    return CheckDescriptorHeader("archive", version, flags, output_mode, kFlagDspRequired);
}

} // namespace threesf
