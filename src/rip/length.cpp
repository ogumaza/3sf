// SPDX-License-Identifier: MIT

// The ripper's length analysis (see length.h and docs/3sf.md, "Lengths").

#include "rip/length.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "xsflib/rip/length.h"

#include "common/sar_options.h"
#include "dsp/dsp.h"
#include "nnsnd/dsp_abi.h"
#include "nnsnd/sound_system.h"
#include "nwsnd/engine.h"
#include "nwsnd/sequence.h"

namespace threesf::rip
{
namespace
{

// Length analysis (xsflib/rip/length.h): any sequence stops after kMaxSeconds. kMaxFrames is that time in sound frames.
constexpr double kMaxSeconds = 600.0;
constexpr uint64_t kMaxFrames = static_cast<uint64_t>(kMaxSeconds * dsp::kSampleRate / dsp::kSamplesPerFrame);

// A sequence that stays busy without a sound for this long has ended where it fell silent.
constexpr double kSilentSeconds = 180.0;
constexpr uint64_t kSilentFrames = static_cast<uint64_t>(kSilentSeconds * dsp::kSampleRate / dsp::kSamplesPerFrame);

// A frame's length in the tags. They count time at the rate players report rather than the DSP's exact rate, so that a
// player that turns a length back into samples stops at the frame the analysis found.
constexpr double kFrameMs = dsp::kSamplesPerFrame * 1000.0 / kPlayerSampleRate;

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

    // Sets the hooks for the sequence's loops and for every track's passes. Only the passes that changed the sound
    // count. A track that another track opens again starts its part again. That counts as a pass too. The fade then
    // doesn't wait for the tracks that a looping track opens again in each of its loops.
    template <typename Function>
    void OnLoop(Function function)
    {
        const auto count_pass = [this](int track, uint32_t)
        {
            const auto* looping = player_.Track(track);
            if (looping && !looping->IsIdle())
            {
                ++passes_[static_cast<std::size_t>(track)];
            }
        };

        const auto count_reopen = [this](int, int opened, uint32_t)
        {
            const auto index = static_cast<std::size_t>(opened);
            if (opened_[index])
            {
                ++passes_[index];
            }
            opened_[index] = true;
        };

        engine_.SetLoopHook(std::move(function));
        engine_.SetTrackLoopHook(count_pass);
        engine_.SetOpenTrackHook(count_reopen);
    }

    // Whether every open track other than `track` has made kLengthLoops passes that changed the sound. A track whose
    // last pass changed nothing, or that waits for ever, doesn't hold the fade back.
    bool OthersLooped(int track) const
    {
        for (int i = 0; i < static_cast<int>(passes_.size()); ++i)
        {
            const auto* other = player_.Track(i);
            if (i != track && other && other->IsOpen() && !other->IsIdle() && !other->WaitsForEver(engine_) &&
                passes_[static_cast<std::size_t>(i)] < xsflib::rip::kLengthLoops)
            {
                return false;
            }
        }

        return true;
    }

private:
    nwsnd::ArchiveModel& model_;
    nwsnd::Engine& engine_;
    nwsnd::SequenceSoundPlayer& player_;
    bool& audible_;
    std::array<int, ntdlib::nwsnd::kTrackCount> passes_{};  // each track's passes that changed the sound
    std::array<bool, ntdlib::nwsnd::kTrackCount> opened_{}; // whether an open-track command has opened the track
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

} // namespace threesf::rip
