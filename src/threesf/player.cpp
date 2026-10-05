// SPDX-License-Identifier: MIT

// 3SF player core (see player.h).

#include "threesf/player.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "horizon/system.h"
#include "threesf/archive_player.h"

namespace threesf
{
namespace
{

// Emulated time run per step while producing output: 1 ms of ARM11 cycles.
constexpr uint64_t kStepCycles = horizon::kArmClock / 1000;

// Longest time allowed from boot until the driver reports that the sound plays.
constexpr uint64_t kBootTimeout = 10 * horizon::kArmClock;

} // namespace

class Player::Snapshot
{
public:
    uint64_t run = 0;
    State state = State::kIdle;
    uint64_t finished_frame = 0;
    uint64_t dropped = 0;         // samples produced before `pending`
    std::vector<int16_t> pending; // samples produced but not handed out yet
    std::shared_ptr<ArchivePlayer::Snapshot> archive;
    std::shared_ptr<horizon::System::Snapshot> system;
};

Player::Player() = default;

Player::~Player() = default;

bool Player::Fail(const std::string& message)
{
    error_ = message;
    state_ = State::kError;

    return false;
}

bool Player::Load(const std::string& path, const FileReader& reader)
{
    LoadedSet set;
    if (auto err = LoadSet(path, reader, set))
    {
        return Fail(*err);
    }

    return Load(std::move(set));
}

bool Player::Load(LoadedSet set)
{
    set_ = std::move(set);
    system_.reset();
    archive_.reset();
    state_ = State::kIdle;
    error_.clear();

    if (!set_.descriptor && !set_.archive)
    {
        return Fail("no process or archive descriptor");
    }

    return true;
}

uint32_t Player::DriverStatus() const
{
    if (archive_)
    {
        return archive_->Busy() ? 1 : 2;
    }

    const uint32_t addr = set_.descriptor ? set_.descriptor->status_address : 0;
    if (!addr || !system_)
    {
        return 1; // no status word: assume playing
    }

    return system_->GetKernel().Memory().Read32(addr);
}

std::vector<int16_t>& Player::Output()
{
    return archive_ ? archive_->output_ : system_->output_;
}

bool Player::Start()
{
    // Emulation failures (assertions in the emulators) arrive as exceptions.
    try
    {
        return StartImpl();
    }
    catch (const std::exception& e)
    {
        return Fail(std::string("emulation failed: ") + e.what());
    }
}

bool Player::StartImpl()
{
    run_++;

    if (set_.archive)
    {
        system_.reset();
        archive_ = std::make_unique<ArchivePlayer>();
        error_.clear();
        consumed_ = 0;
        dropped_ = 0;
        if (auto err = archive_->Start(set_))
        {
            return Fail(*err);
        }

        state_ = State::kPlaying;
        return true;
    }

    archive_.reset();

    if (!set_.descriptor)
    {
        return Fail("nothing loaded");
    }

    const ProcessDescriptor& d = *set_.descriptor;
    horizon::ProcessImage image;
    image.entry = d.entry;
    image.stack_size = d.stack_size;
    image.priority = static_cast<int32_t>(d.priority);
    for (const auto& m : set_.memory)
    {
        image.memory.push_back({m.address, m.data});
    }

    // A FILE chunk replaces an earlier one with the same path.
    std::map<std::string, const std::vector<uint8_t>*> files;
    for (const auto& f : set_.files)
    {
        files[f.path] = &f.data;
    }

    for (const auto& [path, data] : files)
    {
        image.files.push_back({path, std::make_shared<const std::vector<uint8_t>>(*data)});
    }

    horizon::KernelConfig config;
    config.app_memory = d.app_memory;
    system_ = std::make_unique<horizon::System>(image, config);
    system_->GetKernel().log_ = [](const std::string&) // drop the guest's debug output
    {
    };

    consumed_ = 0;
    error_.clear();

    // Run until the driver reports that the sound plays; discard the silence before it.
    horizon::Kernel& k = system_->GetKernel();
    while (true)
    {
        const uint32_t status = DriverStatus();
        if (status & 0x80000000u)
        {
            return Fail("driver error at step " + std::to_string(status & 0xff));
        }

        if (status == 1 || status == 2)
        {
            break;
        }

        if (k.Ticks() > kBootTimeout)
        {
            return Fail("timed out waiting for the sound to start");
        }

        if (!Step())
        {
            return false;
        }
    }

    system_->output_.clear();
    consumed_ = 0;
    dropped_ = 0;
    state_ = State::kPlaying;

    return true;
}

bool Player::Step()
{
    if (archive_)
    {
        try
        {
            if (!archive_->RunFrame())
            {
                return Fail("the DSP stopped producing frames");
            }
        }
        catch (const std::exception& e)
        {
            return Fail(std::string("emulation failed: ") + e.what());
        }

        return true;
    }

    horizon::Kernel& k = system_->GetKernel();
    horizon::Kernel::RunResult result;
    try
    {
        result = k.RunUntil(k.Ticks() + kStepCycles);
    }
    catch (const std::exception& e)
    {
        return Fail(std::string("emulation failed: ") + e.what());
    }

    switch (result)
    {
    case horizon::Kernel::RunResult::kOk:
        return true;
    case horizon::Kernel::RunResult::kExited:
        return Fail("the emulated program exited");
    case horizon::Kernel::RunResult::kError:
        return Fail(k.error_);
    }

    return Fail("unknown run result");
}

std::size_t Player::Render(int16_t* out, std::size_t frames)
{
    if (state_ != State::kPlaying && state_ != State::kFinished)
    {
        std::fill(out, out + frames * 2, int16_t{0});
        return 0;
    }

    auto& buf = Output();
    while ((buf.size() - consumed_) / 2 < frames)
    {
        if (!Step())
        {
            std::fill(out, out + frames * 2, int16_t{0});
            return 0;
        }

        if (state_ == State::kPlaying && DriverStatus() == 2)
        {
            state_ = State::kFinished;
            finished_frame_ = (dropped_ + buf.size()) / 2;
        }
    }

    std::copy(buf.begin() + static_cast<std::ptrdiff_t>(consumed_),
              buf.begin() + static_cast<std::ptrdiff_t>(consumed_ + frames * 2), out);
    consumed_ += frames * 2;

    // Drop consumed samples now and then to bound memory.
    if (consumed_ > (1u << 20))
    {
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed_));
        dropped_ += consumed_;
        consumed_ = 0;
    }

    return frames;
}

