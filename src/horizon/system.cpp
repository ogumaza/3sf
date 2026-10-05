// SPDX-License-Identifier: MIT

// Emulated application process (see system.h).

#include "horizon/system.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "horizon/services.h"

namespace threesf::horizon
{

System::System(const ProcessImage& image, const KernelConfig& config)
{
    fcram_ = std::make_unique<Fcram>(config.app_memory * 2);
    dsp_ = std::make_unique<dsp::TeakraDsp>(*fcram_);

    // Merge the pages the memory segments touch into contiguous regions.
    constexpr uint32_t kPage = arm::Memory::kPageSize;
    std::vector<std::pair<uint32_t, uint32_t>> ranges; // [first page, end page)
    for (const auto& seg : image.memory)
    {
        if (seg.data.empty())
        {
            continue;
        }

        const uint64_t end = static_cast<uint64_t>(seg.address) + seg.data.size();
        ranges.push_back({seg.address / kPage, static_cast<uint32_t>((end + kPage - 1) / kPage)});
    }

    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (const auto& r : ranges)
    {
        if (!merged.empty() && r.first <= merged.back().second)
        {
            merged.back().second = std::max(merged.back().second, r.second);
        }
        else
        {
            merged.push_back(r);
        }
    }

    // The program's code, data and bss are the region that holds the entry point. A rip's driver sits apart from it.
    KernelConfig kernel_config = config;
    for (const auto& [first, last] : merged)
    {
        if (image.entry / kPage >= first && image.entry / kPage < last)
        {
            kernel_config.program_end = last * kPage;
        }
    }

    kernel_ = std::make_unique<Kernel>(kernel_config);
    Kernel& k = *kernel_;
    k.SetFcram(fcram_->At(kFcramBase), static_cast<uint32_t>(fcram_->Size()));

    // Map the regions, then copy the segments in.
    for (const auto& [first, last] : merged)
    {
        k.MapRegion(first * kPage, (last - first) * kPage);
    }

    for (const auto& seg : image.memory)
    {
        k.Memory().WriteBlock(seg.address, seg.data.data(), seg.data.size());
    }

    // Main thread stack.
    const uint32_t stack = (image.stack_size + kPage - 1) & ~(kPage - 1);
    k.MapRegion(kStackTop - stack, stack);

    // DSP RAM (the application maps the shared-memory windows; map all of it).
    k.MapExternal(kDspRamBase, dsp_->Ram(), dsp::kRamSize);

    // Notify Teakra of writes to the program half so it can invalidate translated code.
    k.Memory().WatchWrites(dsp_->Ram(), dsp::kRamSize / 2, [this] { dsp_->ProgramWritten(); });

    InstallServices(k, std::make_shared<RomFs>(image.files), *dsp_);

    // The DSP runs at half the ARM11 clock, in lock-step with emulated time.
    k.device_step_ = [this](uint64_t to)
    {
        const uint64_t target = to / 2;
        const uint64_t now = dsp_->Cycles();
        if (target > now)
        {
            dsp_->Run(target - now);
        }
    };

    const auto capture = [this](const std::array<int16_t, 2>& s)
    {
        output_.push_back(s[0]);
        output_.push_back(s[1]);
    };
    dsp_->SetSampleSink(capture);

    k.CreateMainThread(image.entry, kStackTop, image.priority);
}

System::~System() = default;

struct System::Snapshot
{
    Fcram::Snapshot fcram;
    dsp::TeakraDsp::Snapshot dsp;
    std::shared_ptr<Kernel::Snapshot> kernel;
};

std::shared_ptr<System::Snapshot> System::Save(const Snapshot* previous)
{
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->fcram = fcram_->Save(previous ? &previous->fcram : nullptr);
    snapshot->dsp = dsp_->Save(previous ? &previous->dsp : nullptr);
    snapshot->kernel = kernel_->Save(previous ? previous->kernel.get() : nullptr);

    return snapshot;
}

void System::Restore(const Snapshot& snapshot)
{
    fcram_->Restore(snapshot.fcram);
    dsp_->Restore(snapshot.dsp);
    kernel_->Restore(*snapshot.kernel);
}

} // namespace threesf::horizon
