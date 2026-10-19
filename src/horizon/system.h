// SPDX-License-Identifier: MIT

// The emulated 3DS application process of game mode: the kernel (kernel.h) with the services (services.h), FCRAM
// and the DSP running its firmware in Teakra (dsp/teakra_dsp.h), stepped in lock-step with the ARM11.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "dsp/fcram.h"
#include "dsp/teakra_dsp.h"
#include "horizon/kernel.h"
#include "horizon/romfs.h"

namespace threesf::horizon
{

class Services;

// The process that a game-mode rip holds: memory segments, RomFS files, and the main thread's entry point, stack size
// and priority.
struct ProcessImage
{
    struct Segment
    {
        uint32_t address;
        std::vector<uint8_t> data;
    };

    std::vector<Segment> memory;
    std::vector<RomFs::File> files;
    uint32_t entry = 0x100000;
    uint32_t stack_size = 0x4000;
    int32_t priority = 0x30;
};

class System
{
public:
    // The whole process at one moment, apart from output_.
    struct Snapshot;

    // Builds the process. `upstream` gives the DSP upstream Teakra's behaviour where 3SF's copy departs from it.
    System(const ProcessImage& image, const KernelConfig& config, const dsp::UpstreamTeakra& upstream = {});
    System(const System&) = delete;
    System& operator=(const System&) = delete;
    ~System();

    Kernel& GetKernel();

    // Takes a snapshot, sharing what hasn't changed with `previous`, null or an earlier snapshot of this System.
    std::shared_ptr<Snapshot> Save(const Snapshot* previous);

    // Returns the process to a snapshot that this System took.
    void Restore(const Snapshot& snapshot);

    // The DSP's output: interleaved left and right samples at 268111856 / 8192 Hz.
    std::vector<int16_t> output_;

private:
    Fcram fcram_;
    dsp::TeakraDsp dsp_;
    std::unique_ptr<Kernel> kernel_;
    std::unique_ptr<Services> services_;
};

} // namespace threesf::horizon
