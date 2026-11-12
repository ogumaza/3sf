// SPDX-License-Identifier: MIT

// The 3SF file format (docs/3sf.md): PSF container, program chunks (MEM, FILE and SND), process and archive
// descriptors, tags.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/ctr/snd/starting_tracks.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/program.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/set.h"

#include "nnsnd/aux_effects.h"

namespace threesf
{

// PSF version byte of 3SF. See docs/3sf.md ("Version byte") before changing it.
constexpr uint8_t kPsfVersion3sf = 0x26;

// The sample rate players report, and the rate the `length` and `fade` tags are counted at (docs/3sf.md, "Tags"). The
// DSP's exact output rate is 268111856 / 8192, about 32728.498 Hz.
constexpr int kPlayerSampleRate = 32728;

// Descriptor flags. Every descriptor requires bit 0 (DSP required). An archive descriptor may also set bit 1 (group
// files), bit 2 (aux effects) and bit 4 (timed starts). With bit 1, it lists the group files that the set holds beside
// the sound archive (docs/3sf.md, "Archive descriptor"). With bit 2, it gives the effects on the aux buses
// (docs/3sf.md, "Effects on the aux buses"). Bit 4 marks a set whose files may hold the 3sf_var_change, 3sf_seed,
// 3sf_tempo, 3sf_pitch, 3sf_banks, 3sf_fx_send and 3sf_track_volume tags, or a 3sf_var tag with a track variable
// (docs/3sf.md, "Tags"). Players of 3SF 1.3.3 and older refuse any other flags than bit 0. Players of 3SF 1.4 refuse
// bit 4, and would otherwise play such files without the tags. Bit 3 is reserved.
constexpr uint32_t kFlagDspRequired = xsflib::psf::kDescriptorFlagDspRequired;
constexpr uint32_t kFlagGroupFiles = 2;
constexpr uint32_t kFlagAuxEffects = 4;
constexpr uint32_t kFlagTimedStarts = 16;

// Output modes: 0 mono, 1 stereo, 2 surround. Version 1 plays stereo only.
constexpr uint32_t kOutputStereo = xsflib::psf::kDescriptorOutputStereo;

// The PSF container that the players share (xsflib::psf): tags, reading and writing PSF files, and the 3sf_var tag's
// variables.
using xsflib::psf::FileReader;
using xsflib::psf::FormatVariables;
using xsflib::psf::ReadWholeFile;
using xsflib::psf::Tags;
using xsflib::psf::Variables;
using xsflib::psf::WritePsf;

// The process descriptor: the reserved area of a game-mode set (docs/3sf.md, "Process descriptor").
struct ProcessDescriptor
{
    static constexpr uint32_t kMagic = 0x50465333; // "3SFP"
    static constexpr uint32_t kVersion = 1;

    // The most application memory a 3DS gives a program: 178 MiB, in the New 3DS's largest memory mode.
    static constexpr uint32_t kMaxAppMemory = 0x0B200000;

    static std::optional<ProcessDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    uint32_t version = kVersion;
    uint32_t entry = 0x100000;
    uint32_t stack_size = 0x4000;
    uint32_t priority = 0x30;
    uint32_t app_memory = 0x04000000;
    uint32_t status_address = 0;
    uint32_t flags = kFlagDspRequired;
};

// Descriptor of an archive-mode set: the sound archive is played by 3SF's model of the SDK sound player on the DSP
// firmware, instead of by the game's code (docs/3sf.md, "Archive mode").
struct ArchiveDescriptor
{
    static constexpr uint32_t kMagic = 0x41465333; // "3SFA"
    static constexpr uint32_t kVersion = 1;

    // The version of a set whose minis select several sounds that play together (docs/3sf.md, "Sounds that play
    // together"). Players of 3SF 1.3.3 and older accept version 1 alone. They refuse such a set rather than play its
    // first sound alone.
    static constexpr uint32_t kListVersion = 2;

