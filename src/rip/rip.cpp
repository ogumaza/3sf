// SPDX-License-Identifier: MIT

// 3SF ripper (see rip.h and docs/3sf.md).

#include "rip/rip.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/ascii.h"
#include "xsflib/bytes.h"
#include "xsflib/psf/streaming.h"

#include "common/sar_options.h"
#include "dsp/dsp.h"
#include "rip/driver_blob.h"

namespace threesf::rip
{
namespace
{

constexpr uint32_t kDriverRegion = 0x1000; // one page: code, parameters and bss
static_assert(kDriverEnd - kDriverBase <= kDriverRegion, "the driver and its bss have outgrown their page");
static_assert(kDriverParams == DriverBlock::kAddress, "the driver's parameters aren't where the format has them");
constexpr uint32_t kStatusAddress = DriverBlock::kAddress + DriverBlock::kStatusOffset;
constexpr uint32_t kSoundIdAddress = DriverBlock::kAddress + DriverBlock::kSoundIdOffset;

using dsp::Dsp1Image;

uint32_t Get32(const std::vector<uint8_t>& v, std::size_t off)
{
    return xsflib::LoadLe32(v.data() + off);
}

void Set32(std::vector<uint8_t>& v, std::size_t off, uint32_t x)
{
    xsflib::StoreLe32(v.data() + off, x);
}

// True if a DSP1 image starts at `start` in `code`, its size and segments lie inside it, and it lies inside `code`.
bool IsDspImageAt(const std::vector<uint8_t>& code, std::size_t start)
{
    if (code.size() - start < Dsp1Image::kHeaderSize)
    {
        return false;
    }

    const uint32_t size = Get32(code, start + Dsp1Image::kSizeOffset);
    const unsigned count = code[start + Dsp1Image::kSegmentCountOffset];
    if (size < Dsp1Image::kHeaderSize || size > code.size() - start || count == 0 || count > Dsp1Image::kMaxSegments)
    {
        return false;
    }

    for (unsigned i = 0; i < count; i++)
    {
        const std::size_t record = start + Dsp1Image::kSegmentTableOffset + i * Dsp1Image::kSegmentRecordSize;
        const uint64_t offset = Get32(code, record);
        const uint64_t segment_size = Get32(code, record + Dsp1Image::kSegmentSizeOffset);
        if (offset < Dsp1Image::kHeaderSize || offset + segment_size > size)
        {
            return false;
        }
    }

    return true;
}

// The driver's page: the driver, and its parameters for `profile` in the order driver/driver.c's struct Params
// declares them.
std::vector<uint8_t> DriverPage(const GameProfile& profile)
{
    std::vector<uint8_t> driver(kDriverRegion, 0);
    std::memcpy(driver.data(), kDriverBlob, sizeof(kDriverBlob));
    constexpr std::size_t kParamsOffset = kDriverParams - kDriverBase;
    const uint32_t params[] = {
        DriverBlock::kMagic,    // magic: "3SFD"
        DriverBlock::kVersion,  // version
        0,                      // status
        0,                      // sound id
        kOutputStereo,          // output mode
        profile.handle,         // sound handle slot
        profile.data_heap_size, // device memory for the sound's data
        16713680,               // update period in nanoseconds: one 3DS video frame (268111856 / 4481136 Hz)
        // The game's profile.
        profile.fs_init,
        profile.rom_size,
        profile.rom_mount,
        profile.sound_create,
        profile.set_output,
        profile.heap_create,
        profile.load_data,
        profile.play,
        profile.update,
        profile.heap_manager,
        profile.system_offset,
        profile.handles_offset,
        profile.heap_base,
        profile.heap_size,
        profile.device_base,
        profile.device_size,
    };
    for (std::size_t i = 0; i < std::size(params); i++)
    {
        xsflib::StoreLe32(driver.data() + kParamsOffset + 4 * i, params[i]);
    }

    return driver;
}

} // namespace

const std::vector<GameProfile>& KnownProfiles()
{
    static const std::vector<GameProfile> kProfiles = {
        {
            "Pokemon X (Japan)",
            0x0004000000055d00ull,
            0x3778c475,
            0x00106138, // nnMain
            0x0010e8f8, // gfl fs init
            0x0010ea08, // nn::fs::GetRomRequiredMemorySize wrapper
            0x0010eb90, // nn::fs::MountRom("rom:") wrapper
            0x0011502c, // game sound manager init (gfl::snd::SoundSystem and friends)
            0x00111dc0, // sound output mode
            0x0011e1f0, // gfl::snd::SoundHeap
            0x0011da94, // gfl::snd::SoundSystem::LoadData
            0x0038da90, // gfl::snd::SoundSystem::Play
            0x0011b920, // gfl::snd::SoundSystem::Update
            0x005d10d8, // gfl heap manager
            0x04,       // sound system in the manager object
            0x600,      // handle array in the sound system
            0x08100000, // start and size of the heap the startup code leaves over
            0x00cf0000,
            0x14100000, // start and size of the linear (device) memory the startup code leaves over
            0x02a48000,
            15,        // first of the game's sound-request handle slots
            16u << 20, // data heap
            "sound/xy_sound.bcsar",
            "sound/dspaudio.cdc",
        },
    };
    return kProfiles;
}

const GameProfile* FindProfile(const GameFiles& game)
{
    const uint32_t crc = game.CodeCrc32();
    for (const auto& p : KnownProfiles())
    {
        if (p.program_id == game.ProgramId() && p.code_crc32 == crc)
        {
            return &p;
        }
    }

    return nullptr;
}

std::vector<std::string> FirmwareCandidates(const GameFiles& game, const GameProfile* profile)
{
    const std::string_view code(reinterpret_cast<const char*>(game.code.data()), game.code.size());
    std::vector<std::string> named, dspaudio, others;
    for (const auto& path : game.romfs_files)
    {
        const std::string lower = xsflib::AsciiLower(path);
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".cdc") != 0)
        {
            continue;
        }

