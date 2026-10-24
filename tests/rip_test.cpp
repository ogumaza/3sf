// SPDX-License-Identifier: MIT

// Unit tests for the ripper library (src/rip).

#include "rip/rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

#include "common/sar_options.h"
#include "rip/game.h"
#include "rip/music.h"
#include "threesf/format.h"

using namespace threesf;
using namespace xsflib::testing;

// A well-formed DSP1 image of 0x310 bytes: the header, then one segment of 0x10 bytes.
static std::vector<uint8_t> DspImage()
{
    std::vector<uint8_t> image(0x310);
    std::memcpy(image.data() + 0x100, "DSP1", 4);
    xsflib::StoreLe32(image.data() + 0x104, 0x310); // the image's size
    image[0x10e] = 1;                               // the number of segments
    xsflib::StoreLe32(image.data() + 0x120, 0x300); // the segment's offset in the image
    xsflib::StoreLe32(image.data() + 0x128, 0x10);  // and its size

    return image;
}

// `code` with `image` copied into it at `offset`.
static std::vector<uint8_t> WithImage(std::vector<uint8_t> code, const std::vector<uint8_t>& image, std::size_t offset)
{
    std::copy(image.begin(), image.end(), code.begin() + static_cast<std::ptrdiff_t>(offset));

    return code;
}

// A game's DSP firmware is the .cdc file its code names, then one called dspaudio.cdc, then the first in the RomFS; a
// profile's firmware comes before them all.
static void TestFirmwareCandidates()
{
    rip::GameFiles game;
    const std::string kCode("\0\0rom:/snd/game.cdc\0", 20);
    game.code.assign(kCode.begin(), kCode.end());
    game.romfs_files = {"a/first.cdc", "b/dspaudio.cdc", "snd/game.cdc", "snd/game.bcsar", "c/other.CDC"};

    XSFLIB_CHECK((rip::FirmwareCandidates(game, nullptr) ==
                  std::vector<std::string>{"snd/game.cdc", "b/dspaudio.cdc", "a/first.cdc", "c/other.CDC"}));

    game.code.clear();

    XSFLIB_CHECK((rip::FirmwareCandidates(game, nullptr) ==
                  std::vector<std::string>{"b/dspaudio.cdc", "a/first.cdc", "snd/game.cdc", "c/other.CDC"}));

    rip::GameProfile profile{};
    profile.firmware = "sound/x.cdc";

    const auto with_profile = rip::FirmwareCandidates(game, &profile);

    XSFLIB_CHECK(with_profile.size() == 5 && with_profile.front() == "sound/x.cdc");
}

// A DSP1 image linked into a game's code is found by its header, and a "DSP1" that doesn't start a well-formed image
// inside the code is passed over.
static void TestFirmwareInCode()
{
    const std::vector<uint8_t> image = DspImage();
    std::vector<uint8_t> code(0x1000, 0xee);
    std::memcpy(code.data() + 0x10, "DSP1", 4);                            // too near the start to follow a signature
    std::memcpy(code.data() + 0x180, "DSP1", 4);                           // its "size" is 0xeeeeeeee
    const std::vector<uint8_t> at_end(code.begin(), code.begin() + 0x900); // room for the image at 0x5f0, and no more

    XSFLIB_CHECK(rip::FindFirmwareInCode(WithImage(code, image, 0x800)) == std::size_t{0x800});
    XSFLIB_CHECK(rip::FindFirmwareInCode(WithImage(at_end, image, 0x5f0)) == std::size_t{0x5f0});
    XSFLIB_CHECK(!rip::FindFirmwareInCode(code));
    XSFLIB_CHECK(!rip::FindFirmwareInCode({}));
    XSFLIB_CHECK(!rip::FindFirmwareInCode(std::vector<uint8_t>(image.begin(), image.begin() + 0x2ff)));

    // Each of these breaks the image: it's bigger than what's left of the code, it has no segments, a segment runs past
    // its end, or a segment starts inside the header.
    const auto broken = [&](std::size_t field, uint32_t value)
    {
        std::vector<uint8_t> bad = image;
        if (field == 0x10e)
        {
            bad[field] = static_cast<uint8_t>(value);
        }
        else
        {
            xsflib::StoreLe32(bad.data() + field, value);
        }

        return !rip::FindFirmwareInCode(WithImage(code, bad, 0x800));
    };

    XSFLIB_CHECK(broken(0x104, 0x801));
    XSFLIB_CHECK(broken(0x10e, 0));
    XSFLIB_CHECK(broken(0x128, 0x11));
    XSFLIB_CHECK(broken(0x120, 0x2ff));

    // So does an eleventh segment, even when it and the other ten are well-formed.
    std::vector<uint8_t> eleven = image;
    eleven.resize(0x340);
    xsflib::StoreLe32(eleven.data() + 0x104, 0x340);
    eleven[0x10e] = 11;
    for (std::size_t i = 1; i <= 10; i++)
    {
        std::copy_n(image.begin() + 0x120, 0x30, eleven.begin() + static_cast<std::ptrdiff_t>(0x120 + i * 0x30));
    }

    XSFLIB_CHECK(!rip::FindFirmwareInCode(WithImage(code, eleven, 0x800)));
}

