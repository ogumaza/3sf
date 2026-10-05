// SPDX-License-Identifier: MIT

// Renders ahead of the read position on a worker thread (see render_ahead.h).

#include "threesf/render_ahead.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "threesf/playback.h"
#include "threesf/player.h"

namespace threesf
{
namespace
{

constexpr std::size_t kChunkFrames = 1024; // frames the worker renders at a time

// Interval between abort checks while Read waits.
constexpr auto kAbortPoll = std::chrono::milliseconds(20);

class PlaybackSource final : public RenderAhead::Source
{
public:
    explicit PlaybackSource(std::unique_ptr<Playback> playback) : playback_(std::move(playback))
    {
    }

    std::size_t Render(int16_t* out, std::size_t frames) override
    {
        return playback_->Render(out, frames);
    }

    bool Seek(uint64_t frame, const std::function<bool()>& abort) override
    {
        return playback_->Seek(frame, abort);
    }

    uint64_t Position() const override
    {
        return playback_->Position();
    }

    uint64_t SeekStart(uint64_t frame) const override
    {
        return playback_->SeekStart(frame);
    }

    std::string Error() const override
    {
        const Player& player = playback_->GetPlayer();
        return player.GetState() == Player::State::kError ? player.Error() : std::string();
    }

private:
    std::unique_ptr<Playback> playback_;
};

std::unique_ptr<RenderAhead::Source> FromPlayback(std::unique_ptr<Playback> playback, uint64_t keep_frames)
{
    // A track that fits stays buffered in full, so seeking never needs a snapshot.
    const uint64_t length = playback->LengthFrames();
    if (length != 0 && length <= keep_frames)
    {
        playback->SetSnapshots(false);
    }

    return std::make_unique<PlaybackSource>(std::move(playback));
}

// Each block holds an eighth of the buffer limit, capped at one second. Discarding one block leaves most of the
// buffered audio available for backward seeks.
std::size_t BlockFrames(uint64_t keep_frames)
{
    return static_cast<std::size_t>(std::clamp<uint64_t>(keep_frames / 8, 1, kPlayerSampleRate));
}

} // namespace

RenderAhead::RenderAhead(std::unique_ptr<Playback> playback, uint64_t keep_frames)
    : RenderAhead(FromPlayback(std::move(playback), keep_frames), keep_frames)
{
}

RenderAhead::RenderAhead(std::unique_ptr<Source> source, uint64_t keep_frames)
    : source_(std::move(source)), keep_(std::max<uint64_t>(keep_frames, 1)), block_(BlockFrames(keep_))
{
    start_ = end_ = read_ = source_->Position();
    thread_ = std::thread(&RenderAhead::Run, this);
}

RenderAhead::~RenderAhead()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }

    wanted_.notify_all();
    thread_.join();
}

std::size_t RenderAhead::Read(int16_t* out, std::size_t frames, const std::function<bool()>& abort)
{
    if (frames == 0)
    {
        return 0;
    }

    std::unique_lock lock(mutex_);
    while (true)
    {
        if (read_ >= start_ && read_ < end_)
        {
            const auto n = static_cast<std::size_t>(std::min<uint64_t>(frames, end_ - read_));
            Copy(out, read_, n);
            read_ += n;
            lock.unlock();
            wanted_.notify_one(); // the worker may have room to render more

            return n;
        }

        // No audio here yet. Once the worker handles the latest seek, ended_ and error_ say whether more is coming.
        const bool settled = handled_ == seeks_ && read_ >= start_;
        if (done_ || (settled && (ended_ || !error_.empty())))
        {
            return 0;
        }

        if (abort)
        {
            if (abort())
            {
                return 0;
            }

            rendered_.wait_for(lock, kAbortPoll);
        }
        else
        {
            rendered_.wait(lock);
        }
    }
}

void RenderAhead::Seek(uint64_t frame)
{
    {
        std::lock_guard lock(mutex_);
        read_ = frame;
        seeks_++;
    }

    wanted_.notify_one();
}

uint64_t RenderAhead::Position() const
{
    std::lock_guard lock(mutex_);
    return read_;
}

std::string RenderAhead::Error() const
{
    std::lock_guard lock(mutex_);
    return error_;
}

RenderAhead::Kept RenderAhead::GetKept() const
{
    std::lock_guard lock(mutex_);
    return {start_, end_, ended_};
}

