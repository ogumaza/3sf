// SPDX-License-Identifier: MIT

// Renders a Playback ahead of the reader on a worker thread and buffers the audio. Seeks within the buffer are instant.
// Seeks further ahead wait for the worker to reach the target. Seeking back before the buffered audio calls
// Playback::Seek, which restores a snapshot.
//
// A track that fits within the buffer limit needs no snapshots, and once it has been rendered to the end, every seek is
// instant. Rendering ahead does the same work as normal playback, but sooner: it keeps one core busy until it reaches
// the end of the track or the buffer limit. Buffered audio uses about 7.5 MiB per minute.
//
// The foobar2000 component reads the buffer on its decoding thread.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "threesf/format.h"

namespace threesf
{

class Playback;

class RenderAhead
{
public:
    // Audio source for the worker: a Playback or a test substitute. Only the worker calls it after it starts.
    class Source
    {
    public:
        virtual ~Source() = default;

        // Like Playback::Render: up to `frames` stereo frames, fewer at the end, and 0 after it or after an error.
        virtual std::size_t Render(int16_t* out, std::size_t frames) = 0;

        // Like Playback::Seek: returns false when `abort` stopped it, or after an error.
        virtual bool Seek(uint64_t frame, const std::function<bool()>& abort) = 0;

        // The frame the next Render starts at.
        virtual uint64_t Position() const = 0;

        // The frame a seek to `frame` would start rendering from (see Playback::SeekStart).
        virtual uint64_t SeekStart(uint64_t frame) const = 0;

        // Returns the reason rendering stopped early, or an empty string if no error occurred.
        virtual std::string Error() const = 0;
    };

    // Buffered range for tests: frames [start, end), and whether the track ends at `end`.
    struct Kept
    {
        uint64_t start = 0;
        uint64_t end = 0;
        bool ended = false;
    };

    // Buffer limit: 10 minutes plus a 10-second fade, enough for the longest track 3sfrip's length analysis produces.
    // That's about 76 MiB.
    static constexpr uint64_t kKeepFrames = 610 * static_cast<uint64_t>(kPlayerSampleRate);

    // Takes ownership of a started Playback and begins rendering. Tracks that fit in `keep_frames` have snapshots
    // disabled, since all rendered audio stays in the buffer.
    explicit RenderAhead(std::unique_ptr<Playback> playback, uint64_t keep_frames = kKeepFrames);

    // Takes ownership of a source and starts rendering from its current position.
    explicit RenderAhead(std::unique_ptr<Source> source, uint64_t keep_frames = kKeepFrames);

    RenderAhead(const RenderAhead&) = delete;
    RenderAhead& operator=(const RenderAhead&) = delete;

    // Stops the worker and waits for it to exit: it finishes the chunk it's rendering, but abandons a seek after the
    // current block.
    ~RenderAhead();

    // Copies up to `frames` stereo frames (interleaved L, R) from the read position into `out`, and moves the read
    // position past them. Waits until the worker has rendered at least one, polling `abort`. Returns the number copied,
    // which is 0 at the end of the track, after an error (see Error()) or when `abort` returned true.
    std::size_t Read(int16_t* out, std::size_t frames, const std::function<bool()>& abort = {});

    // Moves the read position to `frame` and returns immediately. If the frame isn't buffered, the next Read waits for
    // it.
    void Seek(uint64_t frame);

    // The read position.
    uint64_t Position() const;

    // Returns the error, or an empty string if none occurred. Read returns the audio before the error first.
    std::string Error() const;

    Kept GetKept() const;

private:
    void Run();
    void Loop();

    // Copies frames [from, from + frames) from the buffer.
    void Copy(int16_t* out, uint64_t from, std::size_t frames) const;

    // Appends frames to the buffer.
    void Append(const int16_t* pcm, std::size_t frames);

    // Discards the oldest blocks before the read position while the buffer holds more than keep_ frames.
    void DropBehind();

    // Checks whether `frames` more fit once DropBehind has discarded what it can, allowing at most keep_ + block_ + one
    // chunk in the buffer. Rendering on while fewer than keep_ frames are ahead of the read position stays within that,
    // but a backward seek while a chunk is rendering can leave fewer blocks to discard: then the worker holds the chunk
    // until the reader advances.
    bool HasRoom(std::size_t frames) const;

    std::unique_ptr<Source> source_;
    const uint64_t keep_;
    const std::size_t block_; // frames per buffer block

    mutable std::mutex mutex_;
    std::condition_variable rendered_; // the worker has rendered, reached the end, failed, moved or stopped
    std::condition_variable wanted_;   // the reader has read, sought or asked the worker to stop

    // Guarded by mutex_.
    std::deque<std::vector<int16_t>> blocks_; // frames [start_, end_), block_ in each but the last
    uint64_t start_ = 0;
    uint64_t end_ = 0;     // the source's position, except while the worker moves it
    uint64_t read_ = 0;    // the read position
    uint64_t handled_ = 0; // latest seek handled by the worker
    bool ended_ = false;
    bool done_ = false; // the worker has exited
    std::string error_;

    // Changed with mutex_ held, and read without it by a seek's abort check.
    std::atomic<uint64_t> seeks_{0}; // counts Seek calls
    std::atomic<bool> stop_{false};

    std::thread thread_; // started by the constructor once the rest is set up, and joined by the destructor
};

} // namespace threesf
