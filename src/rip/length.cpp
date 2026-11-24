// SPDX-License-Identifier: MIT

// The ripper's length analysis (see length.h and docs/3sf.md, "Lengths").

#include "rip/length.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ctr/dsp/dsp.h"
#include "ntdlib/ctr/nnsnd/dsp_abi.h"
#include "ntdlib/ctr/nnsnd/sound_system.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/ctr/snd/sequence.h"
#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"

#include "nnsnd/archive_start.h"
#include "nnsnd/aux_effects.h"
#include "rip/rip.h"
#include "threesf/format.h"
#include "threesf/player.h"

namespace threesf::rip
{
namespace
{

using ntdlib::ctr::dsp::kSampleRate;
using ntdlib::ctr::dsp::kSamplesPerFrame;
using ntdlib::ctr::snd::ArchiveModel;
using ntdlib::ctr::snd::Engine;
using ntdlib::ctr::snd::SequenceSoundPlayer;

// Length analysis (xsflib/rip/length.h): any sequence stops after kMaxSeconds. kMaxFrames is that time in sound frames.
constexpr double kMaxSeconds = 600.0;
constexpr uint64_t kMaxFrames = static_cast<uint64_t>(kMaxSeconds * kSampleRate / kSamplesPerFrame);

// A sequence that stays busy without a sound for this long has ended where it fell silent.
constexpr double kSilentSeconds = 180.0;
constexpr uint64_t kSilentFrames = static_cast<uint64_t>(kSilentSeconds * kSampleRate / kSamplesPerFrame);

// A reverb's ring has ended after kRingSilentSeconds of silence, in the model with effects and in RingOut. A render of
// RingOut that still sounds kRingMaxSeconds after the model's end stops there.
constexpr double kRingSilentSeconds = 1.0;
constexpr uint64_t kRingSilentFrames = static_cast<uint64_t>(kRingSilentSeconds * kSampleRate / kSamplesPerFrame);
constexpr double kRingMaxSeconds = 30.0;
constexpr uint64_t kRingMaxFrames = static_cast<uint64_t>(kRingMaxSeconds * kSampleRate / kSamplesPerFrame);

// The names of the set that RingOut plays.
constexpr const char* kRingMini = "ring.mini3sf";
constexpr const char* kRingLibrary = "ring.3sflib";

// A frame's length in the tags. They count time at the rate players report rather than the DSP's exact rate. A player
// that turns a length back into samples then stops at the frame that the analysis found.
constexpr double kFrameMs = kSamplesPerFrame * 1000.0 / kPlayerSampleRate;

// The length analysis's rules. 3SF counts a loop's point from the frame that runs it. A loop's sound and a steady
// sequence's loop count from the next frame. The DSP's output runs up to two frames behind the engine. An ending
// therefore runs to the output's last sound, not to the engine's first idle frame.
xsflib::rip::LengthRules LengthRules()
{
    xsflib::rip::LengthRules rules;
    rules.frame_ms = kFrameMs;
    rules.max_frames = kMaxFrames;
    rules.silent_frames = kSilentFrames;
    rules.max_ms = kMaxSeconds * 1000.0;
    rules.loop_offset = 0;
    rules.end_at_idle_start = false;

    return rules;
}

// The samples that the DSP's sink has given a model of the analysis since the model started, the firmware's start
// included. A player of the set renders the same samples, in the same order, before it applies the set's volume.
struct SampleStream
{
    uint64_t samples = 0;                         // the stereo samples so far
    uint64_t frames = 0;                          // the frames that the analysis has run so far
    std::optional<uint64_t> first = std::nullopt; // the first stereo sample that isn't 0 on either side
    int64_t lag = 0;                              // the lag of the stream behind the clock (FirstSampleMs)
    bool sounded = false;                         // whether a frame has made a sound
};

// Archive mode's model for the length analysis, with what the analysis reads from the DSP's sample sink.
struct LengthArchiveModel : ArchiveModel
{
    using ArchiveModel::ArchiveModel;

    bool audible = false;           // whether the frame being run has made a sound
    bool effects = false;           // whether the aux buses have effects: they ring on after the sequence's last note
    uint64_t silent_frames = 0;     // the frames since the output last made a sound
    SampleStream* stream = nullptr; // the samples of the model's sink
};

// The rips' part of the length analysis of ntdlib (ntdlib::nwsnd::LengthModel) on the 3DS. The DSP's sample sink sets
// `audible` when a frame makes a sound at the set's volume. Effects ring on until the output has been silent for
// kRingSilentFrames. LengthModel counts each track's passes. A looping sequence's fade then waits for the tracks that
// loop at other lengths than the main track (OthersLooped).
//
// LengthModel takes the first sample that isn't 0 from Output, as an offset into the frame just run. While the DSP's
// output starts, the sink gives fewer samples in a frame than a full frame has, and Output gives none. The analysis
// then finds no first sample, and MeasureSequence sets it from the stream (FirstSampleMs).
struct LengthConsole
{
    using Model = LengthArchiveModel;

