// SPDX-License-Identifier: MIT

// The rips of game mode: the game profiles, the driver's page, the library and the minis (see rip.h and docs/3sf.md).
// firmware.cpp finds a game's DSP firmware, and archive_mode.cpp makes the rips of archive mode.

#include "rip/rip.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/ctr_fx/effects.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "nnsnd/aux_effects.h"
#include "rip/driver_blob.h"
#include "rip/effects.h"
#include "rip/game.h"
#include "rip/length.h"
#include "threesf/format.h"

namespace threesf::rip
{
namespace
{

static_assert(kDriverEnd - kDriverBase <= kDriverRegion, "the driver and its bss have outgrown their page");
static_assert(kDriverParams == DriverBlock::kAddress, "the driver's parameters aren't where the format has them");
constexpr uint32_t kStatusAddress = DriverBlock::kAddress + DriverBlock::kStatusOffset;
constexpr uint32_t kSoundIdAddress = DriverBlock::kAddress + DriverBlock::kSoundIdOffset;

// The driver's update period in nanoseconds: one 3DS video frame (268111856 / 4481136 Hz).
constexpr uint32_t kUpdatePeriodNs = 16713680;

uint32_t Get32(const std::vector<uint8_t>& v, std::size_t off)
{
    return xsflib::LoadLe32(v.data() + off);
}

void Set32(std::vector<uint8_t>& v, std::size_t off, uint32_t x)
{
    xsflib::StoreLe32(v.data() + off, x);
}

// The driver's page: the driver, and its parameters for `profile` in the order driver/driver.c's struct Params declares
// them.
std::vector<uint8_t> DriverPage(const GameProfile& profile)
{
    std::vector<uint8_t> driver(kDriverRegion, 0);
    std::memcpy(driver.data(), kDriverBlob, sizeof(kDriverBlob));
    constexpr std::size_t kParamsOffset = kDriverParams - kDriverBase;
    std::vector<uint32_t> params = {
        DriverBlock::kMagic,    // magic: "3SFD"
        DriverBlock::kVersion,  // version
        0,                      // status
        0,                      // sound id
        kOutputStereo,          // output mode
        profile.handle,         // sound handle slot
        profile.data_heap_size, // device memory for the sound's data
        kUpdatePeriodNs,        // update period in nanoseconds
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
        // The reverb on aux bus A.
        profile.reverb.manager_pointer,
        profile.reverb.start,
    };
    params.insert(params.end(), profile.reverb.parameters.begin(), profile.reverb.parameters.end());
    params.insert(params.end(), profile.reverb.filter_sizes.begin(), profile.reverb.filter_sizes.end());
    for (std::size_t i = 0; i < params.size(); i++)
    {
        xsflib::StoreLe32(driver.data() + kParamsOffset + 4 * i, params[i]);
    }

    return driver;
}

// Pokemon X's nw::snd ends each read of a file in the sound archive on a multiple of 32 bytes from the archive's start.
constexpr std::size_t kReadAlignment = 32;

// The copy of the sound archive `archive` that a game-mode library holds for the sounds at `sounds`. When the sound
// system starts, Pokemon X's nw::snd reads the archive's first 0x40 bytes and its INFO block. To load a sequence,
// LoadData reads the sequence's file, the files of its banks and the files of the wave archives that the banks name.
// Each read starts at the place that the file table gives. Where the waves load one by one, a wave archive is read in
// parts. The parts lie within the file's bytes. The copy keeps all those bytes in their places, with zeros in place of
// the FILE block's other bytes (ntdlib::rip::KeepLoadedFiles). The game therefore reads the same bytes as from the
// whole archive. LoadedArchive returns the whole archive when the archive can't be read or KeepLoadedFiles gives no
// copy. A wave sound among `sounds` is one case where KeepLoadedFiles gives none.
std::vector<uint8_t> LoadedArchive(const std::vector<uint8_t>& archive, const std::vector<uint32_t>& sounds)
{
    try
    {
        const ntdlib::sar::SoundArchive read = ntdlib::sar::SoundArchive::Load(archive, ntdlib::ctr::kSarOptions);
        if (std::optional<std::vector<uint8_t>> copy = ntdlib::rip::KeepLoadedFiles(read, sounds, kReadAlignment))
        {
            return std::move(*copy);
        }
    }
    catch (const xsflib::FormatError&)
    {
        // A damaged archive goes into the library whole.
    }

    return archive;
}

// The words of nw::snd's FxReverb parameters for the driver (ReverbProfile::parameters), from `param`. The flag word is
// 0. `param` must set neither flag.
constexpr std::array<uint32_t, 9> ReverbWords(const ntdlib::ctr_fx::ReverbParam& param)
{
    return {
        param.early_reflection_time,               // early reflections, in ms
        param.fused_time,                          // the time in which the reverb falls by 60 dB, in ms
        param.pre_delay_time,                      // pre-delay, in ms
        std::bit_cast<uint32_t>(param.coloration), // a level
        std::bit_cast<uint32_t>(param.damping),    // damping: the coefficient of a one-pole filter in the feedback
        0,                                         // the filter sizes' pointer, which the driver sets
        std::bit_cast<uint32_t>(param.early_gain), // a level
        std::bit_cast<uint32_t>(param.fused_gain), // a level
        0,                                         // two flags, both clear: stereo, and damping by a low-pass filter
    };
}

static_assert(!kPokemonXFieldReverb.enable_surround && !kPokemonXFieldReverb.high_pass_damping,
              "ReverbWords writes both flags clear");

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
            // The reverb that the game's field module starts on aux bus A (its function at +0xa3908). It calls the
            // static module's wrapper 0x424d5c with these parameters. The wrapper reads the sound manager's pointer at
            // 0x5af0f8, where the game puts it after the manager's init (0x10cc10). It then runs gfl::snd's setup of
            // the reverb (0x38d7c8) on aux bus A.
            {
                0x005af0f8,
                0x00424d5c,
                ReverbWords(kPokemonXFieldReverb),
                kPokemonXFieldReverb.filter_sizes,
            },
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

std::optional<std::vector<uint8_t>> BuildLibrary(const GameFiles& game, const GameProfile& profile,
                                                 const std::vector<uint32_t>& sounds, const Tags& tags,
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

    // The files that the profile names: the sound archive, with only the sounds' files, and the DSP firmware.
    const auto read = [&game, error](const char* path) -> std::optional<std::vector<uint8_t>>
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

        return bytes;
    };

    const std::optional<std::vector<uint8_t>> archive = read(profile.archive);
    const std::optional<std::vector<uint8_t>> firmware = archive ? read(profile.firmware) : std::nullopt;
    if (!firmware)
    {
        return std::nullopt;
    }

    program.AddFile(profile.archive, LoadedArchive(*archive, sounds));
    program.AddFile(profile.firmware, *firmware);

    ProcessDescriptor d;
    d.entry = text;
    d.stack_size = Get32(exh, kExheaderStackSize);
    d.priority = exh[kExheaderPriority];
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

} // namespace threesf::rip
