// SPDX-License-Identifier: MIT

// 3SF ripper (see rip.h and docs/3sf.md).

#include "rip/rip.h"

#include <algorithm>
#include <array>
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
#include "xsflib/rip/length.h"
#include <zlib.h>

#include "common/sar_options.h"
#include "dsp/dsp.h"
#include "nnsnd/dsp_abi.h"
#include "nnsnd/sound_system.h"
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

// Length analysis (xsflib/rip/length.h): any sequence stops after kMaxSeconds. kMaxFrames is that time in sound frames.
constexpr double kMaxSeconds = 600.0;
constexpr uint64_t kMaxFrames = static_cast<uint64_t>(kMaxSeconds * dsp::kSampleRate / dsp::kSamplesPerFrame);

// A sequence that stays busy without a sound for this long has ended where it fell silent.
constexpr double kSilentSeconds = 180.0;
constexpr uint64_t kSilentFrames = static_cast<uint64_t>(kSilentSeconds * dsp::kSampleRate / dsp::kSamplesPerFrame);

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
    return xsflib::LoadLe32(v.data() + off);
}

void Set32(std::vector<uint8_t>& v, std::size_t off, uint32_t x)
{
    xsflib::StoreLe32(v.data() + off, x);
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
        xsflib::StoreLe32(driver.data() + kParamsOffset + 4 * i, params[i]);
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
    return ntdlib::rip::BuildMini<ArchiveMiniFormat>(sound_id & 0xFFFFFF, lib_name, std::move(tags));
}

bool IsDspFirmware(const std::vector<uint8_t>& data)
{
    return data.size() >= kDspHeaderSize && std::memcmp(data.data() + kDspMagicOffset, "DSP1", 4) == 0;
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

namespace
{

// The length analysis's rules. 3SF counts a loop's point from the frame that runs it. A loop's sound and a steady
// sequence's loop count from the next frame.
xsflib::rip::LengthRules LengthRules()
{
    xsflib::rip::LengthRules rules;
    rules.frame_ms = kFrameMs;
    rules.max_frames = kMaxFrames;
    rules.silent_frames = kSilentFrames;
    rules.max_ms = kMaxSeconds * 1000.0;
    rules.loop_offset = 0;

    return rules;
}

// The model of nw::snd and nn::snd on the DSP, for the length analysis. The DSP's sample sink sets `audible` when a
// frame makes a sound.
class LengthModel
{
public:
    LengthModel(nwsnd::ArchiveModel& model, nwsnd::SequenceSoundPlayer& player, bool& audible)
        : model_(model), engine_(*model.engine), player_(player), audible_(audible)
    {
    }

    bool RunFrame()
    {
        audible_ = false;

        return engine_.RunFrame();
    }

    bool Sounded() const
    {
        return audible_;
    }

    // The analysis doesn't wait for effects to ring out.
    bool Ringing() const
    {
        return false;
    }

    std::size_t VoiceCount() const
    {
        return nnsnd::kNumSources;
    }

    xsflib::rip::VoicePlace Voice(std::size_t index) const
    {
        const nnsnd::SoundSystem::VoicePlace place = model_.snd.PlaceOf(static_cast<int>(index));
        return {place.playing, place.loops, place.position};
    }

    bool Active() const
    {
        return player_.IsActive();
    }

    bool Settled() const
    {
        return player_.IsSettled(engine_);
    }

    bool Stuck() const
    {
        return player_.IsStuck(engine_);
    }

    bool Moving() const
    {
        return player_.IsMoving();
    }

    uint64_t ChangedAt() const
    {
        return player_.ChangedAt();
    }

    bool Busy() const
    {
        return engine_.IsBusy();
    }

    bool ChannelsChanging() const
    {
        return engine_.Channels().AnyChanging();
    }

    int ActiveChannels() const
    {
        return engine_.Channels().ActiveCount();
    }

    template <typename Function>
    void OnLoop(Function function)
    {
        engine_.on_loop_ = std::move(function);
    }

private:
    nwsnd::ArchiveModel& model_;
    nwsnd::Engine& engine_;
    nwsnd::SequenceSoundPlayer& player_;
    bool& audible_;
};

// Plays a sequence on the model until it loops twice, ends, falls silent for good or reaches kMaxSeconds
// (xsflib::rip::MeasureLength). docs/3sf.md, "Lengths", gives the rules.
std::optional<Timing> MeasureSequence(const ntdlib::sar::SoundArchive& archive, const std::vector<uint8_t>& cdc,
                                      const Variables& variables, uint32_t sound_index)
{
    // The model, and whether the frame being run has made a sound, from the DSP's output.
    nwsnd::ArchiveModel model;
    bool audible = false;
    const auto listen = [&audible](std::array<int16_t, 2> sample)
    {
        audible = audible || xsflib::rip::Audible(sample);
    };
    model.dsp.SetSampleSink(listen);

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

    for (const auto& [number, value] : variables)
    {
        player->SetVariable(engine, number, value);
    }

    LengthModel length_model(model, *player, audible);
    return xsflib::rip::MeasureLength(length_model, LengthRules());
}

} // namespace

struct LengthAnalyzer::Impl
{
    ntdlib::sar::SoundArchive archive;
    std::vector<uint8_t> cdc;
    Variables variables;
};

LengthAnalyzer::LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const std::vector<uint8_t>& dsp_component,
                               const Variables& variables)
    : impl_(new Impl{ntdlib::sar::SoundArchive::Load(archive_bytes, kSarOptions), dsp_component, variables})
{
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index) const
{
    return xsflib::rip::TryMeasure(
        [this, sound_index] { return MeasureSequence(impl_->archive, impl_->cdc, impl_->variables, sound_index); });
}

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
