// SPDX-License-Identifier: MIT

// 3SF ripper (see rip.h and docs/3sf.md).

#include "rip/rip.h"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/ascii.h"
#include "common/bytes.h"
#include "csar/formats.h"
#include "dsp/dsp.h"
#include "nwsnd/engine.h"
#include "nwsnd/sequence.h"
#include "rip/driver_blob.h"

namespace threesf::rip
{
namespace
{

constexpr uint32_t kDriverRegion = 0x1000; // one page: code, parameters and bss
static_assert(kDriverEnd - kDriverBase <= kDriverRegion, "the driver and its bss have outgrown their page");
constexpr uint32_t kStatusAddress = kDriverParams + 0x08;
constexpr uint32_t kSoundIdAddress = kDriverParams + 0x0c;

// Length analysis: a looping sequence gets kLoops loops and a kFadeSeconds fade, and any sequence stops after
// kMaxSeconds, which is kMaxFrames sound frames.
constexpr int kLoops = 2;
constexpr double kFadeSeconds = 10.0;
constexpr double kMaxSeconds = 600.0;
constexpr uint64_t kMaxFrames = static_cast<uint64_t>(kMaxSeconds * dsp::kSampleRate / dsp::kSamplesPerFrame);

// A frame's length in the tags. They count time at the rate players report rather than the DSP's exact rate, so that a
// player that turns a length back into samples stops at the frame the analysis found.
constexpr double kFrameMs = dsp::kSamplesPerFrame * 1000.0 / kPlayerSampleRate;

// A DSP1 firmware image: a 0x300-byte header, with "DSP1" after the signature, the image's size, the number of segments
// and a table of up to ten 0x30-byte segment records (offset in the image, DSP address, size, ...), then the segments.
constexpr std::size_t kDspHeaderSize = 0x300;
constexpr std::size_t kDspMagicOffset = 0x100;
constexpr std::size_t kDspSizeOffset = 0x104;
constexpr std::size_t kDspSegmentCountOffset = 0x10e;
constexpr std::size_t kDspSegmentTableOffset = 0x120;
constexpr std::size_t kDspSegmentRecordSize = 0x30;
constexpr unsigned kDspMaxSegments = 10;

uint32_t Get32(const std::vector<uint8_t>& v, std::size_t off)
{
    return LoadLe32(v.data() + off);
}

void Set32(std::vector<uint8_t>& v, std::size_t off, uint32_t x)
{
    StoreLe32(v.data() + off, x);
}

// True if a DSP1 image starts at `start` in `code`, its size and segments lie inside it, and it lies inside `code`.
bool IsDspImageAt(const std::vector<uint8_t>& code, std::size_t start)
{
    if (code.size() - start < kDspHeaderSize)
    {
        return false;
    }

    const uint32_t size = Get32(code, start + kDspSizeOffset);
    const unsigned count = code[start + kDspSegmentCountOffset];
    if (size < kDspHeaderSize || size > code.size() - start || count == 0 || count > kDspMaxSegments)
    {
        return false;
    }

    for (unsigned i = 0; i < count; i++)
    {
        const std::size_t record = start + kDspSegmentTableOffset + i * kDspSegmentRecordSize;
        const uint64_t offset = Get32(code, record);
        const uint64_t segment_size = Get32(code, record + 8);
        if (offset < kDspHeaderSize || offset + segment_size > size)
        {
            return false;
        }
    }

    return true;
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
    const uint32_t crc = static_cast<uint32_t>(crc32(0, game.code.data(), static_cast<uInt>(game.code.size())));
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
        const std::string lower = AsciiLower(path);
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
    for (std::size_t magic = bytes.find("DSP1", kDspMagicOffset); magic != std::string_view::npos;
         magic = bytes.find("DSP1", magic + 1))
    {
        if (IsDspImageAt(code, magic - kDspMagicOffset))
        {
            return magic - kDspMagicOffset;
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
    const auto end = begin + static_cast<std::ptrdiff_t>(Get32(game.code, *offset + kDspSizeOffset));
    return GameFirmware{"", *offset, std::vector<uint8_t>(begin, end)};
}

std::optional<std::vector<uint8_t>> BuildLibrary(const GameFiles& game, const GameProfile& profile, const Tags& tags,
                                                 std::string* error)
{
    const auto& exh = game.exheader;
    const uint32_t text = Get32(exh, 0x10);
    const uint32_t data = Get32(exh, 0x30);
    const uint32_t data_size = Get32(exh, 0x38);
    const uint32_t bss = Get32(exh, 0x3c);
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

    // The driver's page, with its parameters in the order driver/driver.c's struct Params declares them.
    std::vector<uint8_t> driver(kDriverRegion, 0);
    std::memcpy(driver.data(), kDriverBlob, sizeof(kDriverBlob));
    constexpr std::size_t kParamsOffset = kDriverParams - kDriverBase;
    static_assert(kDriverParams == DriverBlock::kAddress, "the driver's parameters aren't where the format has them");
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
        StoreLe32(driver.data() + kParamsOffset + 4 * i, params[i]);
    }

    ProgramBuilder program;
    program.AddMemory(text, image);
    program.AddMemory(kDriverBase, driver);

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
    d.stack_size = Get32(exh, 0x1c);
    d.priority = exh[0x20f];
    d.app_memory = 0x04000000;
    d.status_address = kStatusAddress;

    return WritePsf(kPsfVersion3sf, d.Serialize(), program.Data(), tags);
}

std::vector<uint8_t> BuildMini(uint32_t sound_id, const std::string& lib_name, Tags tags)
{
    uint8_t id[4];
    StoreLe32(id, sound_id);
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

std::vector<uint8_t> BuildArchiveMini(uint32_t sound_id, const std::string& lib_name, Tags tags)
{
    ProgramBuilder program;
    program.AddSound(sound_id);

    tags["_lib"] = lib_name;

    return WritePsf(kPsfVersion3sf, {}, program.Data(), tags);
}

bool IsDspFirmware(const std::vector<uint8_t>& data)
{
    return data.size() >= kDspHeaderSize && std::memcmp(data.data() + kDspMagicOffset, "DSP1", 4) == 0;
}

std::vector<std::string> MissingData(const csar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size())
    {
        return {"the sound itself"};
    }

    const auto name_of = [](const std::string& name, uint32_t i)
    {
        return name.empty() ? "#" + std::to_string(i) : name;
    };

    const csar::SoundInfo& info = archive.Sounds()[index];
    std::vector<std::string> missing;
    if (archive.FileData(info.file_id).empty())
    {
        missing.push_back("its sequence data");
    }

    std::vector<uint32_t> wave_archives;
    for (uint32_t bank_item : info.banks)
    {
        const uint32_t b = bank_item & 0xFFFFFF;
        if ((bank_item >> 24) != 0x03 || b >= archive.Banks().size())
        {
            continue;
        }

        const auto bytes = archive.FileData(archive.Banks()[b].file_id);
        if (bytes.empty())
        {
            missing.push_back("bank " + name_of(archive.Banks()[b].name, b));
            continue;
        }

        try
        {
            for (const csar::WaveId& w : csar::Bank::Parse(bytes).waves)
            {
                if (std::find(wave_archives.begin(), wave_archives.end(), w.wave_archive_item) == wave_archives.end())
                {
                    wave_archives.push_back(w.wave_archive_item);
                }
            }
        }
        catch (const std::exception&)
        {
            missing.push_back("bank " + name_of(archive.Banks()[b].name, b) + " (unreadable)");
        }
    }

    for (uint32_t item : wave_archives)
    {
        const uint32_t w = item & 0xFFFFFF;
        if ((item >> 24) != 0x05 || w >= archive.WaveArchives().size())
        {
            continue;
        }

        if (archive.FileData(archive.WaveArchives()[w].file_id).empty())
        {
            missing.push_back("wave archive " + name_of(archive.WaveArchives()[w].name, w));
        }
    }

    return missing;
}

namespace
{

// Plays a sequence on the model until it ends, loops kLoops times or reaches kMaxSeconds.
std::optional<Timing> MeasureSequence(const csar::SoundArchive& archive, const std::vector<uint8_t>& cdc,
                                      uint32_t sound_index)
{
    nwsnd::ArchiveModel model;
    if (!model.Start(cdc, archive))
    {
        return std::nullopt;
    }

    nwsnd::Engine& engine = *model.engine;
    nwsnd::SequenceSoundPlayer* player = engine.StartSequence(sound_index);
    if (!player)
    {
        return std::nullopt;
    }

    int loops_seen = 0;
    uint64_t loop_frame = 0;
    player->on_loop_ = [&]
    {
        loops_seen++;
        if (loops_seen == kLoops)
        {
            loop_frame = engine.FrameCount();
        }
    };

    uint64_t idle = 0;
    while (engine.FrameCount() < kMaxFrames)
    {
        if (!engine.RunFrame())
        {
            return std::nullopt;
        }

        if (loops_seen >= kLoops)
        {
            return Timing{static_cast<long long>(loop_frame * kFrameMs + 0.5),
                          static_cast<long long>(kFadeSeconds * 1000)};
        }

        if (!engine.IsBusy())
        {
            if (++idle > 10)
            {
                break;
            }
        }
        else
        {
            idle = 0;
        }
    }
    if (engine.FrameCount() >= kMaxFrames)
    {
        return Timing{static_cast<long long>(kMaxSeconds * 1000), static_cast<long long>(kFadeSeconds * 1000)};
    }

    // Ended on its own: length to the end plus a short tail, no fade.
    const uint64_t end_frame = engine.FrameCount() - idle;
    return Timing{static_cast<long long>(end_frame * kFrameMs + 500.5), 0};
}

} // namespace

struct LengthAnalyzer::Impl
{
    csar::SoundArchive archive;
    std::vector<uint8_t> cdc;
};

LengthAnalyzer::LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const std::vector<uint8_t>& dsp_component)
    : impl_(new Impl{csar::SoundArchive::Load(archive_bytes), dsp_component})
{
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index) const
{
    // A sequence the model can't play, with a damaged bank for instance, gets no length. Running out of memory isn't
    // the sequence's fault, so std::bad_alloc goes to the caller.
    try
    {
        return MeasureSequence(impl_->archive, impl_->cdc, sound_index);
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

namespace
{

// ASCII letters, digits, '_', '-' and '.' are kept, and everything else, spaces included, becomes '_'. Unlike
// std::isalnum, this doesn't depend on the C locale.
std::string SafeName(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                          c == '-' || c == '.';
        out += keep ? c : '_';
    }

    return out;
}

// Windows reserves these device names, whatever follows the first dot of a file name.
bool ReservedOnWindows(const std::string& stem)
{
    const std::string name = AsciiLower(stem);
    const bool numbered =
        name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) && name[3] >= '0' && name[3] <= '9';
    return name == "con" || name == "prn" || name == "aux" || name == "nul" || numbered;
}

} // namespace

std::string FileNames::Library(const std::string& base)
{
    std::string name = SafeName(base);
    const std::size_t stem_length = std::min(name.find('.'), name.size());
    if (ReservedOnWindows(name.substr(0, stem_length)))
    {
        name.insert(stem_length, "_");
    }

    return Unique(name, ".3sflib");
}

std::string FileNames::Mini(uint32_t index, const std::string& label)
{
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "%04u ", static_cast<unsigned>(index));

    return Unique(prefix + SafeName(label), ".mini3sf");
}

std::string FileNames::Unique(const std::string& stem, const std::string& extension)
{
    std::string name = stem + extension;
    for (int n = 2; !taken_.insert(AsciiLower(name)).second; n++)
    {
        name = stem + "_" + std::to_string(n) + extension;
    }

    return name;
}

} // namespace threesf::rip