    // A frame's length at the rate players report, as in the tags.
    static constexpr double kFrameMs = threesf::rip::kFrameMs;

    static bool RunFrame(Model& model)
    {
        model.audible = false;
        const bool ran = model.engine->RunFrame();
        model.silent_frames = model.audible ? 0 : model.silent_frames + 1;
        SampleStream& stream = *model.stream;
        stream.frames++;
        if (model.audible || !stream.sounded)
        {
            stream.lag = static_cast<int64_t>(stream.frames * kSamplesPerFrame) - static_cast<int64_t>(stream.samples);
            stream.sounded = stream.sounded || model.audible;
        }

        return ran;
    }

    static bool Sounded(const Model& model)
    {
        return model.audible;
    }

    static std::span<const int16_t> Output(const Model& /*model*/)
    {
        return {};
    }

    static bool Ringing(const Model& model)
    {
        return model.effects && model.silent_frames < kRingSilentFrames;
    }

    static std::size_t VoiceCount(const Model& /*model*/)
    {
        return ntdlib::ctr::nnsnd::kNumSources;
    }

    static xsflib::rip::VoicePlace Voice(const Model& model, std::size_t index)
    {
        const ntdlib::ctr::nnsnd::SoundSystem::VoicePlace place = model.snd.PlaceOf(static_cast<int>(index));
        return {place.playing, place.loops, place.position};
    }
};

// The time of the first sample of `stream` that isn't 0, in milliseconds on the analysis's clock: frame N starts at N ×
// kFrameMs. Once the DSP's output has started, the sink gives about a frame's samples in each frame, and the stream
// lags the clock by a count that drifts slowly. The lag comes from the last frame with a sound. The first sample and
// that frame, where a sequence that ends ends and a loop's second pass ends, are then the same time apart on the clock
// as in a player's render, whose length tags count from that sample. Nothing without a first sample.
std::optional<long long> FirstSampleMs(const SampleStream& stream)
{
    if (!stream.first)
    {
        return std::nullopt;
    }

    const auto on_clock = static_cast<double>(static_cast<int64_t>(*stream.first) + stream.lag);

    return std::llround(on_clock * 1000.0 / kPlayerSampleRate);
}

// Plays a sequence on the model until it loops twice, ends, falls silent for good or reaches the limit: kMaxSeconds, or
// 3,600 s with `limit` kLong (xsflib::rip::MeasureLength). docs/3sf.md, "Lengths", gives the rules. ntdlib's
// MeasureSequence runs the measure, and runs it again with the outermost counted loops endless when it reaches the
// limit inside them.
std::optional<Timing> MeasureSequence(const ntdlib::sar::SoundArchive& archive, const std::vector<uint8_t>& firmware,
                                      const Variables& variables, const nnsnd::AuxEffectLists& effects,
                                      uint32_t sound_index, const SequenceStart& start, xsflib::rip::Limit limit)
{
    // The model, and whether the frame being run has made a sound, from the DSP's output. The set's volume tag scales
    // the output by the start's volume. A sample is sound if it's audible at that volume. The stream counts the samples
    // of the model made last, and finds its first sample that isn't 0, before the volume.
    SampleStream stream;
    const auto make_model = [&archive, &firmware, &effects, &stream, volume = start.volume]
    {
        stream = {};
        auto model = std::make_unique<LengthArchiveModel>(ntdlib::ctr::snd::DspKind::kTeakra);
        model->stream = &stream;
        LengthArchiveModel* const heard_by = model.get();
        const auto listen = [heard_by, volume](std::array<int16_t, 2> sample)
        {
            const auto heard = [volume](int16_t s)
            {
                return std::abs(static_cast<float>(s)) * volume > static_cast<float>(xsflib::rip::kSilenceLevel);
            };

            heard_by->audible = heard_by->audible || heard(sample[0]) || heard(sample[1]);
            SampleStream& samples = *heard_by->stream;
            if (!samples.first && (sample[0] != 0 || sample[1] != 0))
            {
                samples.first = samples.samples;
            }

            samples.samples++;
        };
        model->dsp->SetSampleSink(listen);

        if (!nnsnd::StartArchiveModel(*model, firmware, archive, static_cast<uint8_t>(kOutputStereo), effects))
        {
            return std::unique_ptr<LengthArchiveModel>();
        }

        model->effects = nnsnd::HasEffects(effects);

        return model;
    };

    // The sequence, with the set's variables and start.
    const auto start_sound = [&variables, &start, sound_index](LengthArchiveModel& model)
    {
        Engine& engine = *model.engine;
        SequenceSoundPlayer* player = engine.StartSequence(sound_index, start.banks);
        if (!player)
        {
            return player;
        }

        for (const auto& [number, value] : variables)
        {
            player->SetVariable(engine, number, value);
        }

        ntdlib::ctr::snd::ApplySequenceStart(engine, *player, start);

        return player;
    };

    std::optional<Timing> timing =
        ntdlib::nwsnd::MeasureSequence<LengthConsole>(make_model, start_sound, LengthRules(), {}, limit);
    if (timing && !timing->silent)
    {
        timing->first_sample_ms = FirstSampleMs(stream);
    }

    return timing;
}

} // namespace

struct LengthAnalyzer::Impl
{
    ntdlib::sar::SoundArchive archive;
    std::vector<uint8_t> firmware;
    Variables variables;
    nnsnd::AuxEffectLists effects;
};

LengthAnalyzer::LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const std::vector<uint8_t>& firmware,
                               const Variables& variables, const std::vector<std::vector<uint8_t>>& group_files,
                               const nnsnd::AuxEffectLists& effects)
    : impl_(new Impl{ntdlib::sar::SoundArchive::Load(archive_bytes, ntdlib::ctr::kSarOptions), firmware, variables,
                     effects})
{
    for (const std::vector<uint8_t>& group : group_files)
    {
        impl_->archive.AddGroupFile(group);
    }
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index, const SequenceStart& start,
                                              const nnsnd::AuxEffectLists* effects, xsflib::rip::Limit limit) const
{
    const nnsnd::AuxEffectLists& buses = effects ? *effects : impl_->effects;
    const auto measure = [this, sound_index, &start, &buses, limit]
    {
        return MeasureSequence(impl_->archive, impl_->firmware, impl_->variables, buses, sound_index, start, limit);
    };
    return xsflib::rip::TryMeasure(measure);
}