        if (code.find("/" + path) != std::string_view::npos)
        {
            named.push_back(path);
        }
        else if (lower == "dspaudio.cdc" ||
                 (lower.size() > 13 && lower.compare(lower.size() - 13, 13, "/dspaudio.cdc") == 0))
        {
            dspaudio.push_back(path);
        }
        else
        {
            others.push_back(path);
        }
    }

    std::vector<std::string> out;
    if (profile)
    {
        out.push_back(profile->dsp_component);
    }

    for (const auto* list : {&named, &dspaudio, &others})
    {
        out.insert(out.end(), list->begin(), list->end());
    }

    return out;
}

std::optional<std::size_t> FindFirmwareInCode(const std::vector<uint8_t>& code)
{
    const std::string_view bytes(reinterpret_cast<const char*>(code.data()), code.size());
    for (std::size_t magic = bytes.find("DSP1", Dsp1Image::kMagicOffset); magic != std::string_view::npos;
         magic = bytes.find("DSP1", magic + 1))
    {
        if (IsDspImageAt(code, magic - Dsp1Image::kMagicOffset))
        {
            return magic - Dsp1Image::kMagicOffset;
        }
    }

    return std::nullopt;
}

std::optional<GameFirmware> FindGameFirmware(const GameFiles& game, const GameProfile* profile)
{
    for (const auto& path : FirmwareCandidates(game, profile))
    {
        std::vector<uint8_t> data;
        if (game.read_romfs(path, data) && IsDspFirmware(data))
        {
            return GameFirmware{path, 0, std::move(data)};
        }
    }

    const auto offset = FindFirmwareInCode(game.code);
    if (!offset)
    {
        return std::nullopt;
    }

    const auto begin = game.code.begin() + static_cast<std::ptrdiff_t>(*offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(Get32(game.code, *offset + Dsp1Image::kSizeOffset));
    return GameFirmware{"", *offset, std::vector<uint8_t>(begin, end)};
}

std::optional<std::vector<uint8_t>> BuildLibrary(const GameFiles& game, const GameProfile& profile, const Tags& tags,
                                                 std::string* error)
{
    const auto& exh = game.exheader;
    const uint32_t text = Get32(exh, kExheaderTextAddress);
    const uint32_t data = Get32(exh, kExheaderDataAddress);
    const uint32_t data_size = Get32(exh, kExheaderDataSize);
    const uint32_t bss = Get32(exh, kExheaderBssSize);
    const uint64_t end = (uint64_t{data} + data_size + bss + 0xfff) & ~uint64_t{0xfff};
    if (profile.nnmain < text || uint64_t{profile.nnmain} + 8 > end || end > kDriverBase)
    {
        if (error)
        {
            *error = "the exheader's memory layout doesn't fit the game's profile";
        }

        return std::nullopt;
    }

    // Code, rodata, data and bss (zeros, so the pages are mapped). nnMain is patched to jump to the driver: its first
    // word becomes "ldr pc, [pc, #-4]" and its second the driver's entry point.
    std::vector<uint8_t> image(static_cast<std::size_t>(end - text), 0);
    std::memcpy(image.data(), game.code.data(), std::min<std::size_t>(game.code.size(), image.size()));
    Set32(image, profile.nnmain - text, 0xe51ff004);
    Set32(image, profile.nnmain - text + 4, kDriverEntry);

    ProgramBuilder program;
    program.AddMemory(text, image);
    program.AddMemory(kDriverBase, DriverPage(profile));

    for (const char* path : {profile.archive, profile.dsp_component})
    {
        std::vector<uint8_t> bytes;
        if (!game.read_romfs || !game.read_romfs(path, bytes))
        {
            if (error)
            {
                *error = std::string("can't read ") + path + " from the RomFS";
            }

            return std::nullopt;
        }

        program.AddFile(path, bytes);
    }

    ProcessDescriptor d;
    d.entry = text;
    d.stack_size = Get32(exh, kExheaderStackSize);
    d.priority = exh[kExheaderPriority];
    d.app_memory = 0x04000000;
    d.status_address = kStatusAddress;

    return WritePsf(kPsfVersion3sf, d.Serialize(), program.Data(), tags);
}

std::vector<uint8_t> BuildMini(uint32_t sound_id, const std::string& lib_name, Tags tags)
{
    uint8_t id[4];
    xsflib::StoreLe32(id, sound_id);
    ProgramBuilder program;
    program.AddMemory(kSoundIdAddress, id, sizeof(id));

    tags["_lib"] = lib_name;

    return WritePsf(kPsfVersion3sf, {}, program.Data(), tags);
}

std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& firmware_path, const std::vector<uint8_t>& firmware,
                                         const Tags& tags)
{
    ArchiveDescriptor descriptor;
    descriptor.archive_path = archive_path;
    descriptor.firmware_path = firmware_path;

    ProgramBuilder program;
    program.AddFile(archive_path, archive);
    program.AddFile(firmware_path, firmware);

    return WritePsf(kPsfVersion3sf, descriptor.Serialize(), program.Data(), tags);
}

