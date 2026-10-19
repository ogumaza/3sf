// SPDX-License-Identifier: MIT

// 3SF player core (see player.h).

#include "threesf/player.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xsflib/player/modes.h"

#include "horizon/system.h"
#include "threesf/archive_player.h"
#include "threesf/game_mode.h"

namespace threesf
{

struct Player::SourceSnapshot : xsflib::player::ModeSnapshot<ArchivePlayer::Snapshot, horizon::System::Snapshot>
{
};

Player::Player() = default;

Player::~Player() = default;

void Player::Unload()
{
    system_.reset();
    archive_.reset();
}

bool Player::Playable() const
{
    return set_.descriptor || set_.archive;
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

bool Player::StartSource()
{
    if (set_.archive)
    {
        system_.reset();
        archive_ = std::make_unique<ArchivePlayer>();
        if (auto err = archive_->Start(set_))
        {
            return Fail(*err);
        }

        return true;
    }

    archive_.reset();
    system_ = MakeGameSystem(set_);

    // Run until the driver reports that the sound plays; discard the silence before it.
    return RunUntilSoundPlays(kGameBootTimeout);
}

bool Player::RunStep()
{
    if (archive_)
    {
        if (!archive_->RunFrame())
        {
            return Fail("the DSP stopped producing frames");
        }

        return true;
    }

    horizon::Kernel& k = system_->GetKernel();
    switch (k.RunUntil(k.Ticks() + kGameStepCycles))
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

bool Player::Finished() const
{
    return DriverStatus() == 2;
}

uint64_t Player::EmulatedCycles() const
{
    return system_->GetKernel().Ticks();
}

std::shared_ptr<const Player::SourceSnapshot> Player::SaveSource(const SourceSnapshot* previous)
{
    return xsflib::player::SaveModes(archive_.get(), system_.get(), previous);
}

bool Player::CanRestore(const SourceSnapshot* source) const
{
    return xsflib::player::CanRestoreModes(archive_.get(), system_.get(), source);
}

void Player::RestoreSource(const SourceSnapshot& source)
{
    xsflib::player::RestoreModes(archive_.get(), system_.get(), source);
}

} // namespace threesf
