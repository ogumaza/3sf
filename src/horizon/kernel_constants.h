// SPDX-License-Identifier: MIT

// The constants and the helper that the source files of the Horizon kernel HLE share: kernel.cpp, kernel_memory.cpp,
// kernel_sync.cpp and kernel_svc.cpp. Nothing else includes this header.

#pragma once

#include <cstdint>

#include "arm/memory.h"

namespace threesf::horizon
{

// Result codes.
inline constexpr uint32_t kTimeout = 0x09401bfe;
inline constexpr uint32_t kInvalidHandle = 0xd8e007f7;
inline constexpr uint32_t kNotFound = 0xd8e007fa;
inline constexpr uint32_t kOutOfMemory = 0xd86007f3;
inline constexpr uint32_t kInvalidAddress = 0xe0e01bf5;
inline constexpr uint32_t kMisalignedAddress = 0xe0e01bf1;
inline constexpr uint32_t kMisalignedSize = 0xe0e01bf2;
inline constexpr uint32_t kOutOfRange = 0xd8e007fd;
inline constexpr uint32_t kNotOwner = 0xd8e0041f;
inline constexpr uint32_t kInvalidEnum = 0xd8e093ed;
inline constexpr uint32_t kNotImplemented = 0xf8601bfd;

// The address map: the heap, linear memory (page n of FCRAM at kLinearBase + n pages), configuration memory, the shared
// page and the slots of thread-local storage, eight to a page.
inline constexpr uint32_t kHeapBase = 0x08000000;
inline constexpr uint32_t kHeapEnd = 0x0e000000;
inline constexpr uint32_t kLinearBase = 0x14000000;
inline constexpr uint32_t kConfigMemory = 0x1ff80000;
inline constexpr uint32_t kSharedPage = 0x1ff81000;
inline constexpr uint32_t kTlsBase = 0x1ff82000;
inline constexpr uint32_t kTlsSize = 0x200;
inline constexpr uint32_t kTlsSlotsPerPage = 8;

inline constexpr uint32_t kPageSize = arm::Memory::kPageSize;
inline constexpr uint32_t kPageMask = arm::Memory::kPageMask;

// The reset type of an event or timer that a wait clears, and of one that a signal clears at once.
inline constexpr uint32_t kOneShot = 0;
inline constexpr uint32_t kPulse = 2;

// The index that WaitSynchronizationN reports when no single object ends the wait: when the thread gets all its
// objects, and on a timeout. It's -1 as a u32.
inline constexpr uint32_t kNoWaitIndex = 0xffffffff;

// A time in nanoseconds from two registers: the s64 with `high` in its upper 32 bits.
inline int64_t Nanoseconds(uint32_t low, uint32_t high)
{
    return static_cast<int64_t>(uint64_t{high} << 32 | low);
}

} // namespace threesf::horizon