namespace
{

// ntdlib's minis with 3SF's version byte. xsflib's streaming builder writes the same bytes as WritePsf.
struct ArchiveMiniFormat
{
    using ProgramBuilder = xsflib::psf::StreamingProgramBuilder;

    static constexpr uint8_t kPsfVersion = kPsfVersion3sf;
};

} // namespace

std::vector<uint8_t> BuildArchiveMini(uint32_t sound_id, const std::string& lib_name, Tags tags)
{
    return ntdlib::rip::BuildMini<ArchiveMiniFormat>(ntdlib::sar::ItemNumber(sound_id), lib_name, std::move(tags));
}

bool IsDspFirmware(const std::vector<uint8_t>& data)
{
    return data.size() >= Dsp1Image::kHeaderSize && std::memcmp(data.data() + Dsp1Image::kMagicOffset, "DSP1", 4) == 0;
}

std::vector<std::string> MissingData(const ntdlib::sar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size())
    {
        return {"the sound itself"};
    }

    const auto name_of = [](const std::string& name, std::size_t i)
    {
        return name.empty() ? "#" + std::to_string(i) : name;
    };

    const ntdlib::sar::SoundInfo& info = archive.Sounds()[index];
    std::vector<std::string> missing;
    if (archive.FileData(info.file_id).empty())
    {
        missing.push_back("its sequence data");
    }

    std::vector<uint32_t> wave_archives;
    for (uint32_t bank_item : info.banks)
    {
        const std::optional<std::size_t> b =
            ntdlib::sar::ItemIndex(bank_item, ntdlib::sar::kBankItem, archive.Banks().size());
        if (!b)
        {
            continue;
        }

        const auto bytes = archive.FileData(archive.Banks()[*b].file_id);
        if (bytes.empty())
        {
            missing.push_back("bank " + name_of(archive.Banks()[*b].name, *b));
            continue;
        }

        try
        {
            for (const ntdlib::sar::WaveId& w : ntdlib::sar::Bank::Parse(bytes, kSarOptions).waves)
            {
                if (std::find(wave_archives.begin(), wave_archives.end(), w.wave_archive_item) == wave_archives.end())
                {
                    wave_archives.push_back(w.wave_archive_item);
                }
            }
        }
        catch (const std::exception&)
        {
            missing.push_back("bank " + name_of(archive.Banks()[*b].name, *b) + " (unreadable)");
        }
    }

    for (uint32_t item : wave_archives)
    {
        const std::optional<std::size_t> w =
            ntdlib::sar::ItemIndex(item, ntdlib::sar::kWaveArchiveItem, archive.WaveArchives().size());
        if (!w)
        {
            continue;
        }

        if (archive.FileData(archive.WaveArchives()[*w].file_id).empty())
        {
            missing.push_back("wave archive " + name_of(archive.WaveArchives()[*w].name, *w));
        }
    }

    return missing;
}

} // namespace threesf::rip
