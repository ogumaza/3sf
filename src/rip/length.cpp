// SPDX-License-Identifier: MIT

// The ripper's length analysis (see length.h and docs/3sf.md, "Lengths").

#include "rip/length.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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

// A frame's length in the tags. They count time at the rate players report rather than the DSP's exact rate, so that a
// player that turns a length back into samples stops at the frame the analysis found.
constexpr double kFrameMs = kSamplesPerFrame * 1000.0 / kPlayerSampleRate;

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
// frame makes a sound. The model also counts each track's passes. A looping sequence's fade then waits for the tracks
// that loop at other lengths than the main track (OthersLooped).
class LengthModel
{
public:
    // `effects`: whether the aux buses have effects, which ring on after the sequence's last note.
    LengthModel(ArchiveModel& model, SequenceSoundPlayer& player, bool& audible, bool effects)
        : model_(model),
          engine_(*model.engine),
          player_(player),
          audible_(audible),
          effects_(effects),
          passes_(engine_, player_)
    {
    }

    bool RunFrame()
    {
        audible_ = false;
        const bool ran = engine_.RunFrame();
        silent_frames_ = audible_ ? 0 : silent_frames_ + 1;
        ++frames_;

        return ran;
    }

    // The frames that the analysis has run.
    uint64_t Frames() const
    {
        return frames_;
    }

    bool Sounded() const
    {
        return audible_;
    }

    // Effects ring on until the output has been silent for kRingSilentFrames.
    bool Ringing() const
    {
        return effects_ && silent_frames_ < kRingSilentFrames;
    }

    std::size_t VoiceCount() const
    {
        return ntdlib::ctr::nnsnd::kNumSources;
    }

    xsflib::rip::VoicePlace Voice(std::size_t index) const
    {
        const ntdlib::ctr::nnsnd::SoundSystem::VoicePlace place = model_.snd.PlaceOf(static_cast<int>(index));
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

    // Sets the hooks for the sequence's loops and for every track's passes (ntdlib::nwsnd::TrackPasses::SetHooks).
    template <typename Function>
    void OnLoop(Function function)
    {
        engine_.SetLoopHook(std::move(function));
        passes_.SetHooks();
    }

    // Whether every other open track has made kLengthLoops passes that changed the sound
    // (ntdlib::nwsnd::TrackPasses::OthersLooped).
    bool OthersLooped(int track) const
    {
        return passes_.OthersLooped(track);
    }

private:
    ArchiveModel& model_;
    Engine& engine_;
    SequenceSoundPlayer& player_;
    bool& audible_;
    bool effects_;
    uint64_t silent_frames_ = 0; // the frames since the output last made a sound
    uint64_t frames_ = 0;        // the frames run
    ntdlib::nwsnd::TrackPasses<Engine, SequenceSoundPlayer> passes_;
};

// Plays a sequence on the model until it loops twice, ends, falls silent for good or reaches kMaxSeconds
// (xsflib::rip::MeasureLength). docs/3sf.md, "Lengths", gives the rules. The counted loops in `endless_loops` loop for
// ever. A sequence can repeat its music in a counted loop that the game plays for longer than the limit. When the
// measure reaches the limit inside counted loops, it runs again on a new model with each track's outermost counted loop
// endless (ntdlib::nwsnd::OutermostCountedLoops). ntdlib's length analysis does the same. Those loops' passes then
// count as loops of the sequence. An inner loop that runs at the limit still ends, and repeats in each pass of the
// outer loop.
std::optional<Timing> MeasureSequence(const ntdlib::sar::SoundArchive& archive, const std::vector<uint8_t>& firmware,
                                      const Variables& variables, const nnsnd::AuxEffectLists& effects,
                                      uint32_t sound_index, const SequenceStart& start,
                                      const ntdlib::nwsnd::CountedLoopSet& endless_loops = {})
{
    // The model, and whether the frame being run has made a sound, from the DSP's output. The set's volume tag scales
    // the output by the start's volume. A sample is sound if it's audible at that volume.
    ArchiveModel model{ntdlib::ctr::snd::DspKind::kTeakra};
    bool audible = false;
    const float volume = start.volume;
    const auto listen = [&audible, volume](std::array<int16_t, 2> sample)
    {
        const auto heard = [volume](int16_t s)
        {
            return std::abs(static_cast<float>(s)) * volume > static_cast<float>(xsflib::rip::kSilenceLevel);
        };

        audible = audible || heard(sample[0]) || heard(sample[1]);
    };
    model.dsp->SetSampleSink(listen);

    if (!nnsnd::StartArchiveModel(model, firmware, archive, static_cast<uint8_t>(kOutputStereo), effects))
    {
        return std::nullopt;
    }

    Engine& engine = *model.engine;
    engine.SetEndlessLoops(endless_loops);
    SequenceSoundPlayer* player = engine.StartSequence(sound_index, start.banks);
    if (!player)
    {
        return std::nullopt;
    }

    for (const auto& [number, value] : variables)
    {
        player->SetVariable(engine, number, value);
    }

    ntdlib::ctr::snd::ApplySequenceStart(engine, *player, start);

    const bool rings = nnsnd::HasEffects(effects);
    LengthModel length_model(model, *player, audible, rings);
    const xsflib::rip::LengthRules rules = LengthRules();
    const std::optional<Timing> timing = xsflib::rip::MeasureLength(length_model, rules);
    if (timing && endless_loops.empty() && length_model.Frames() >= rules.max_frames)
    {
        if (const ntdlib::nwsnd::CountedLoopSet running = ntdlib::nwsnd::OutermostCountedLoops(*player);
            !running.empty())
        {
            return MeasureSequence(archive, firmware, variables, effects, sound_index, start, running);
        }
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
                                              const nnsnd::AuxEffectLists* effects) const
{
    const nnsnd::AuxEffectLists& buses = effects ? *effects : impl_->effects;
    const auto measure = [this, sound_index, &start, &buses]
    {
        return MeasureSequence(impl_->archive, impl_->firmware, impl_->variables, buses, sound_index, start);
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

    // The frame after the model's last sound. The render's frames count from the frame in which the sound starts. The
    // model's frames count from the same frame.
    const xsflib::rip::LengthRules rules = LengthRules();
    const auto model_end = static_cast<uint64_t>(
        std::llround(std::max(0.0, static_cast<double>(model.length_ms) - rules.tail_ms) / kFrameMs));

    std::array<int16_t, 2 * kSamplesPerFrame> samples{};
    uint64_t end = 0; // the frame after the render's last sound
    for (uint64_t frame = 0; frame < model_end + kRingMaxFrames; frame++)
    {
        if (player.Render(samples.data(), kSamplesPerFrame) != kSamplesPerFrame)
        {
            return std::nullopt;
        }

        if (xsflib::rip::Audible(samples))
        {
            end = frame + 1;
        }
        else if (frame >= model_end && frame + 1 - end >= kRingSilentFrames)
        {
            break;
        }
    }

    const Timing rung = xsflib::rip::EndAt(end, kFrameMs, rules.tail_ms);
    return rung.length_ms > model.length_ms ? rung : model;
}

} // namespace threesf::rip