    // The version of a set that gives further sets of effects on the aux buses, beside the effects of kFlagAuxEffects.
    // A mini's 3sf_aux tag picks one (docs/3sf.md, "Effect sets"). Version 3 also plays version 2's lists. Players of
    // 3SF 1.4 and older refuse it rather than play its sounds without their effects.
    static constexpr uint32_t kEffectSetsVersion = 3;

    static std::optional<ArchiveDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    uint32_t version = kVersion;
    uint32_t flags = kFlagDspRequired;
    uint32_t output_mode = kOutputStereo;
    std::string archive_path;  // FILE chunk holding the sound archive (.bcsar)
    std::string firmware_path; // FILE chunk holding the DSP firmware (a DSP1 image)

    // With kFlagGroupFiles: the FILE chunks holding the group files beside the archive (.bcgrp), in the order in which
    // the player adds them.
    std::vector<std::string> group_paths;

    // With kFlagAuxEffects: the effects on aux buses A and B. A bus without an effect stays off. This is effect set 0,
    // which a mini without the 3sf_aux tag plays.
    nnsnd::AuxEffectLists aux_effects;

    // With kFlagAuxEffects and version 3: effect sets 1 on. A mini whose 3sf_aux tag gives set N plays the effects of
    // effect_sets[N - 1] in place of aux_effects.
    std::vector<nnsnd::AuxEffectLists> effect_sets;

    // Why this player can't play the effects that Parse read: one has a kind or flags that it doesn't know. Parse
    // stops reading the effects there, and Check returns the reason.
    std::optional<std::string> unsupported_effect;
};

// The most sounds that an archive-mode set plays together.
constexpr std::size_t kMaxSounds = 16;

// The program builder (xsflib::psf).
using xsflib::psf::ProgramBuilder;

// The parameter block of 3SF's game-mode driver, which a game-mode set loads at kAddress (docs/3sf.md, "Driver").
struct DriverBlock
{
    static constexpr uint32_t kAddress = 0x0e000010;
    static constexpr uint32_t kMagic = 0x44465333; // "3SFD"
    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kVersionOffset = 0x04;
    static constexpr uint32_t kStatusOffset = 0x08;
    static constexpr uint32_t kSoundIdOffset = 0x0C;
    static constexpr uint32_t kOutputModeOffset = 0x10;
};

// What xsflib::psf's set loader requires of 3SF: its descriptors, version byte and driver block.
struct SetFormat
{
    using ProcessDescriptor = threesf::ProcessDescriptor;
    using ArchiveDescriptor = threesf::ArchiveDescriptor;

