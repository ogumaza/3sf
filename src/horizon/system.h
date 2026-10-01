// SPDX-License-Identifier: MIT

// Ties the kernel, the HLE services and the DSP (Teakra running the game's firmware) together into one emulated 3DS
// application process.

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

// A process as a 3SF rip describes it: memory contents, RomFS files and the main thread.
struct ProcessImage
{
    struct Segment
    {
        uint32_t address;
        std::vector<uint8_t> data;
    };

    std::vector<Segment> memory; // applied in order; touched pages are mapped zero-filled
    std::vector<RomFs::File> files;
    uint32_t entry = 0x100000;
    uint32_t stack_size = 0x4000;
    int32_t priority = 0x30;
};

class System
{
public:
    System(const ProcessImage& image, const KernelConfig& config);
    ~System();

    Kernel& GetKernel()
    {
        return *kernel_;
    }

    // Stereo samples produced by the DSP so far (interleaved L, R at 32728 Hz).
    std::vector<int16_t> output_;

private:
    std::unique_ptr<Fcram> fcram_;
    std::unique_ptr<dsp::TeakraDsp> dsp_;
    std::unique_ptr<Kernel> kernel_;
};

} // namespace threesf::horizon