// A game's firmware is the first of its RomFS files that's a DSP1 image, and otherwise the image in its code.
static void TestGameFirmware()
{
    rip::GameFiles game;
    const std::vector<uint8_t> image = DspImage();
    std::vector<uint8_t> file = DspImage();
    file[0x300] = 0x5a; // tells the file from the image in the code
    game.code = WithImage(std::vector<uint8_t>(0x1000), image, 0x400);
    game.romfs_files = {"sound/other.cdc", "sound/dspaudio.cdc"};
    game.read_romfs = [&](const std::string& path, std::vector<uint8_t>& data)
    {
        data = path == "sound/dspaudio.cdc" ? file : std::vector<uint8_t>{1, 2, 3};

        return true;
    };

    const auto from_romfs = rip::FindGameFirmware(game, nullptr);
    game.romfs_files = {"sound/other.cdc"};
    const auto from_code = rip::FindGameFirmware(game, nullptr);
    game.code.assign(0x1000, 0);
    const auto from_neither = rip::FindGameFirmware(game, nullptr);

    XSFLIB_CHECK(from_romfs && from_romfs->romfs_path == "sound/dspaudio.cdc" && from_romfs->data == file);
    XSFLIB_CHECK(from_code && from_code->romfs_path.empty() && from_code->code_offset == 0x400u &&
                 from_code->data == image);
    XSFLIB_CHECK(!from_neither);
}

// --bgm's labels: a word that starts with BGM, a jingle or a fanfare, with or without a number, or SEQ_M_ at the start,
// in any case. Other words, and the same letters inside another word, don't count.
static void TestMusicLabels()
{
    for (const char* label : {"AB_BGM_C", "BGM_X", "SEQ_S_BGM1", "ab_bgm_c", "SEQ_JIN_X", "X_JINGLE2", "SQ_FANFARE_A",
                              "SEQ_M_X", "seq-m-x"})
    {
        Check(rip::LabelMarksMusic(label), std::string(label) + " is music");
    }

    for (const char* label : {"SEQ_SE_JUMP", "SE_JINJA", "SE_M_FALL", "SEQ_12", "SE_SONG_A", "XBGM_A", "SEQ_BG_M", ""})
    {
        Check(!rip::LabelMarksMusic(label), std::string(label) + " isn't music");
    }
}

// The walk counts the tracks a sequence opens and the notes on every path, each once. It sizes each command's arguments
// as the parser does (prefixes, variable-length numbers), and reads on past a conditional jump.
static void TestWalkSequence()
{
    const std::vector<uint8_t> kData = {
        0x88, 0x01, 0x00, 0x00, 0x10,             // 0x00: opentrack 1 at 0x10
        0x88, 0x02, 0x00, 0x00, 0x20,             // 0x05: opentrack 2 at 0x20
        0x3c, 0x64, 0x30,                         // 0x0a: a note, 48 ticks long
        0xff, 0xff, 0xff,                         // 0x0d: fin
        0x3e, 0x64, 0x30,                         // 0x10: a note
        0x80, 0x30,                               // 0x13: wait
        0x89, 0x00, 0x00, 0x10,                   // 0x15: jump back to 0x10
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0x19: fin
        0xa0, 0x40, 0x64, 0x00, 0x00, 0x00, 0x30, // 0x20: a note with a random length (4 bytes)
        0xa2, 0x89, 0x00, 0x00, 0x30,             // 0x27: a conditional jump to 0x30, which doesn't end the track
        0xff, 0xff, 0xff, 0xff,                   // 0x2c: fin
        0x41, 0x64, 0x81, 0x00,                   // 0x30: a note 128 ticks long (a two-byte length)
        0xff,                                     // 0x34: fin
    };
    const std::vector<uint8_t> kPastCondition = {0xa2, 0x89, 0x00, 0x00, 0x08, 0x3c, 0x64, 0x30, 0xff};
    const std::vector<uint8_t> kCutShort = {0x88, 0x01};

    const rip::SequenceShape shape = rip::WalkSequence(kData, 0);
    const rip::SequenceShape conditional = rip::WalkSequence(kPastCondition, 0);
    const rip::SequenceShape truncated = rip::WalkSequence(kCutShort, 0);

    XSFLIB_CHECK(shape.tracks == 3 && shape.notes == 4);
    XSFLIB_CHECK(conditional.tracks == 1 && conditional.notes == 1);
    XSFLIB_CHECK(truncated.tracks == 1 && truncated.notes == 0);
}

