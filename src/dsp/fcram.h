// SPDX-License-Identifier: MIT

// The 3DS main memory (FCRAM) as the DSP reaches it. Wave data lives here at physical addresses from 0x20000000: the
// ARM side writes it and the DSP reads it over its AHBM bus. In game mode the game's code writes it, through the kernel
// that maps this block, and in archive mode the nw::snd model stores whole wave archives here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "common/common_types.h"
#include "common/memory_image.h"

namespace threesf
{

// Physical address of the start of FCRAM.
constexpr PAddr kFcramBase = 0x20000000;

class Fcram
{
public:
    // A copy of FCRAM's contents and of how much Store has used.
    struct Snapshot
    {
        MemoryImage bytes;
        std::size_t used = 0;
    };

    explicit Fcram(std::size_t size) : bytes_(size, 0)
    {
    }

    std::size_t Size() const
    {
        return bytes_.size();
    }

    // The `size` bytes at physical address `address`, or nullptr if any of them is outside FCRAM.
    uint8_t* At(PAddr address, std::size_t size = 1)
    {
        const uint64_t offset = uint64_t{address} - kFcramBase;
        return address >= kFcramBase && offset + size <= bytes_.size() ? bytes_.data() + offset : nullptr;
    }

    // Copies `size` bytes into the next free 32-byte aligned space (bump allocation, for the wave archives the nw::snd
    // model loads) and returns their physical address. Throws when FCRAM is full.
    PAddr Store(const void* data, std::size_t size)
    {
        const std::size_t offset = (used_ + 31) / 32 * 32;
        if (offset + size > bytes_.size())
        {
            throw std::runtime_error("FCRAM model exhausted");
        }

        std::memcpy(bytes_.data() + offset, data, size);
        used_ = offset + size;

        return kFcramBase + static_cast<PAddr>(offset);
    }

    // Takes a snapshot, sharing the pages that haven't changed since `previous`.
    Snapshot Save(const Snapshot* previous) const
    {
        return {MemoryImage(bytes_.data(), bytes_.size(), previous ? &previous->bytes : nullptr), used_};
    }

    // Restores FCRAM from a snapshot. Throws if the snapshot has a different size.
    void Restore(const Snapshot& snapshot)
    {
        if (snapshot.bytes.Size() != bytes_.size())
        {
            throw std::invalid_argument("FCRAM snapshot of a different size");
        }

        snapshot.bytes.Restore(bytes_.data());
        used_ = snapshot.used;
    }

private:
    std::vector<uint8_t> bytes_;
    std::size_t used_ = 0;
};

} // namespace threesf
