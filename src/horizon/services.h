// SPDX-License-Identifier: MIT

// HLE services for the 3SF player: srv:, err:f, fs:USER (RomFS only), dsp::DSP (on the Teakra DSP), cfg:u, APT:U, and
// logging stubs for everything else. Command ids and reply layouts follow 3dbrew's documentation, checked against
// Azahar's behaviour.

#pragma once

#include <memory>

#include "dsp/teakra_dsp.h"
#include "horizon/kernel.h"
#include "horizon/romfs.h"

namespace threesf::horizon
{

// Registers the services and the srv: and err:f ports with the kernel, which keeps them.
void InstallServices(Kernel& kernel, std::shared_ptr<RomFs> romfs, dsp::TeakraDsp& dsp);

} // namespace threesf::horizon