std::optional<Timing> RingOut(const std::vector<uint8_t>& library, uint32_t sound_id, const Timing& model)
{
    if (model.fade_ms > 0 || model.silent)
    {
        return model;
    }

    const std::vector<uint8_t> mini = BuildMini(sound_id, kRingLibrary, {});
    const xsflib::psf::FileReader reader = [&library, &mini](const std::string& path, std::vector<uint8_t>& data)
    {
        const std::string name = std::filesystem::path(path).filename().string();
        if (name != kRingMini && name != kRingLibrary)
        {
            return false;
        }

        data = name == kRingMini ? mini : library;

        return true;
    };

    Player player;
    if (!player.Load(kRingMini, reader) || !player.Start())
    {
        return std::nullopt;
    }

    // The render starts before the analysis's clock, and its first sample that isn't 0 is the model's first sample. A
    // place in the render, in stereo samples, is on the clock at its distance from that sample plus the model's first
    // sample. The render stops a second past the model's end with no sound, or kRingMaxFrames past that end.
    if (!model.first_sample_ms)
    {
        return model;
    }

    const double first_on_clock = static_cast<double>(*model.first_sample_ms) * kPlayerSampleRate / 1000.0;
    const double model_end = static_cast<double>(model.length_ms) * kPlayerSampleRate / 1000.0;
    const double ring_max = static_cast<double>(kRingMaxFrames * kSamplesPerFrame);
    const uint64_t ring_silent = kRingSilentFrames * kSamplesPerFrame;
    const auto max_blocks = static_cast<uint64_t>((model_end + 2 * ring_max) / kSamplesPerFrame);

    std::array<int16_t, 2 * kSamplesPerFrame> samples{};
    std::optional<uint64_t> first; // the render's first sample that isn't 0
    uint64_t end = 0;              // the end of the render's last block with a sound, in samples
    for (uint64_t block = 0; block < max_blocks; block++)
    {
        if (player.Render(samples.data(), kSamplesPerFrame) != kSamplesPerFrame)
        {
            return std::nullopt;
        }

        const uint64_t block_end = (block + 1) * kSamplesPerFrame;
        if (!first)
        {
            const auto nonzero = std::find_if(samples.begin(), samples.end(), [](int16_t v) { return v != 0; });
            if (nonzero != samples.end())
            {
                first = block * kSamplesPerFrame + static_cast<uint64_t>(nonzero - samples.begin()) / 2;
            }
        }

        if (xsflib::rip::Audible(samples))
        {
            end = block_end;
        }

        if (first)
        {
            const double on_clock = static_cast<double>(block_end - *first) + first_on_clock;
            if (on_clock >= model_end + ring_max || (on_clock >= model_end && block_end - end >= ring_silent))
            {
                break;
            }
        }
    }

    // The render ends later than the model. The first sounds stay the model's: the reverb only adds to the end.
    if (!first || end <= *first)
    {
        return model;
    }

    const double end_on_clock = static_cast<double>(end - *first) + first_on_clock;
    const auto length_ms = static_cast<long long>(std::llround(end_on_clock * 1000.0 / kPlayerSampleRate));
    if (length_ms <= model.length_ms)
    {
        return model;
    }

    Timing rung = model;
    rung.length_ms = length_ms;

    return rung;
}

} // namespace threesf::rip
