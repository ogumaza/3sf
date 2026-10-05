// SPDX-License-Identifier: MIT

// Playback of a 3SF set with length, fade and volume (see playback.h).

#include "threesf/playback.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <utility>

namespace threesf
{
namespace
{

constexpr std::size_t kSeekBlock = 4096; // frames rendered at a time while seeking

uint64_t MsToFrames(long long ms)
{
    return ms <= 0 ? 0 : static_cast<uint64_t>(ms) * Playback::kSampleRate / 1000;
}

} // namespace

TrackLength ResolveLength(long long length_ms, long long fade_ms, const PlaybackOptions& options)
{
    if (length_ms < 0)
    {
        return {options.default_length_ms, fade_ms >= 0 ? fade_ms : options.default_fade_ms};
    }

    return {length_ms, fade_ms >= 0 ? fade_ms : 0};
}

bool Playback::Open(const std::string& path, const FileReader& reader, const PlaybackOptions& options)
{
    options_ = options;
    saved_.clear();
    latest_.reset();

    if (!player_.Load(path, reader))
    {
        return false;
    }

    ComputeLength();

    return true;
}

bool Playback::Open(LoadedSet set, const PlaybackOptions& options)
{
    options_ = options;
    saved_.clear();
    latest_.reset();

    if (!player_.Load(std::move(set)))
    {
        return false;
    }

    ComputeLength();

    return true;
}

void Playback::ComputeLength()
{
    // Without a length tag, the defaults apply unless the sound ends by itself first (see Render).
    const long long length = options_.length_override_ms >= 0 ? options_.length_override_ms : player_.LengthMs();
    const long long fade = options_.fade_override_ms >= 0 ? options_.fade_override_ms : player_.FadeMs();
    tagged_ = length >= 0;

    const TrackLength resolved = ResolveLength(length, fade, options_);
    play_frames_ = MsToFrames(resolved.play_ms);
    fade_frames_ = MsToFrames(resolved.fade_ms);
}

bool Playback::Start()
{
    position_ = 0;
    finished_at_.reset();
    saved_.clear();
    latest_.reset();
    interval_ = std::max<uint64_t>(options_.snapshot_interval, 1);

    if (!player_.Start())
    {
        return false;
    }

    SaveIfDue();

    return true;
}

void Playback::SaveIfDue()
{
    if (!options_.snapshots)
    {
        return;
    }

    const uint64_t stretch = position_ / interval_;
    const auto it = std::ranges::upper_bound(saved_, position_, {}, &SavedState::position);
    if (it != saved_.begin() && std::prev(it)->position / interval_ == stretch)
    {
        return;
    }

    std::shared_ptr<const Player::Snapshot> snapshot = player_.Save(latest_.get());
    if (!snapshot)
    {
        return;
    }

    latest_ = snapshot;
    saved_.insert(it, {position_, finished_at_, std::move(snapshot)});

    // Double the interval and keep the first snapshot in each new interval.
    while (saved_.size() > std::max<std::size_t>(options_.max_snapshots, 1))
    {
        interval_ *= 2;
        std::vector<SavedState> kept;
        for (SavedState& s : saved_)
        {
            if (kept.empty() || kept.back().position / interval_ != s.position / interval_)
            {
                kept.push_back(std::move(s));
            }
        }

        saved_ = std::move(kept);
    }
}

std::size_t Playback::Render(int16_t* out, std::size_t frames)
{
    std::size_t n = frames;
    if (!options_.endless)
    {
        uint64_t end = play_frames_ + fade_frames_;

        // A sound without a length tag ends half a second after it reports that it finished.
        if (!tagged_ && finished_at_)
        {
            end = std::min(end, *finished_at_ + kSampleRate / 2);
        }

        if (position_ >= end)
        {
            return 0;
        }

        n = static_cast<std::size_t>(std::min<uint64_t>(n, end - position_));
    }

    const std::size_t got = player_.Render(out, n);
    if (got == 0)
    {
        return 0;
    }

    const double volume = player_.Volume();
    const bool fading = !options_.endless && position_ + got > play_frames_ && fade_frames_ > 0;
    if (volume != 1.0 || fading)
    {
        for (std::size_t i = 0; i < got; i++)
        {
            double gain = volume;
            const uint64_t f = position_ + i;
            if (fading && f >= play_frames_)
            {
                gain *= 1.0 - static_cast<double>(f - play_frames_) / static_cast<double>(fade_frames_);
            }
            for (int c = 0; c < 2; c++)
            {
                const double v = std::round(out[2 * i + c] * gain);
                out[2 * i + c] = static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0));
            }
        }
    }

    position_ += got;
    if (!tagged_ && !finished_at_ && player_.GetState() == Player::State::kFinished)
    {
        finished_at_ = player_.FinishedFrame();
    }

    SaveIfDue();

    return got;
}

Playback::Route Playback::RouteTo(uint64_t frame) const
{
    // Use the latest snapshot at or before the target, unless the current position is nearer and the player can
    // continue from it.
    const bool can_go_on =
        player_.GetState() == Player::State::kPlaying || player_.GetState() == Player::State::kFinished;
    const auto it = std::ranges::upper_bound(saved_, frame, {}, &SavedState::position);
    const SavedState* from = it == saved_.begin() ? nullptr : &*std::prev(it);
    if (from && (!can_go_on || frame < position_ || from->position > position_))
    {
        return {from, false};
    }

    return {nullptr, !can_go_on || frame < position_};
}

uint64_t Playback::SeekStart(uint64_t frame) const
{
    const Route route = RouteTo(frame);
    return route.from ? route.from->position : route.restart ? 0 : position_;
}

void Playback::SetSnapshots(bool on)
{
    options_.snapshots = on;
    if (!on)
    {
        saved_.clear();
        latest_.reset();
    }
}

bool Playback::Seek(uint64_t frame, const std::function<bool()>& abort)
{
    const Route route = RouteTo(frame);
    if (route.from)
    {
        if (player_.Restore(*route.from->snapshot))
        {
            position_ = route.from->position;
            finished_at_ = route.from->finished_at;
            latest_ = route.from->snapshot;
        }
        else if (!Start())
        {
            return false;
        }
    }
    else if (route.restart && !Start())
    {
        return false;
    }

    scratch_.resize(kSeekBlock * 2);
    while (position_ < frame)
    {
        if (abort && abort())
        {
            return false;
        }

        const std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(kSeekBlock, frame - position_));
        if (Render(scratch_.data(), n) == 0)
        {
            break; // the end of the track
        }
    }

    return true;
}

} // namespace threesf
