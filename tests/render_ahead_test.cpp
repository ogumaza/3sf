// SPDX-License-Identifier: MIT

// Tests rendering ahead (src/threesf/render_ahead.h) with a test source that generates samples from a formula. Reads
// must return the correct samples, seeks within the buffer must not move the source, and the buffer must stay within
// its memory limit. Errors must reach the reader after any preceding audio. Reads and the worker must stop when asked.
// A random test covers combinations of reads and seeks across many tracks.
//
// usage: render_ahead_test [rounds of the random test]

#include "threesf/render_ahead.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "check.h"

using namespace threesf;

namespace
{

using Clock = std::chrono::steady_clock;

// The sample at `frame` in `channel`. No two frames have the same pair below 2^32 frames.
int16_t Sample(uint64_t frame, int channel)
{
    return static_cast<int16_t>(static_cast<uint16_t>(channel == 0 ? frame : (frame >> 16) ^ 0x5A5A));
}

// Source activity, read by the test while the worker runs.
struct Counts
{
    std::atomic<int> seeks{0};
    std::atomic<uint64_t> rendered{0}; // frames rendered, including during seeks
};

struct Settings
{
    uint64_t length = 100000;
    uint64_t interval = 0;                   // frames between snapshots, or 0 for none
    std::optional<uint64_t> fail_at;         // where rendering fails
    std::chrono::microseconds delay{0};      // delay per Render call
    std::chrono::microseconds seek_delay{0}; // delay per block rendered during a seek
};

// Settings for a track of `length` frames with no rendering delay, snapshots or errors.
Settings Track(uint64_t length)
{
    Settings settings;
    settings.length = length;

    return settings;
}

// A test substitute for Playback that generates a track with Sample(). Simulates snapshots every `interval` frames up
// to the furthest rendered position. Like Playback, seeks render from the latest snapshot at or before the target, the
// current position if nearer, or the start of the track.
class FakeSource : public RenderAhead::Source
{
public:
    FakeSource(const Settings& settings, Counts& counts) : settings_(settings), counts_(counts)
    {
    }

    std::size_t Render(int16_t* out, std::size_t frames) override
    {
        if (settings_.delay.count() > 0)
        {
            std::this_thread::sleep_for(settings_.delay);
        }

        const uint64_t end = std::min(settings_.length, settings_.fail_at.value_or(UINT64_MAX));
        if (failed_ || position_ >= end)
        {
            failed_ = failed_ || position_ >= settings_.fail_at.value_or(UINT64_MAX);
            return 0;
        }

        const auto n = static_cast<std::size_t>(std::min<uint64_t>(frames, end - position_));
        for (std::size_t i = 0; i < n; i++)
        {
            out[2 * i] = Sample(position_ + i, 0);
            out[2 * i + 1] = Sample(position_ + i, 1);
        }

        position_ += n;
        furthest_ = std::max(furthest_, position_);
        counts_.rendered += n;

        return n;
    }

    bool Seek(uint64_t frame, const std::function<bool()>& abort) override
    {
        counts_.seeks++;
        position_ = SeekStart(frame);
        failed_ = false;
        std::vector<int16_t> block(2 * 4096);
        while (position_ < frame)
        {
            if (abort && abort())
            {
                return false;
            }

            if (settings_.seek_delay.count() > 0)
            {
                std::this_thread::sleep_for(settings_.seek_delay);
            }

            if (Render(block.data(), static_cast<std::size_t>(std::min<uint64_t>(4096, frame - position_))) == 0)
            {
                break; // end of track or error; the next Render reports it, as in Playback
            }
        }

        return true;
    }

    uint64_t Position() const override
    {
        return position_;
    }

    uint64_t SeekStart(uint64_t frame) const override
    {
        std::optional<uint64_t> snapshot;
        if (settings_.interval)
        {
            snapshot = std::min(frame, furthest_) / settings_.interval * settings_.interval;
        }

        if (snapshot && (failed_ || frame < position_ || *snapshot > position_))
        {
            return *snapshot;
        }

        return failed_ || frame < position_ ? 0 : position_;
    }

