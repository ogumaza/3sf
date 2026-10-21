// SPDX-License-Identifier: MIT

// Game mode's emulated system (see game_mode.h).

#include "threesf/game_mode.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dsp/teakra_dsp.h"
#include "horizon/kernel.h"
#include "horizon/system.h"
#include "threesf/format.h"

namespace threesf
{

std::unique_ptr<horizon::System> MakeGameSystem(const LoadedSet& set, horizon::KernelConfig config,
                                                const dsp::UpstreamTeakra& upstream)
{
    const ProcessDescriptor& d = *set.descriptor;
    horizon::ProcessImage image;
    image.entry = d.entry;
    image.stack_size = d.stack_size;
    image.priority = static_cast<int32_t>(d.priority);
    for (const auto& m : set.memory)
    {
        image.memory.push_back({m.address, m.data});
    }

    // A FILE chunk replaces an earlier one with the same path.
    std::map<std::string, const std::vector<uint8_t>*> files;
    for (const auto& f : set.files)
    {
        files[f.path] = &f.data;
    }

    for (const auto& [path, data] : files)
    {
        image.files.push_back({path, std::make_shared<const std::vector<uint8_t>>(*data)});
    }

    config.app_memory = d.app_memory;
    auto system = std::make_unique<horizon::System>(image, config, upstream);
    system->GetKernel().log_ = [](const std::string&) // drop the guest's debug output
    {
    };

    return system;
}

} // namespace threesf