void RenderAhead::Run()
{
    std::string error;
    try
    {
        Loop();
    }
    catch (const std::exception& e)
    {
        error = std::string("rendering failed: ") + e.what();
    }
    catch (...)
    {
        error = "rendering failed";
    }

    {
        std::lock_guard lock(mutex_);
        if (!error.empty())
        {
            error_ = error;
        }

        done_ = true;
    }

    rendered_.notify_all();
}

void RenderAhead::Loop()
{
    std::vector<int16_t> pcm(2 * kChunkFrames);
    std::size_t held = 0; // frames in pcm after end_, waiting to be buffered (see HasRoom)
    std::unique_lock lock(mutex_);
    while (!stop_)
    {
        if (handled_ != seeks_)
        {
            handled_ = seeks_;

            // Keep rendering if the target is buffered, pending, or quicker to reach without a source seek. Otherwise
            // seek the source and discard the buffer, which must cover one continuous range.
            const uint64_t target = read_;
            const uint64_t position = end_ + held; // source position
            if (target < start_ || (target > position && source_->SeekStart(target) > position))
            {
                blocks_.clear();
                held = 0;
                start_ = end_ = target;
                ended_ = false;
                error_.clear();
                const uint64_t seek = handled_;
                lock.unlock();
                const bool moved = source_->Seek(target, [&] { return stop_ || seeks_ != seek; });

                // A seek cancelled by another seek or by shutdown is not an error.
                std::string error = moved ? std::string() : source_->Error();
                const uint64_t position = source_->Position();
                lock.lock();
                start_ = end_ = position;
                error_ = std::move(error);
                rendered_.notify_all();
                continue;
            }

            // Wake readers waiting for this seek to be handled.
            rendered_.notify_all();
        }

        if (held > 0)
        {
            if (!HasRoom(held))
            {
                wanted_.wait(lock);
                continue;
            }

            Append(pcm.data(), held);
            held = 0;
            DropBehind();
            rendered_.notify_all();
            continue;
        }

        const uint64_t ahead = end_ > read_ ? end_ - read_ : 0;
        if (ended_ || !error_.empty() || ahead >= keep_)
        {
            wanted_.wait(lock);
            continue;
        }

        lock.unlock();
        const std::size_t n = source_->Render(pcm.data(), kChunkFrames);
        std::string error = n == 0 ? source_->Error() : std::string();
        lock.lock();
        if (n > 0)
        {
            held = n; // added to the buffer at the top of the loop
            continue;
        }

        if (error.empty())
        {
            ended_ = true;
        }
        else
        {
            error_ = std::move(error);
        }

        rendered_.notify_all();
    }
}

bool RenderAhead::HasRoom(std::size_t frames) const
{
    // Discount blocks that DropBehind can discard: whole blocks before the read position, except the last block.
    uint64_t kept = end_ - start_;
    if (blocks_.size() > 1 && read_ > start_)
    {
        kept -= std::min<uint64_t>((read_ - start_) / block_, blocks_.size() - 1) * block_;
    }

    return kept + frames <= keep_ + block_ + kChunkFrames;
}

void RenderAhead::Copy(int16_t* out, uint64_t from, std::size_t frames) const
{
    while (frames > 0)
    {
        const uint64_t offset = from - start_;
        const std::vector<int16_t>& block = blocks_[static_cast<std::size_t>(offset / block_)];
        const auto at = static_cast<std::size_t>(offset % block_);
        const std::size_t n = std::min(frames, block.size() / 2 - at);
        std::copy_n(block.begin() + static_cast<std::ptrdiff_t>(2 * at), 2 * n, out);
        out += 2 * n;
        from += n;
        frames -= n;
    }
}

void RenderAhead::Append(const int16_t* pcm, std::size_t frames)
{
    while (frames > 0)
    {
        if (blocks_.empty() || blocks_.back().size() == 2 * block_)
        {
            blocks_.emplace_back().reserve(2 * block_);
        }

        std::vector<int16_t>& back = blocks_.back();
        const std::size_t n = std::min(frames, block_ - back.size() / 2);
        back.insert(back.end(), pcm, pcm + 2 * n);
        pcm += 2 * n;
        frames -= n;
        end_ += n;
    }
}

void RenderAhead::DropBehind()
{
    // Discard only whole blocks before the read position, leaving nearby audio available for instant backward seeks.
    // Keep the last block until a new one follows it.
    while (end_ - start_ > keep_ && blocks_.size() > 1 && start_ + block_ <= read_)
    {
        blocks_.pop_front();
        start_ += block_;
    }
}

} // namespace threesf
