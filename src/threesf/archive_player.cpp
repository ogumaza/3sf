// SPDX-License-Identifier: MIT

// Archive mode (see archive_player.h).

#include "threesf/archive_player.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/ctr/snd/sequence.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/set_archive.h"
#include "xsflib/player/archive_source.h"

#include "nnsnd/archive_start.h"
#include "nnsnd/aux_effects.h"
#include "threesf/format.h"

namespace threesf
{

struct ArchivePlayer::Snapshot
{
    ntdlib::ctr::snd::ArchiveModel::Snapshot model;
};

namespace
{

using ntdlib::ctr::snd::ArchiveModel;

// The model, and the samples that its DSP has made and RunFrame hasn't handed over yet. The DSP's sink adds to them
// from the moment the model exists. Samples made while the firmware starts therefore come first.
struct Model
{
    ArchiveModel model{ntdlib::ctr::snd::DspKind::kTeakra};
    std::vector<int16_t> pending;
};

// What Prepare finds in the set: the descriptor, the firmware, the sequences to play by their indices in the archive,
// in the order in which they start, and the effects on the aux buses from the set's 3sf_aux tag.
struct Found
{
    const ArchiveDescriptor* descriptor = nullptr;
    const std::vector<uint8_t>* firmware = nullptr;
    std::vector<uint32_t> sounds;
    const nnsnd::AuxEffectLists* effects = nullptr;
};

// 3SF's steps of archive mode (xsflib::player::ArchiveSource).
struct Traits
{
    using Set = const LoadedSet;
    using Prepared = Found;
    using Archive = ntdlib::sar::SoundArchive;
    using Model = threesf::Model;
    using Snapshot = ArchivePlayer::Snapshot;

    static constexpr bool kKeepArchive = false;

    static std::optional<std::string> Prepare(const LoadedSet& set, Found& found)
    {
        if (!set.archive)
        {
            return "not an archive-mode set";
        }

        const ArchiveDescriptor& d = *set.archive;
        found.descriptor = &d;
        if (!set.File(d.archive_path))
        {
            return "the sound archive (" + d.archive_path + ") is missing";
        }

        found.firmware = set.File(d.firmware_path);
        if (!found.firmware)
        {
            return "the DSP firmware (" + d.firmware_path + ") is missing";
        }

        const auto is_sound = [](uint32_t id)
        {
            return ntdlib::sar::ItemType(id) == ntdlib::sar::kSoundItem;
        };
        const std::vector<uint32_t> sounds = SoundsToPlay(set);
        if (sounds.empty() || !std::ranges::all_of(sounds, is_sound))
        {
            return "no sequence selected";
        }

        if (sounds.size() > kMaxSounds)
        {
            return std::to_string(sounds.size()) + " sounds selected; a set plays at most " +
                   std::to_string(kMaxSounds) + " together";
        }

        found.sounds.clear();
        for (const uint32_t id : sounds)
        {
            found.sounds.push_back(ntdlib::sar::ItemNumber(id));
        }

        // The 3sf_aux tag picks one of the descriptor's effect sets (docs/3sf.md, "Effect sets").
        const uint32_t effect_set = EffectSetOf(set.tags);
        found.effects = EffectSet(d, effect_set);
        if (!found.effects)
        {
            return "the 3sf_aux tag gives effect set " + std::to_string(effect_set) +
                   ", and the archive descriptor gives " + std::to_string(d.effect_sets.size()) +
                   " effect sets beside set 0";
        }

        return std::nullopt;
    }

    // The archive, with the group files beside it that the descriptor lists.
    static std::optional<std::string> LoadArchive(const LoadedSet& set, const Found& found,
                                                  std::optional<Archive>& archive)
    {
        const ArchiveDescriptor& d = *found.descriptor;

        return ntdlib::sar::LoadSetArchive(set, d.archive_path, d.group_paths, ntdlib::ctr::kSarOptions, archive);
    }

    static std::unique_ptr<Model> MakeModel(const Found&, Archive&)
    {
        auto model = std::make_unique<Model>();
        std::vector<int16_t>& pending = model->pending;
        const auto capture = [&pending](const std::array<int16_t, 2>& sample)
        {
            pending.push_back(sample[0]);
            pending.push_back(sample[1]);
        };
        model->model.dsp->SetSampleSink(capture);

        return model;
    }

