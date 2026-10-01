// SPDX-License-Identifier: MIT

// IPC command buffer helpers for the HLE services (see 3dbrew "IPC").
//
// A request is a header word (command id << 16 | normal words << 6 | translate words) followed by the normal parameters
// and then the translate parameters (handles, static buffers, mapped buffers). HLE services read requests and write
// replies in place in the caller's TLS.

#pragma once

#include <cstdint>
#include <vector>

#include "horizon/kernel.h"

namespace threesf::horizon
{

constexpr uint32_t IpcHeader(uint32_t command, uint32_t normal, uint32_t translate)
{
    return (command << 16) | (normal << 6) | translate;
}

// Translate descriptors
constexpr uint32_t IpcCopyHandles(uint32_t count)
{
    return (count - 1) << 26;
}

constexpr uint32_t IpcMoveHandles(uint32_t count)
{
    return ((count - 1) << 26) | 0x10;
}

constexpr uint32_t IpcStaticBuffer(uint32_t size, uint32_t id)
{
    return (size << 14) | (id << 10) | 2;
}

class IpcContext
{
public:
    IpcContext(Kernel& kernel, uint32_t cmdbuf) : kernel_(kernel), base_(cmdbuf)
    {
        header_ = kernel.Read32(cmdbuf);
    }

    uint32_t Command() const
    {
        return header_ >> 16;
    }

    uint32_t Header() const
    {
        return header_;
    }

    // Word i of the command buffer (word 0 is the header).
    uint32_t Word(uint32_t i) const
    {
        return kernel_.Read32(base_ + 4 * i);
    }

    uint64_t Word64(uint32_t i) const
    {
        return Word(i) | (static_cast<uint64_t>(Word(i + 1)) << 32);
    }

    void SetWord(uint32_t i, uint32_t v)
    {
        kernel_.Write32(base_ + 4 * i, v);
    }

    void SetWord64(uint32_t i, uint64_t v)
    {
        SetWord(i, static_cast<uint32_t>(v));
        SetWord(i + 1, static_cast<uint32_t>(v >> 32));
    }

    // Starts a reply: header with the same command id and word 1 = result.
    void Reply(uint32_t normal, uint32_t translate, uint32_t result = kResultSuccess)
    {
        SetWord(0, IpcHeader(Command(), normal, translate));
        SetWord(1, result);
    }

    // Copies data into the caller's receive static buffer `id` and returns its address.
    uint32_t WriteStaticBuffer(uint32_t id, const void* data, uint32_t size)
    {
        const uint32_t tls = base_ - kIpcCommandOffset;
        const uint32_t addr = kernel_.Read32(tls + 0x180 + 8 * id + 4);
        kernel_.Memory().WriteBlock(addr, data, size);

        return addr;
    }

    std::vector<uint8_t> ReadBuffer(uint32_t addr, uint32_t size) const
    {
        std::vector<uint8_t> v(size);
        kernel_.Memory().ReadBlock(addr, v.data(), size);

        return v;
    }

private:
    Kernel& kernel_;
    uint32_t base_;
    uint32_t header_;
};

} // namespace threesf::horizon
