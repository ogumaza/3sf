// SPDX-License-Identifier: MIT

// The emulated process (see system.h).

#include "horizon/system.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "arm/cpu.h"
#include "dsp/dsp.h"
#include "dsp/fcram.h"
#include "dsp/teakra_dsp.h"
#include "horizon/kernel.h"
#include "horizon/services.h"

namespace threesf::horizon
{
namespace
{

// The main thread's stack ends here, and DSP RAM starts here.
constexpr uint32_t kStackTop = 0x10000000;
constexpr uint32_t kDspRam = 0x1ff00000;

constexpr uint64_t kPageSize = arm::Memory::kPageSize;

// A run of pages, [first, end), in page numbers.
struct PageRange
{
    uint64_t first;
    uint64_t end;
};

// The pages of the segments that hold data, sorted, with ranges that overlap or touch merged.
std::vector<PageRange> ProgramPages(const ProcessImage& image)
{
    std::vector<PageRange> ranges;
    for (const ProcessImage::Segment& segment : image.memory)
    {
        if (!segment.data.empty())
        {
            const uint64_t end = uint64_t{segment.address} + segment.data.size();
            ranges.push_back({segment.address / kPageSize, (end + kPageSize - 1) / kPageSize});
        }
    }

    const auto starts_first = [](const PageRange& a, const PageRange& b)
    {
        return a.first != b.first ? a.first < b.first : a.end < b.end;
    };
    std::sort(ranges.begin(), ranges.end(), starts_first);

    std::vector<PageRange> merged;
    for (const PageRange& range : ranges)
    {
        if (!merged.empty() && range.first <= merged.back().end)
        {
            merged.back().end = std::max(merged.back().end, range.end);
        }
        else
        {
            merged.push_back(range);
        }
    }

    return merged;
}

// Where the program ends: the end of the range of pages that holds the entry point. A rip's driver sits apart from the
// program and doesn't count. With the entry point outside every range, the config's value stays.
uint32_t ProgramEnd(const std::vector<PageRange>& ranges, uint32_t entry, uint32_t otherwise)
{
    const uint64_t page = entry / kPageSize;
    for (const PageRange& range : ranges)
    {
        if (page >= range.first && page < range.end)
        {
            return static_cast<uint32_t>(range.end * kPageSize);
        }
    }

    return otherwise;
}

KernelConfig WithProgramEnd(KernelConfig config, uint32_t program_end)
{
    config.program_end = program_end;

    return config;
}

} // namespace

struct System::Snapshot
{
    Kernel::Snapshot kernel;
    Services::State services;
    Fcram::Snapshot fcram;
    dsp::TeakraDsp::Snapshot dsp;
};

// The order of the steps fixes the mappings, the handles and the results of overlapping segments. A region mapped
// later replaces any mapping of the same pages.
System::System(const ProcessImage& image, const KernelConfig& config, const dsp::UpstreamTeakra& upstream)
    : fcram_(std::size_t{config.app_memory * 2}), dsp_(fcram_, upstream)
{
    const std::vector<PageRange> program = ProgramPages(image);
    kernel_ = std::make_unique<Kernel>(WithProgramEnd(config, ProgramEnd(program, image.entry, config.program_end)));
    Kernel& kernel = *kernel_;
    arm::Memory& memory = kernel.Memory();
    kernel.SetLinearMemory(fcram_.At(kFcramBase, fcram_.Size()), static_cast<uint32_t>(fcram_.Size()));

    // The program's pages, then its segments in the image's order: a later segment overwrites an earlier one.
    for (const PageRange& range : program)
    {
        kernel.MapNewRegion(static_cast<uint32_t>(range.first * kPageSize),
                            static_cast<uint32_t>((range.end - range.first) * kPageSize));
    }

    for (const ProcessImage::Segment& segment : image.memory)
    {
        memory.WriteBlock(segment.address, segment.data.data(), segment.data.size());
    }

    // The main thread's stack, and DSP RAM. The guest and the DSP share DSP RAM. A write to the DSP's program memory
    // calls ProgramWritten.
    const uint32_t stack_size = arm::Memory::RoundUpToPage(image.stack_size);
    kernel.MapNewRegion(kStackTop - stack_size, stack_size);
    memory.Map(kDspRam, dsp_.Ram(), dsp::kRamSize);
    memory.WatchWrites(dsp_.Ram(), dsp::kDataMemoryOffset, [this] { dsp_.ProgramWritten(); });

    // The services.
    services_ = std::make_unique<Services>(kernel, dsp_, image.files);
    kernel.SetServiceHost(services_.get());

    // The DSP's steps with the clock.
    const auto step_dsp = [this](uint64_t ticks)
    {
        const uint64_t target = ticks / 2;
        const uint64_t cycles = dsp_.Cycles();
        if (cycles < target)
        {
            dsp_.Run(target - cycles);
        }
    };
    kernel.SetDeviceStep(step_dsp);

    // The DSP's samples.
    const auto keep_sample = [this](std::array<int16_t, 2> sample)
    {
        output_.push_back(sample[0]);
        output_.push_back(sample[1]);
    };
    dsp_.SetSampleSink(keep_sample);

    // The main thread.
    kernel.CreateThread(image.priority, image.entry, 0, kStackTop);
}

System::~System() = default;

Kernel& System::GetKernel()
{
    return *kernel_;
}

std::shared_ptr<System::Snapshot> System::Save(const Snapshot* previous)
{
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->kernel = kernel_->Save(previous ? &previous->kernel : nullptr);
    snapshot->services = services_->Save();
    snapshot->fcram = fcram_.Save(previous ? &previous->fcram : nullptr);
    snapshot->dsp = dsp_.Save(previous ? &previous->dsp : nullptr);

    return snapshot;
}

void System::Restore(const Snapshot& snapshot)
{
    kernel_->Restore(snapshot.kernel);
    services_->Restore(snapshot.services);
    fcram_.Restore(snapshot.fcram);
    dsp_.Restore(snapshot.dsp);
}

} // namespace threesf::horizon