    static constexpr uint8_t kPsfVersion = kPsfVersion3sf;
    static constexpr const char* kName = "3SF";
    static constexpr xsflib::psf::DriverBlockLayout kDriverBlock{.address = DriverBlock::kAddress,
                                                                 .magic = DriverBlock::kMagic,
                                                                 .version = DriverBlock::kVersion,
                                                                 .version_offset = DriverBlock::kVersionOffset,
                                                                 .big_endian = false,
                                                                 .output_mode_offset = DriverBlock::kOutputModeOffset,
                                                                 .output_mode = kOutputStereo};
};

// A complete set: the chunks of a file and its libraries in loading order, the effective descriptor (exactly one of
// `descriptor` and `archive`) and the tags of the top-level file.
using LoadedSet = xsflib::psf::LoadedSet<SetFormat>;

// Loads a 3SF/mini3SF file with its _lib/_libN chain. Paths in _lib tags are relative to the directory of the file that
// names them: its path up to the last of `separators`. Returns an error for a set that this player can't play: a
// descriptor that fails its Check(), or a game-mode set whose driver block has another version or an output mode other
// than stereo.
using xsflib::psf::LoadSet;

// The sounds that an archive-mode set plays, by their item IDs, in the order in which they start (docs/3sf.md, "Sounds
// that play together"): every sound that the set's last SND chunk lists with archive descriptor version 2 or 3, and
// the first alone with version 1. Empty for a set without a SND chunk or an archive descriptor.
std::vector<uint32_t> SoundsToPlay(const LoadedSet& set);

// The effect set that an archive-mode set plays, from its 3sf_aux tag (docs/3sf.md, "Effect sets"): 0 for the
// descriptor's aux effects, or N for its effect set N. A tag that's missing or doesn't parse gives 0.
uint32_t EffectSetOf(const Tags& tags);

// Parses the 3sf_aux tag: an effect set from 0 to 65535, in decimal, with white space around it allowed. Returns
// nullopt for anything else.
std::optional<uint32_t> ParseEffectSet(std::string_view text);

// The effects of effect set `set` of `d`: its aux effects for set 0, and effect_sets[set - 1] for the others. Returns
// nullptr for a set that the descriptor doesn't give.
const nnsnd::AuxEffectLists* EffectSet(const ArchiveDescriptor& d, uint32_t set);

// The tracks of each sequence that an archive-mode set quiets as the sequence starts: the masks of the 3sf_mute and
// 3sf_silence tags, bit N for track N (docs/3sf.md, "Tags"). Muted tracks start no note. Silenced tracks start their
// notes at a volume of 0.
using SequenceStart = ntdlib::ctr::snd::SequenceStart;

// Parses a mask of tracks: 0 to 65535, in decimal or in hex with a 0x prefix, with white space around it allowed.
// Returns nullopt for anything else.
std::optional<uint16_t> ParseTrackMask(std::string_view text);

// The 3SF tags that set how a set plays (docs/3sf.md, "Tags"). Without one of them, a set can play wrongly. A sequence
// that waits for a variable then stays silent. The plugins keep these tags when the tags are edited.
constexpr std::array<const char*, 11> kPlaybackTags = {
    "3sf_var",     "3sf_var_change",   "3sf_seed", "3sf_tempo", "3sf_pitch",   "3sf_banks",
    "3sf_fx_send", "3sf_track_volume", "3sf_aux",  "3sf_mute",  "3sf_silence",
};

// The highest variable number of the 3sf_var and 3sf_var_change tags. 0 to 15 are the sound's player variables, 16 to
// 31 the global ones, and 32 to 47 the track variables: N sets variable N - 32 of every track (docs/3sf.md, "Tags").
constexpr int kMaxVariable = 47;

// Parses the 3sf_var tag with xsflib::psf::ParseVariables, with variables 0 to kMaxVariable: one or more assignments
// N=V, separated by commas or white space. Returns nullopt if any part is malformed or out of range, or if there's no
// assignment. Players of 3SF 1.4 and older take N up to 31. They ignore a tag with a track variable.
inline std::optional<Variables> ParseVariables(std::string_view text)
{
    return xsflib::psf::ParseVariables(text, kMaxVariable);
}

// A change of a sequence variable in a later sound frame: the 3sf_var_change tag's entries (docs/3sf.md, "Tags").
using SequenceVariable = ntdlib::ctr::snd::SequenceVariable;

// Parses the 3sf_var_change tag: one or more changes F:N=V, separated by commas or white space. Returns nullopt if any
// part is malformed or out of range.
std::optional<std::vector<SequenceVariable>> ParseVariableChanges(std::string_view text);

// Parses the 3sf_seed tag: a state of the random generator, 0 to 0xFFFFFFFF, in decimal or in hex with a 0x prefix,
// with white space around it allowed. Returns nullopt for anything else.
std::optional<uint32_t> ParseSeed(std::string_view text);

// The range of the 3sf_tempo and 3sf_pitch tags' ratios.
constexpr float kMinRatio = 0.0625f;
constexpr float kMaxRatio = 16.0f;

// Parses the 3sf_tempo or the 3sf_pitch tag: a ratio from kMinRatio to kMaxRatio, as decimal digits with an optional
// '.' and more digits, with white space around it allowed. Returns nullopt for anything else.
std::optional<float> ParseRatio(std::string_view text);

// Parses the 3sf_banks tag: one to four item IDs for bank slots 0 to 3 in order, separated by commas or white space.
// Each ID is decimal, or hex with a 0x prefix: a bank's item ID (0x03000000 to 0x03FFFFFF), or 0xFFFFFFFF
// (ntdlib::ctr::snd::kNoBank) for an empty slot. The slots after the last ID are empty. Returns nullopt if any part is
// malformed or out of range, or if there's no ID.
std::optional<std::array<uint32_t, 4>> ParseBanks(std::string_view text);

// Parses the 3sf_fx_send tag: the sends on aux buses A and B, each from 0 to 1, as decimal digits with an optional '.'
// and more digits, separated by a comma. White space is allowed around each. A single value gives bus A's send, and B
// gets 0. Returns nullopt for anything else.
std::optional<std::array<float, 2>> ParseSends(std::string_view text);

// The volumes of a sequence's 16 tracks, track 0 first, as factors of amplitude.
using TrackVolumes = std::array<float, ntdlib::ctr::snd::kSequenceTracks>;

// Parses the 3sf_track_volume tag: one or more entries N=V, separated by commas or white space. N is a track, 0 to 15,
// in decimal or in hex with a 0x prefix. V is its volume, from 0 to kMaxRatio, as decimal digits with an optional '.'
// and more digits. The tracks that the tag leaves out get 1. Returns nullopt if any part is malformed or out of range,
// or if there's no entry.
std::optional<TrackVolumes> ParseTrackVolumes(std::string_view text);

// The start that the 3sf_mute, 3sf_silence, 3sf_seed, 3sf_tempo, 3sf_pitch, 3sf_banks, 3sf_fx_send and
// 3sf_track_volume tags of `tags` give. A tag that's missing or doesn't parse gives no mask, no seed, a ratio of 1, the
// archive's banks, no sends or every track at 1.
// The start holds no variables and no game effect: the player reads 3sf_var, 3sf_var_change and 3sf_aux itself. Its
// volume is 1: the player applies the volume tag to its output.
SequenceStart SequenceStartOf(const Tags& tags);

// The changes of the 3sf_var_change tag of `tags`, in the tag's order. A tag that's missing or doesn't parse gives
// none.
std::vector<SequenceVariable> VariableChangesOf(const Tags& tags);

// Writes `start` into the tags of `tags`: each mask into 3sf_mute or 3sf_silence as 0x and four hex digits, the
// variables of frame 0 into 3sf_var, the later ones into 3sf_var_change, the seed into 3sf_seed as 0x and eight hex
// digits, the tempo ratio and the pitch into 3sf_tempo and 3sf_pitch as the shortest decimal that reads back as the
// same float, the volume into the volume tag in the same way, the banks into 3sf_banks as 0x and eight hex digits
// each, up to the last slot with a bank, the sends into 3sf_fx_send as two decimals like the ratios, and each track's
// volume other than 1 into 3sf_track_volume as N=V, separated by ", ", with V like the ratios. A start's variable of
// frame 0 replaces one that 3sf_var already sets. An empty mask, no later variable, no seed, a ratio or volume of 1,
// the archive's banks, sends of 0 and every track at 1 write no tag. The game effect writes no tag: the ripper gives it
// the 3sf_aux tag of its effect set.
void WriteSequenceStart(const SequenceStart& start, Tags& tags);

// Whether a set that rips `start` needs flag bit 4 (kFlagTimedStarts): a variable of a later frame, a track variable, a
// seed, a tempo ratio or pitch other than 1, banks, a send other than 0, or a track's volume other than 1. A volume
// needs no flag: every player applies the volume tag.
bool NeedsTimedStarts(const SequenceStart& start);

} // namespace threesf