    std::string Error() const override
    {
        return failed_ ? "failed at frame " + std::to_string(position_) : std::string();
    }

private:
    Settings settings_;
    Counts& counts_;
    uint64_t position_ = 0;
    uint64_t furthest_ = 0;
    bool failed_ = false;
};

std::unique_ptr<RenderAhead> Make(const Settings& settings, Counts& counts, uint64_t keep = RenderAhead::kKeepFrames)
{
    return std::make_unique<RenderAhead>(std::make_unique<FakeSource>(settings, counts), keep);
}

// Reads up to `frames` frames in chunks of at most `chunk`, checking samples against the track starting at `from`.
// Returns the number read, which may be fewer at the end of the track or after an error.
uint64_t ReadAndCheck(RenderAhead& ahead, uint64_t from, uint64_t frames, std::size_t chunk, const std::string& what)
{
    std::vector<int16_t> pcm(2 * chunk);
    uint64_t done = 0;
    while (done < frames)
    {
        const std::size_t n =
            ahead.Read(pcm.data(), static_cast<std::size_t>(std::min<uint64_t>(chunk, frames - done)));
        if (n == 0)
        {
            break;
        }

        for (std::size_t i = 0; i < n; i++)
        {
            const uint64_t frame = from + done + i;
            if (pcm[2 * i] != Sample(frame, 0) || pcm[2 * i + 1] != Sample(frame, 1))
            {
                Check(false, what + ": frame " + std::to_string(frame) + " matches the track");
                return done + i;
            }
        }

        done += n;
    }

    return done;
}

// Waits up to a minute for the buffer to reach `frame` or the end of the track. Waits a little longer afterwards to
// catch workers that render beyond the limit.
RenderAhead::Kept WaitForKept(const RenderAhead& ahead, uint64_t frame)
{
    const auto deadline = Clock::now() + std::chrono::minutes(1);
    while (Clock::now() < deadline)
    {
        const RenderAhead::Kept kept = ahead.GetKept();
        if (kept.end >= frame || kept.ended)
        {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    return ahead.GetKept();
}

void TestStraight()
{
    Counts counts;
    auto ahead = Make(Track(50000), counts);

    THREESF_CHECK(ReadAndCheck(*ahead, 0, 60000, 1024, "straight") == 50000);
    int16_t pcm[2];
    THREESF_CHECK(ahead->Read(pcm, 1) == 0);
    THREESF_CHECK(ahead->Error().empty());
    THREESF_CHECK(ahead->Position() == 50000);
    THREESF_CHECK(counts.seeks == 0);
}

void TestSeeksIntoKept()
{
    Counts counts;
    auto ahead = Make(Track(200000), counts);

    const RenderAhead::Kept kept = WaitForKept(*ahead, UINT64_MAX);
    THREESF_CHECK(kept.start == 0 && kept.end == 200000 && kept.ended);

    std::mt19937 rng(7);
    for (int i = 0; i < 100; i++)
    {
        const uint64_t target = rng() % 200000;
        ahead->Seek(target);
        const uint64_t want = std::min<uint64_t>(3000, 200000 - target);
        Check(ReadAndCheck(*ahead, target, 3000, 1000, "seek within buffer") == want,
              "a seek to " + std::to_string(target) + " reads 3000 frames or stops at the end of the track");
    }

    ahead->Seek(200005);
    int16_t pcm[2];
    THREESF_CHECK(ahead->Read(pcm, 1) == 0 && ahead->Error().empty());
    THREESF_CHECK(counts.seeks == 0);
    THREESF_CHECK(counts.rendered == 200000);
}

void TestKeepLimit()
{
    constexpr uint64_t kKeep = 50000;
    Counts counts;
    auto ahead = Make(Track(1000000), counts, kKeep);

    RenderAhead::Kept kept = WaitForKept(*ahead, kKeep);
    THREESF_CHECK(kept.start == 0 && kept.end >= kKeep && kept.end < kKeep + 1024);
    THREESF_CHECK(counts.rendered == kept.end);

    THREESF_CHECK(ReadAndCheck(*ahead, 0, 200000, 1024, "keep limit") == 200000);
    kept = WaitForKept(*ahead, 200000 + kKeep);
    THREESF_CHECK(kept.end >= 200000 + kKeep && kept.end < 200000 + kKeep + 1024);
    THREESF_CHECK(kept.start > 0 && kept.start <= 200000);
    THREESF_CHECK(kept.end - kept.start <= kKeep + kKeep / 8 + 1 + 1024);

    // Seeking to the start of the buffer needs no source seek.
    ahead->Seek(kept.start);
    THREESF_CHECK(ReadAndCheck(*ahead, kept.start, 2000, 1024, "seek to start of buffer") == 2000);
    THREESF_CHECK(counts.seeks == 0);
}

void TestSeekBack(uint64_t interval)
{
    const std::string what = interval ? "seek back with snapshots" : "seek back without snapshots";
    Counts counts;
    Settings settings = Track(1000000);
    settings.interval = interval;
    auto ahead = Make(settings, counts, 50000);

    Check(ReadAndCheck(*ahead, 0, 300000, 1024, what) == 300000, what + ": the first reads");

    // Seeking before the buffer requires a source seek.
    ahead->Seek(5000);
    Check(ReadAndCheck(*ahead, 5000, 20000, 1024, what) == 20000, what + ": reads after the seek");
    Check(counts.seeks == 1, what + ": sought the source");

    // Seeking ahead of the buffer uses a snapshot if one is closer to the target, or continues rendering otherwise.
    ahead->Seek(290000);
    Check(ReadAndCheck(*ahead, 290000, 20000, 1024, what) == 20000, what + ": reads after the seek forwards");
    Check(counts.seeks == (interval ? 2 : 1), what + ": the source moved forwards only to a snapshot");
}

void TestSeekPastKept()
{
    // Delay rendering so the reader gets ahead of the worker.
    Counts counts;
    Settings settings = Track(1000000);
    settings.delay = std::chrono::microseconds(100);
    auto ahead = Make(settings, counts);

    ahead->Seek(300000);

    THREESF_CHECK(ReadAndCheck(*ahead, 300000, 5000, 1024, "seek ahead of buffer") == 5000);
    THREESF_CHECK(counts.seeks == 0);
    THREESF_CHECK(ahead->GetKept().start == 0);
}

void TestError()
{
    Counts counts;
    Settings settings = Track(100000);
    settings.fail_at = 30000;
    auto ahead = Make(settings, counts);

    THREESF_CHECK(ReadAndCheck(*ahead, 0, 100000, 1024, "error") == 30000);
    THREESF_CHECK(!ahead->Error().empty());

    // Audio before the error stays buffered. Seeking past it reports the error again.
    ahead->Seek(10000);
    THREESF_CHECK(ReadAndCheck(*ahead, 10000, 1000, 1024, "before the error") == 1000);
    ahead->Seek(40000);
    int16_t pcm[2];
    THREESF_CHECK(ahead->Read(pcm, 1) == 0 && !ahead->Error().empty());

    // Seeking the source backwards renders up to the error again.
    Counts counts2;
    settings.interval = 4000;
    auto small = Make(settings, counts2, 5000);
    THREESF_CHECK(ReadAndCheck(*small, 0, 100000, 1024, "error, small") == 30000);
    small->Seek(1000);
    THREESF_CHECK(ReadAndCheck(*small, 1000, 100000, 1024, "error, from a snapshot") == 29000);
    THREESF_CHECK(counts2.seeks == 1 && !small->Error().empty());
}

void TestAbort()
{
    Counts counts;
    Settings settings = Track(100000000);
    settings.delay = std::chrono::milliseconds(2);
    auto ahead = Make(settings, counts);
    ahead->Seek(50000000);
    int16_t pcm[2048];

    const auto t0 = Clock::now();
    const std::size_t n = ahead->Read(pcm, 1024, [&] { return Clock::now() - t0 > std::chrono::milliseconds(50); });
    const auto waited = Clock::now() - t0;

    THREESF_CHECK(n == 0);
    THREESF_CHECK(waited < std::chrono::seconds(2));
}

void TestStopWhileBusy()
{
    // Stop while rendering towards a distant seek target.
    {
        Counts counts;
        Settings settings = Track(100000000);
        settings.delay = std::chrono::milliseconds(2);
        auto ahead = Make(settings, counts);
        ahead->Seek(50000000);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        const auto t0 = Clock::now();
        ahead.reset();

        THREESF_CHECK(Clock::now() - t0 < std::chrono::seconds(1));
    }

    // Stop during a source seek. Rendering from the start takes about 2 s, at 20 ms per block.
    {
        Counts counts;
        Settings settings = Track(1000000);
        settings.seek_delay = std::chrono::milliseconds(20);
        auto ahead = Make(settings, counts, 10000);
        THREESF_CHECK(ReadAndCheck(*ahead, 0, 450000, 1024, "stop while busy") == 450000);
        ahead->Seek(400000);
        while (counts.seeks == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const auto t0 = Clock::now();
        ahead.reset();

        THREESF_CHECK(Clock::now() - t0 < std::chrono::seconds(1));
    }
}

// Varies track settings, buffer limits, seek targets and read sizes at random. Checks the samples from every read and
// verifies that the buffer stays within its memory limit.
void TestRandom(int rounds)
{
    std::mt19937_64 rng(2026);
    for (int round = 0; round < rounds; round++)
    {
        Settings settings;
        settings.length = 1 + rng() % 300000;
        if (rng() % 2)
        {
            settings.interval = 1 + rng() % 20000;
        }

        if (rng() % 4 == 0)
        {
            settings.fail_at = rng() % (settings.length + 1000);
        }

        if (rng() % 3 == 0)
        {
            settings.delay = std::chrono::microseconds(rng() % 200);
        }

        const uint64_t keep = rng() % 3 ? 1 + rng() % 100000 : RenderAhead::kKeepFrames;
        const uint64_t limit = std::min(settings.length, settings.fail_at.value_or(UINT64_MAX));
        const bool fails = settings.fail_at && *settings.fail_at <= settings.length;
        const std::string what = "random round " + std::to_string(round);

        Counts counts;
        auto ahead = Make(settings, counts, keep);
        uint64_t position = 0;
        std::vector<int16_t> pcm(2 * 5000);
        for (int op = 0; op < 60 && !failures; op++)
        {
            if (rng() % 3 == 0)
            {
                position = rng() % (settings.length + 2000);
                ahead->Seek(position);
                if (rng() % 2)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
                }

                continue;
            }

            const std::size_t frames = 1 + rng() % 5000;
            const std::size_t n = ahead->Read(pcm.data(), frames);
            if (n == 0)
            {
                Check(position >= limit, what + ": read returned no frames at " + std::to_string(position) +
                                             " before the end at " + std::to_string(limit));
                Check(ahead->Error().empty() != fails, what + ": error status matches source failure");
                continue;
            }

            Check(n <= frames && position + n <= limit, what + ": read stays within requested and available frames");
            for (std::size_t i = 0; i < n; i++)
            {
                if (pcm[2 * i] != Sample(position + i, 0) || pcm[2 * i + 1] != Sample(position + i, 1))
                {
                    Check(false, what + ": frame " + std::to_string(position + i) + " matches the track");
                    break;
                }
            }

            position += n;
            Check(ahead->Position() == position, what + ": read position advances by the number of frames returned");

            // Allow the buffer limit plus one block (at most keep / 8, but at least one frame) and a 1,024-frame chunk.
            const RenderAhead::Kept kept = ahead->GetKept();
            Check(kept.end - kept.start <= keep + keep / 8 + 1 + 1024, what + ": buffer stays within its memory limit");
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 200;

    TestStraight();
    TestSeeksIntoKept();
    TestKeepLimit();
    TestSeekBack(0);
    TestSeekBack(10000);
    TestSeekPastKept();
    TestError();
    TestAbort();
    TestStopWhileBusy();
    TestRandom(rounds);

    if (failures)
    {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }

    std::fprintf(stderr, "render ahead test passed\n");

    return 0;
}