    // The effects of the set's effect set go on the aux buses (docs/3sf.md, "Effects on the aux buses"). The
    // descriptor's Check has made sure that the libraries take them.
    static std::optional<std::string> StartModel(Model& model, Archive& archive, const Found& found)
    {
        const ArchiveDescriptor& d = *found.descriptor;
        if (!nnsnd::StartArchiveModel(model.model, *found.firmware, archive, static_cast<uint8_t>(d.output_mode),
                                      *found.effects))
        {
            return "the DSP firmware didn't start";
        }

        return std::nullopt;
    }

    // Every sequence starts before the first frame, in the list's order (docs/3sf.md, "Sounds that play together").
    static std::optional<std::string> StartSequence(Model& model, const LoadedSet& set, const Found& found)
    {
        // The 3sf_var tag sets variables before each sequence's first tick, and 3sf_var_change sets them in later
        // frames. A tag that doesn't parse is ignored. The masks, the seed, the tempo ratio, the pitch and the sends
        // follow. The 3sf_banks tag gives each sequence its banks as it starts.
        const auto tag = set.tags.find("3sf_var");
        const std::optional<Variables> variables =
            tag != set.tags.end() ? ParseVariables(tag->second) : std::optional<Variables>();
        const std::vector<SequenceVariable> changes = VariableChangesOf(set.tags);
        const SequenceStart start = SequenceStartOf(set.tags);
        ntdlib::ctr::snd::Engine& engine = *model.model.engine;
        for (const uint32_t index : found.sounds)
        {
            ntdlib::ctr::snd::SequenceSoundPlayer* player = engine.StartSequence(index, start.banks);
            if (!player)
            {
                return "sound " + std::to_string(index) + " isn't a sequence, or its data isn't in the archive";
            }

            for (const auto& [number, value] : variables.value_or(Variables{}))
            {
                player->SetVariable(engine, number, value);
            }

            ntdlib::ctr::snd::ApplyVariables(engine, *player, changes);
            ntdlib::ctr::snd::ApplySequenceStart(engine, *player, start);
        }

        return std::nullopt;
    }

    // The samples that the DSP has made since the last hand-over: first those of the firmware's start, then those of
    // each frame.
    static void Drain(Model& model, std::vector<int16_t>& out)
    {
        out.insert(out.end(), model.pending.begin(), model.pending.end());
        model.pending.clear();
    }

    // The frame's samples go to `out` even when the DSP stops. The sink added them as they came.
    static bool RunFrame(Model& model, std::vector<int16_t>& out)
    {
        const bool ran = model.model.engine && model.model.engine->RunFrame();
        Drain(model, out);

        return ran;
    }

    static bool Busy(const Model& model)
    {
        return model.model.engine && model.model.engine->IsBusy();
    }

    static Snapshot Save(Model& model, const Snapshot* previous)
    {
        return Snapshot{model.model.Save(previous ? &previous->model : nullptr)};
    }

    static void Restore(Model& model, const Snapshot& snapshot)
    {
        model.model.Restore(snapshot.model);
    }
};

} // namespace

struct ArchivePlayer::State
{
    xsflib::player::ArchiveSource<Traits> source;
};

ArchivePlayer::ArchivePlayer() : state_(std::make_unique<State>())
{
}

ArchivePlayer::~ArchivePlayer() = default;

std::optional<std::string> ArchivePlayer::Start(const LoadedSet& set)
{
    output_.clear();

    return state_->source.Start(set, output_);
}

bool ArchivePlayer::RunFrame()
{
    return state_->source.RunFrame(output_);
}

bool ArchivePlayer::Busy() const
{
    return state_->source.Busy();
}

std::shared_ptr<ArchivePlayer::Snapshot> ArchivePlayer::Save(const Snapshot* previous)
{
    return state_->source.Save(previous);
}

void ArchivePlayer::Restore(const Snapshot& snapshot)
{
    state_->source.Restore(snapshot);
}

} // namespace threesf
