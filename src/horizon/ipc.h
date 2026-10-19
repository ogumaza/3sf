// SPDX-License-Identifier: MIT

// IPC between the process and the services (3dbrew, "IPC"): the command buffer in a thread's local storage, the
// header word of a request or reply, and the descriptors of the handles and buffers that a reply gives.

#pragma once

#include <cstdint>
#include <initializer_list>

#include "arm/cpu.h"

namespace threesf::horizon::ipc
{

// The command buffer's offset in a thread's local storage, and the offset of the descriptors of the thread's receive
// buffers there: two words for each buffer id, a descriptor and then the buffer's address.
constexpr uint32_t kCommandBuffer = 0x80;
constexpr uint32_t kReceiveBuffers = 0x180;

// A header word: the command id in bits 16 to 31, the count of normal words in bits 6 to 11 and the count of translate
// words in bits 0 to 5.
constexpr uint32_t Header(uint32_t command, uint32_t normal, uint32_t translate)
{
    return command << 16 | normal << 6 | translate;
}

// The descriptor of `count` handles that a reply copies: each one is a new handle to an existing object.
constexpr uint32_t CopyHandles(uint32_t count)
{
    return (count - 1) << 26;
}

// The descriptor of `count` handles that a reply moves: each one is a new session.
constexpr uint32_t MoveHandles(uint32_t count)
{
    return (count - 1) << 26 | 0x10;
}

// The descriptor of `size` bytes in the static buffer with id `id`.
constexpr uint32_t StaticBuffer(uint32_t size, uint32_t id)
{
    return size << 14 | id << 10 | 2;
}

// A thread's command buffer in guest memory, with the header of the request that it holds. Each word is a single
// access. An access to an unmapped address sets the memory's fault flag.
class CommandBuffer
{
public:
    // Reads the request's header from word 0.
    CommandBuffer(arm::Memory& memory, uint32_t address)
        : memory_(memory), address_(address), request_header_(memory.Read32(address))
    {
    }

    uint32_t Word(uint32_t index) const
    {
        return memory_.Read32(address_ + 4 * index);
    }

    uint32_t RequestHeader() const
    {
        return request_header_;
    }

    uint32_t CommandId() const
    {
        return request_header_ >> 16;
    }

    // Writes a reply from word 0. The header gives the request's command id and the counts of normal and translate
    // words. The words follow the header. The result code comes first.
    void Reply(uint32_t normal, uint32_t translate, std::initializer_list<uint32_t> words)
    {
        memory_.Write32(address_, Header(CommandId(), normal, translate));
        uint32_t address = address_ + 4;
        for (const uint32_t word : words)
        {
            memory_.Write32(address, word);
            address += 4;
        }
    }

private:
    arm::Memory& memory_;
    uint32_t address_;
    uint32_t request_header_;
};

} // namespace threesf::horizon::ipc
