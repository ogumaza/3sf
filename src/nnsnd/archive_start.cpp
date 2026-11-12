// SPDX-License-Identifier: MIT

#include "nnsnd/archive_start.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/sar/formats.h"

#include "nnsnd/aux_effects.h"

namespace threesf::nnsnd
{

bool StartArchiveModel(ntdlib::ctr::snd::ArchiveModel& model, std::span<const uint8_t> firmware,
                       const ntdlib::sar::SoundArchive& archive, uint8_t output_mode, const AuxEffectLists& effects)
{
    // The effects of each bus. The aux callback of the bus keeps them by value. A snapshot of the sound system copies
    // the callbacks. Each copy keeps the state of the effects.
    std::array<AuxEffects, kAuxBusCount> buses;
    for (std::size_t bus = 0; bus < buses.size(); bus++)
    {
        buses[bus] = AuxEffects(effects[bus]);
    }

    if (!model.Start(firmware, archive, output_mode))
    {
        return false;
    }

    // The game gives each of the firmware's delays a line in its memory, in silence (0x22d5d8 and 0x22d9ec in Luigi's
    // Mansion of 2018). Each of the firmware's reverbs gets its lines in one buffer (0x80cef0 to 0x80cf2c in StreetPass
    // Mii Plaza's update v14.6.0). The rips give that buffer in silence too.
    std::array<uint32_t, kAuxBusCount> delay_lines{};
    std::array<uint32_t, kAuxBusCount> reverb_buffers{};
    for (std::size_t bus = 0; bus < buses.size(); bus++)
    {
        if (const std::optional<FirmwareDelay>& delay = buses[bus].FirmwareDelayOnBus())
        {
            const std::vector<uint8_t> silence(delay->LineBytes());
            delay_lines[bus] = model.fcram.Store(silence.data(), silence.size());
        }

        if (const std::optional<FirmwareReverb>& reverb = buses[bus].FirmwareReverbOnBus())
        {
            const std::vector<uint8_t> silence(reverb->BufferBytes());
            reverb_buffers[bus] = model.fcram.Store(silence.data(), silence.size());
        }
    }

    for (std::size_t bus = 0; bus < buses.size(); bus++)
    {
        if (!buses[bus].Any())
        {
            continue;
        }

        const auto b = static_cast<int>(bus);
        if (const std::optional<FirmwareDelay>& delay = buses[bus].FirmwareDelayOnBus())
        {
            model.snd.SetFirmwareDelay(b, delay->Record(delay_lines[bus]));
        }

        if (const std::optional<FirmwareReverb>& reverb = buses[bus].FirmwareReverbOnBus())
        {
            model.snd.SetFirmwareReverb(b, reverb->Record(reverb_buffers[bus]));
        }

        model.snd.SetAuxReturnVolume(b, 1.0f);
        auto run_effects =
            [bus_effects = std::move(buses[bus])](const std::array<int32_t*, 4>& channels, std::size_t samples) mutable
        {
            bus_effects.Process(channels, samples);
        };
        model.snd.SetAuxCallback(b, std::move(run_effects));
    }

    return true;
}

} // namespace threesf::nnsnd