std::shared_ptr<const Player::Snapshot> Player::Save(const Snapshot* previous)
{
    if ((state_ != State::kPlaying && state_ != State::kFinished) || (!archive_ && !system_))
    {
        return nullptr;
    }

    if (previous && previous->run != run_)
    {
        previous = nullptr;
    }

    auto snapshot = std::make_shared<Snapshot>();
    snapshot->run = run_;
    snapshot->state = state_;
    snapshot->finished_frame = finished_frame_;
    const std::vector<int16_t>& buf = Output();
    snapshot->pending.assign(buf.begin() + static_cast<std::ptrdiff_t>(consumed_), buf.end());
    snapshot->dropped = dropped_ + consumed_;
    try
    {
        if (archive_)
        {
            snapshot->archive = archive_->Save(previous ? previous->archive.get() : nullptr);
        }
        else
        {
            snapshot->system = system_->Save(previous ? previous->system.get() : nullptr);
        }
    }
    catch (const std::exception&)
    {
        // Saving only reads the emulation's state, so playback goes on without the snapshot.
        return nullptr;
    }

    return snapshot;
}

bool Player::Restore(const Snapshot& snapshot)
{
    if (snapshot.run != run_ || (archive_ ? !snapshot.archive : !system_ || !snapshot.system))
    {
        return false;
    }

    try
    {
        if (archive_)
        {
            archive_->Restore(*snapshot.archive);
        }
        else
        {
            system_->Restore(*snapshot.system);
        }
    }
    catch (const std::exception& e)
    {
        return Fail(std::string("restoring the emulation's state failed: ") + e.what());
    }

    Output() = snapshot.pending;
    consumed_ = 0;
    dropped_ = snapshot.dropped;
    finished_frame_ = snapshot.finished_frame;
    state_ = snapshot.state;
    error_.clear();

    return true;
}

long long Player::LengthMs() const
{
    auto it = set_.tags.find("length");
    return it == set_.tags.end() ? -1 : ParseTime(it->second);
}

long long Player::FadeMs() const
{
    auto it = set_.tags.find("fade");
    return it == set_.tags.end() ? -1 : ParseTime(it->second);
}

double Player::Volume() const
{
    auto it = set_.tags.find("volume");
    if (it == set_.tags.end())
    {
        return 1.0;
    }

    // A value that isn't a finite number, such as nan, is ignored: it would turn every sample into nonsense.
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return end != it->second.c_str() && std::isfinite(v) ? v : 1.0;
}

} // namespace threesf