// A 3DS sound archive whose sounds have the labels `labels`, each with a file of its own. They're sequences, but for
// those in `wave_sounds`.
static std::vector<uint8_t> ArchiveOf(const std::vector<std::string>& labels,
                                      const std::vector<std::string>& wave_sounds = {})
{
    namespace sar_files = ntdlib::testing::sar_files;
    sar_files::ArchiveSpec spec;
    spec.players = {{"PLAYER_BGM", 1}};
    for (std::size_t i = 0; i < labels.size(); i++)
    {
        const bool wave_sound = std::ranges::find(wave_sounds, labels[i]) != wave_sounds.end();
        const auto kind = wave_sound ? sar_files::SoundSpec::kWaveSound : sar_files::SoundSpec::kSequence;
        spec.sounds.push_back({.name = labels[i], .kind = kind, .file = static_cast<uint32_t>(i)});
        spec.files.push_back({.bytes = sar_files::Sequence(ntdlib::sar::Flavor::kCtr, {0xff}, {})});
    }

    return sar_files::Archive(ntdlib::sar::Flavor::kCtr, spec);
}

// The pieces whose channels are separate sequences (rip::FindPieces): five sequences labelled with one name and the
// suffixes _DPCM, _NOISE, _P1, _P2 and _TRI.
static void TestPieces()
{
    // Two pieces, one of them with its sounds in another order, and labels that make no piece: a channel missing, a
    // channel that isn't a sequence, suffixes without a name, and a second sound with a piece's first label.
    const std::vector<std::string> kLabels = {
        "BGM_A_DPCM", "BGM_A_NOISE", "BGM_A_P1",   "BGM_A_P2",   "BGM_A_TRI",   "BGM_B_TRI", "BGM_B_P2",
        "BGM_B_P1",   "BGM_B_NOISE", "BGM_B_DPCM", "BGM_C_DPCM", "BGM_C_NOISE", "BGM_C_P1",  "BGM_C_P2",
        "BGM_D_DPCM", "BGM_D_NOISE", "BGM_D_P1",   "BGM_D_P2",   "BGM_D_TRI",   "_DPCM",     "_NOISE",
        "_P1",        "_P2",         "_TRI",       "BGM_A_DPCM",
    };
    const auto archive = ntdlib::sar::SoundArchive::Load(ArchiveOf(kLabels, {"BGM_D_TRI"}), kSarOptions);

    const std::vector<rip::Piece> pieces = rip::FindPieces(archive);

    Check(pieces.size() == 2, "two pieces (" + std::to_string(pieces.size()) + ")");
    if (pieces.size() == 2)
    {
        Check(pieces[0].name == "BGM_A" && pieces[0].sounds == std::vector<uint32_t>{0, 1, 2, 3, 4},
              "a piece's sounds: _DPCM, _NOISE, _P1, _P2 and _TRI");
        Check(pieces[1].name == "BGM_B" && pieces[1].sounds == std::vector<uint32_t>{9, 8, 7, 6, 5},
              "a piece's sounds go by their suffixes, not their places in the archive");
    }
}

