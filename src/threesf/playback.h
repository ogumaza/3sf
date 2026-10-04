// SPDX-License-Identifier: MIT

// Playback of a 3SF set as a finite (or endless) stream: length, fade and volume from the tags or the defaults, end
// detection and seeking. Shared by the CLI renderer and the foobar2000 plugin so that they play a file identically.
//
// Seeking uses save states: as it renders, the player takes a snapshot of the emulation every few seconds (see
// Player::Save), and a seek goes back to the latest one before the target and renders from there.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "threesf/player.h"

namespace threesf
{

struct PlaybackOptions
{
    // The defaults of default_length_ms and default_fade_ms, which 3sfplay's help and the foobar2000 component's
    // preferences give in whole seconds.
    static constexpr long long kDefaultLengthMs = 150000;
    static constexpr long long kDefaultFadeMs = 10000;
    static_assert(kDefaultLengthMs % 1000 == 0 && kDefaultFadeMs % 1000 == 0);

    // Length and fade used when a file has no length tag.
    long long default_length_ms = kDefaultLengthMs;
    long long default_fade_ms = kDefaultFadeMs;

    // Play endlessly: ignore the length and fade, and don't stop at the end of the sound.
    bool endless = false;

    // Overrides of the length and fade tags (ignored when negative).
    long long length_override_ms = -1;
    long long fade_override_ms = -1;

    // Take snapshots while rendering, so that seeks are quick. They cost a little time and memory, which a render that
    // never seeks can save.
    bool snapshots = true;

    // Initial interval between snapshots, in frames: a seek renders up to this much. Shorter intervals reduce rendering
    // time when seeking, at the cost of more time and memory spent saving states.
    uint64_t snapshot_interval = 2 * static_cast<uint64_t>(kPlayerSampleRate);

    // Maximum number of snapshots. When exceeded, double the interval and discard every other snapshot to bound memory
    // use for long or endless tracks.
    std::size_t max_snapshots = 128;
};

// A file's play time before its fade, and the fade's length.
struct TrackLength
{
    long long play_ms = 0;
    long long fade_ms = 0;
};

// The length for a file whose length and fade tags give `length_ms` and `fade_ms` (negative when a tag is missing).
// Without a length tag, the file gets the default length, and the default fade unless it has a fade tag.
TrackLength ResolveLength(long long length_ms, long long fade_ms, const PlaybackOptions& options);

class Playback
{
public:
    static constexpr int kSampleRate = Player::kSampleRate;

    // Loads a file and its libraries, without starting emulation.
    bool Open(const std::string& path, const FileReader& reader, const PlaybackOptions& options = {});

    // Uses an already assembled set.
    bool Open(LoadedSet set, const PlaybackOptions& options);

    // Starts (or restarts) from the beginning.
    bool Start();

    // Renders up to `frames` stereo frames (interleaved L, R, 16-bit) with fade and volume applied. Returns the number
    // of frames written: fewer than requested at the end of the track, 0 after it or after an error (see Error()).
    std::size_t Render(int16_t* out, std::size_t frames);

    // Seeks to `frame` by rendering from the latest snapshot at or before it, or from the current position if nearer.
    // Within previously rendered audio, this requires less than one snapshot interval of rendering (PlaybackOptions).
    // Seeking further ahead renders the remaining distance. Without snapshots, backward seeks restart the emulation.
    // Polls `abort` between blocks and returns false if it requests cancellation.
    bool Seek(uint64_t frame, const std::function<bool()>& abort = {});

    // The snapshots kept, for tests.
    std::size_t SnapshotCount() const
    {
        return saved_.size();
    }

    // Current position in frames.
    uint64_t Position() const
    {
        return position_;
    }

    // Frames before the fade starts. Endless playback ignores this and FadeFrames.
    uint64_t PlayFrames() const
    {
        return play_frames_;
    }

    // The fade's length in frames.
    uint64_t FadeFrames() const
    {
        return fade_frames_;
    }

    // Total length in frames (0 when endless).
    uint64_t LengthFrames() const
    {
        return options_.endless ? 0 : play_frames_ + fade_frames_;
    }

    // True when the length comes from the file's tags or the options rather than the defaults.
    bool HasLengthTag() const
    {
        return tagged_;
    }

    const Tags& GetTags() const
    {
        return player_.GetTags();
    }

    const std::string& Error() const
    {
        return player_.Error();
    }

    Player& GetPlayer()
    {
        return player_;
    }

private:
    // A snapshot, and where playback was when it was taken.
    struct SavedState
    {
        uint64_t position = 0;
        std::optional<uint64_t> finished_at;
        std::shared_ptr<const Player::Snapshot> snapshot;
    };

    void ComputeLength();

    // Takes a snapshot if none has been taken yet in this stretch of interval_ frames.
    void SaveIfDue();

    Player player_;
    PlaybackOptions options_;
    bool tagged_ = false;
    uint64_t play_frames_ = 0;
    uint64_t fade_frames_ = 0;
    uint64_t position_ = 0;
    std::optional<uint64_t> finished_at_; // the frame at which the sound reported its end (untagged files)
    std::vector<int16_t> scratch_;

    // Declared after player_ so snapshots release their references to the player's objects before the player is
    // destroyed.
    std::vector<SavedState> saved_; // in order of position, at most one in each stretch of interval_ frames
    // The last one taken or restored, which the next shares pages with. It may be one that has gone from saved_.
    std::shared_ptr<const Player::Snapshot> latest_;
    uint64_t interval_ = 0;
};

} // namespace threesf
