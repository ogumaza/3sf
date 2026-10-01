// SPDX-License-Identifier: MIT

// Archive mode (see archive_player.h).

#include "threesf/archive_player.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "csar/formats.h"
#include "dsp/teakra_dsp.h"
#include "nwsnd/engine.h"

namespace threesf
{

struct ArchivePlayer::State
{
    std::optional<csar::SoundArchive> archive; // declared first, so that it outlives the model's engine
    nwsnd::ArchiveModel model;
};

ArchivePlayer::ArchivePlayer() = default;
ArchivePlayer::~ArchivePlayer() = default;

namespace
{

const std::vector<uint8_t>* FindFile(const LoadedSet& set, const std::string& path)
{
    const std::vector<uint8_t>* found = nullptr;
    for (const auto& f : set.files)
    {
        if (f.path == path)
        {
            found = &f.data; // later chunks replace earlier ones
        }
    }

    return found;
}

} // namespace

std::optional<std::string> ArchivePlayer::Start(const LoadedSet& set)
{
    if (!set.archive)
    {
        return "not an archive-mode set";
    }

    const ArchiveDescriptor& d = *set.archive;
    const std::vector<uint8_t>* archive_bytes = FindFile(set, d.archive_path);
    if (!archive_bytes)
    {
        return "the sound archive (" + d.archive_path + ") is missing";
    }

    const std::vector<uint8_t>* firmware = FindFile(set, d.firmware_path);
    if (!firmware)
    {
        return "the DSP firmware (" + d.firmware_path + ") is missing";
    }

    if (!set.sound || (*set.sound >> 24) != 0x01)
    {
        return "no sequence selected";
    }

    const uint32_t index = *set.sound & 0x00FFFFFFu;

    output_.clear();
    state_ = std::make_unique<State>();
    State& s = *state_;

    const auto capture = [this](const std::array<int16_t, 2>& sample)
    {
        output_.push_back(sample[0]);
        output_.push_back(sample[1]);
    };
    s.model.dsp.SetSampleSink(capture);

    s.archive = csar::SoundArchive::Load(*archive_bytes);
    if (!s.model.Start(*firmware, *s.archive, static_cast<uint8_t>(d.output_mode)))
    {
        return "the DSP firmware didn't start";
    }

    if (!s.model.engine->StartSequence(index))
    {
        return "sound " + std::to_string(index) + " isn't a sequence, or its data isn't in the archive";
    }

    return std::nullopt;
}

bool ArchivePlayer::RunFrame()
{
    return state_ && state_->model.engine && state_->model.engine->RunFrame();
}

bool ArchivePlayer::Busy() const
{
    return state_ && state_->model.engine && state_->model.engine->IsBusy();
}

} // namespace threesf
