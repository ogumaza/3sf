// SPDX-License-Identifier: MIT

// Game mode's emulated system (docs/3sf.md, "The emulated system"): the 3DS application process that a game-mode set
// describes. The player makes it here. So can a program that runs the system with other kernel settings, or with
// upstream Teakra's behaviour.

#pragma once

#include <cstdint>
#include <memory>

#include "dsp/teakra_dsp.h"
#include "horizon/kernel.h"
#include "horizon/system.h"
#include "threesf/format.h"

namespace threesf
{

// The emulated time that the player runs per step in game mode: 1 ms of ARM11 cycles.
constexpr uint64_t kGameStepCycles = horizon::kArmClock / 1000;

// The longest emulated time that the player waits after boot for the driver to report that the sound plays.
constexpr uint64_t kGameBootTimeout = 10 * horizon::kArmClock;

// Makes the system for the process that `set` describes: its process descriptor, MEM chunks and FILE chunks. A FILE
// chunk replaces an earlier one with the same path. The descriptor gives the kernel's application memory. `config`
// gives the kernel's other settings. The kernel drops the guest's debug output. `upstream` gives the DSP upstream
// Teakra's behaviour where 3SF's copy departs from it.
std::unique_ptr<horizon::System> MakeGameSystem(const LoadedSet& set, horizon::KernelConfig config = {},
                                                const dsp::UpstreamTeakra& upstream = {});

} // namespace threesf