// The timing of sequences that play together (rip::TimingOfPiece): the latest end, and the longest fade. A silent
// sequence doesn't count.
static void TestTimingOfPiece()
{
    using rip::Timing;
    const Timing kLoop{60000, 10000};
    const Timing kEnd{30000, 0};
    const Timing kSilent{500, 0, true};
    const auto same = [](const std::optional<Timing>& a, const Timing& b)
    {
        return a && a->length_ms == b.length_ms && a->fade_ms == b.fade_ms && a->silent == b.silent;
    };

    Check(same(rip::TimingOfPiece({kLoop, kLoop}), kLoop), "sequences that loop together");
    Check(same(rip::TimingOfPiece({kEnd, Timing{32000, 0}}), Timing{32000, 0}), "sequences that end: the latest end");
    Check(same(rip::TimingOfPiece({kLoop, Timing{70000, 0}}), Timing{70000, 10000}),
          "a sequence that ends after another's loops: its end, and the fade");
    Check(same(rip::TimingOfPiece({kSilent, kEnd, kSilent}), kEnd), "a silent sequence doesn't count");
    Check(same(rip::TimingOfPiece({kSilent, kSilent}), kSilent), "a piece of silent sequences is silent");
    Check(same(rip::TimingOfPiece({kEnd}), kEnd), "one sequence keeps its timing");
    Check(!rip::TimingOfPiece({kLoop, std::nullopt}) && !rip::TimingOfPiece({}),
          "no timing without the timing of each sequence");
}

// The archive-mode library and minis: the group files beside the archive, the descriptor's version and a mini that
// lists the sounds of a piece (docs/3sf.md, "Archive descriptor" and "Sounds that play together").
static void TestArchiveSets()
{
    const std::vector<uint8_t> kArchive = {1, 2, 3};
    const std::vector<uint8_t> kFirmware = {4, 5};
    const std::vector<rip::FileChunk> kGroups = {{"GROUP_A.bcgrp", {6}}, {"sound/GROUP_B.bcgrp", {7, 8}}};
    const std::vector<uint32_t> kPiece = {0x01000009, 0x01000008, 0x01000007};
    std::map<std::string, std::vector<uint8_t>> files;
    files["a/plain.3sflib"] = rip::BuildArchiveLibrary("Sound.bcsar", kArchive, "dspaudio.cdc", kFirmware, {});
    files["a/groups.3sflib"] = rip::BuildArchiveLibrary("Sound.bcsar", kArchive, "dspaudio.cdc", kFirmware, {}, kGroups,
                                                        ArchiveDescriptor::kListVersion);
    files["a/piece.mini3sf"] = rip::BuildArchiveMini(kPiece, "groups.3sflib", {});
    files["a/one.mini3sf"] = rip::BuildArchiveMini(0x01000013, "plain.3sflib", {});
    const FileReader reader = xsflib::testing::MapReader(files);

    LoadedSet piece;
    LoadedSet one;
    const auto piece_error = LoadSet("a/piece.mini3sf", reader, piece);
    const auto one_error = LoadSet("a/one.mini3sf", reader, one);

    // A library without group files has version 1, and only bit 0 of the flags: the descriptor that every player of
    // archive mode takes.
    ArchiveDescriptor v1;
    v1.archive_path = "Sound.bcsar";
    v1.firmware_path = "dspaudio.cdc";

    Check(!one_error && one.archive && one.archive->Serialize() == v1.Serialize() &&
              SoundsToPlay(one) == std::vector<uint32_t>{0x01000013},
          "a set without group files keeps version 1's descriptor");

    // The group files follow the archive and the firmware, and the descriptor lists them in their order.
    std::vector<std::string> paths;
    for (const auto& file : piece.files)
    {
        paths.push_back(file.path);
    }

    Check(!piece_error && piece.archive && piece.archive->version == ArchiveDescriptor::kListVersion &&
              piece.archive->flags == (kFlagDspRequired | kFlagGroupFiles) &&
              piece.archive->group_paths == std::vector<std::string>{"GROUP_A.bcgrp", "sound/GROUP_B.bcgrp"},
          "a set with group files lists them");
    Check(paths == std::vector<std::string>{"Sound.bcsar", "dspaudio.cdc", "GROUP_A.bcgrp", "sound/GROUP_B.bcgrp"},
          "the library holds the group files after the archive and the firmware");
    Check(SoundsToPlay(piece) == kPiece, "a mini of a piece plays its sounds in their order");
    Check(rip::BuildArchiveMini(0x01000013, "lib", {}) ==
              rip::BuildArchiveMini(std::vector<uint32_t>{0x01000013}, "lib", {}),
          "a mini of one sound is the same either way");
}

int main()
{
    TestArchiveSets();
    TestFirmwareCandidates();
    TestFirmwareInCode();
    TestGameFirmware();
    TestMusicLabels();
    TestPieces();
    TestTimingOfPiece();
    TestWalkSequence();

    return Summary("rip");
}
