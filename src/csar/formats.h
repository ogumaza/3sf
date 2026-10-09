// SPDX-License-Identifier: MIT

// Parsers for the CTR NintendoWare sound formats used by 3SF: BCSAR (sound archive) and the CSEQ, CBNK, CWAR and CWAV
// files it contains. Layouts follow nw::snd's readers (CSAR version 2.3).

#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace threesf::csar
{

// CWAV: a wave.
enum class WaveEncoding : uint8_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kDspAdpcm = 2,
    kImaAdpcm = 3
};

struct DspAdpcmInfo
{
    std::array<int16_t, 16> coefs{};
    uint16_t pred_scale = 0;
    int16_t yn1 = 0, yn2 = 0;
    uint16_t loop_pred_scale = 0;
    int16_t loop_yn1 = 0, loop_yn2 = 0;
};

struct WaveChannel
{
    std::span<const uint8_t> data; // sample data from the channel's start to the end of DATA
    std::optional<DspAdpcmInfo> adpcm;
};

// A parsed CWAV. The channels' data points into the file passed to Parse.
struct Wave
{
    static Wave Parse(std::span<const uint8_t> file);

    WaveEncoding encoding = WaveEncoding::kPcm16;
    bool loop = false;
    uint32_t sample_rate = 32728;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0; // also the length when not looping
    std::vector<WaveChannel> channels;
};

// CWAR: a wave archive. The waves point into the file passed to Parse.
struct WaveArchive
{
    static WaveArchive Parse(std::span<const uint8_t> file);

    std::vector<std::span<const uint8_t>> waves;
};

// CBNK: a bank of instruments.
struct Adshr
{
    // Default when a region has no ADSHR (code.bin 0x4c4edc fills 0x5e9800 with 127s).
    uint8_t attack = 127, decay = 127, sustain = 127, hold = 127, release = 127;
};

struct VelocityRegion
{
    uint32_t wave_id_index = 0;
    uint8_t original_key = 60;
    uint8_t volume = 127;
    uint8_t pan = 64;
    float pitch = 1.0f;
    bool ignore_note_off = false;
    uint8_t key_group = 0;
    uint8_t interpolation_type = 0;
    Adshr adshr;
};

// The bounds are ints: a direct table covers every value, up to kAnyValue, and a velocity can pass 255 (a velocity
// byte above 127 with a velocity range above 127).
struct KeyRegion
{
    static constexpr int kAnyValue = std::numeric_limits<int>::max();

    // A velocity range and the region it plays, if any.
    struct Vel
    {
        int lo, hi;
        std::optional<VelocityRegion> region;
    };

    int lo = 0, hi = 127;
    std::vector<Vel> velocities;
};

struct Instrument
{
    // Finds the velocity region for a key/velocity pair (nw::snd BankFileReader::ReadNoteInfo).
    const VelocityRegion* Find(int key, int velocity) const;

    std::vector<KeyRegion> keys;
};

struct WaveId
{
    uint32_t wave_archive_item = 0; // 0x05xxxxxx item id
    uint32_t index = 0;
};

struct Bank
{
    static Bank Parse(std::span<const uint8_t> file);

    std::vector<WaveId> waves;
    std::vector<std::optional<Instrument>> instruments;
};

// CSEQ: a sequence. The data points into the file passed to Parse.
struct Sequence
{
    static Sequence Parse(std::span<const uint8_t> file);

    std::span<const uint8_t> data; // DATA block payload (the bytecode)
};

// CSAR: the sound archive.
enum class SoundType
{
    kSequence,
    kStream,
    kWaveSound,
    kUnknown
};

struct SoundInfo
{
    std::string name; // from the string table, or made up from the index (SEQ_12) when the archive has none
    uint32_t file_id = 0;
    uint8_t volume = 127;
    SoundType type = SoundType::kUnknown;

    // From the sound's optional parameters. The defaults stand when it doesn't have them.
    uint8_t pan_mode = 0, pan_curve = 0;

    // Sequence sounds only.
    std::vector<uint32_t> banks; // bank item ids (0x03xxxxxx)
    uint32_t allocate_track_flags = 1;
    uint32_t start_offset = 0;
    uint8_t channel_priority = 64;
    bool release_priority_fix = false;
};

struct BankInfo
{
    std::string name;
    uint32_t file_id = 0;
};

struct WaveArchiveInfo
{
    std::string name;
    uint32_t file_id = 0;
};

// A parsed BCSAR. It keeps the archive's bytes, so the spans FileData returns stay valid as long as it does.
class SoundArchive
{
public:
    static SoundArchive Load(std::vector<uint8_t> bytes);

    const std::vector<SoundInfo>& Sounds() const
    {
        return sounds_;
    }

    const std::vector<BankInfo>& Banks() const
    {
        return banks_;
    }

    const std::vector<WaveArchiveInfo>& WaveArchives() const
    {
        return wave_archives_;
    }

    std::optional<uint32_t> FindSound(const std::string& name) const;

    // File contents for a file id, whether the archive holds it on its own or inside a group, or an empty span for an
    // external file or one past the end of a truncated archive.
    std::span<const uint8_t> FileData(uint32_t file_id) const;

    // The archive is shorter than its header says, or files lie past its end.
    bool Truncated() const
    {
        return truncated_;
    }

private:
    struct FileEntry
    {
        uint32_t offset = 0, size = 0;
        bool internal = false;
    };

    std::vector<uint8_t> bytes_;
    std::vector<SoundInfo> sounds_;
    std::vector<BankInfo> banks_;
    std::vector<WaveArchiveInfo> wave_archives_;
    std::vector<FileEntry> files_;
    bool truncated_ = false;
};

} // namespace threesf::csar
