// SPDX-License-Identifier: MIT

// The address space of the ARM11 interpreter (see cpu.h), with snapshots and a write watch.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

namespace threesf::arm
{

// Flat 32-bit address space backed by host memory in 4 KiB pages.
class Memory
{
public:
    // Snapshot of the mapping: runs of pages mapped to consecutive host memory. The kernel and system own the host
    // memory and keep its addresses stable. The snapshot can only be restored to this address space.
    struct Snapshot
    {
        struct Run
        {
            uint32_t first_page;
            uint32_t pages;
            uint8_t* host;
        };

        std::shared_ptr<const std::vector<Run>> runs; // shared with the previous snapshot while the mapping is the same
        uint64_t generation = 0;
        bool fault = false;
        uint32_t fault_address = 0;
        bool fault_write = false;
    };

    static constexpr uint32_t kPageBits = 12;
    static constexpr uint32_t kPageSize = 1u << kPageBits;
    static constexpr uint32_t kPageMask = kPageSize - 1;
    static constexpr uint32_t kPageCount = 1u << (32 - kPageBits);

    Memory();

    // A size rounded up to a whole number of pages, in u32 arithmetic. A size above 0xFFFFF000 rounds to 0.
    static constexpr uint32_t RoundUpToPage(uint32_t size)
    {
        return (size + kPageMask) & ~kPageMask;
    }

    // Map maps [vaddr, vaddr + size) to the host memory at `host`. Unmap removes the mapping of that range. size must
    // be page aligned. An address inside a page stands for the start of that page.
    void Map(uint32_t vaddr, uint8_t* host, uint32_t size);
    void Unmap(uint32_t vaddr, uint32_t size);

    // Calls `on_write` after CPU or kernel writes (WriteBlock, ZeroBlock) to [host, host + size), through any current
    // or future virtual mapping. host and size must be page aligned. Supports one watch at a time, with no overhead for
    // writes to other pages.
    void WatchWrites(const uint8_t* host, uint32_t size, std::function<void()> on_write);

    // Host pointer for a virtual address, or nullptr if unmapped.
    uint8_t* HostPtr(uint32_t vaddr) const
    {
        uint8_t* p = pages_[vaddr >> kPageBits];
        return p ? p + (vaddr & kPageMask) : nullptr;
    }

    uint8_t Read8(uint32_t a)
    {
        if (uint8_t* p = pages_[a >> kPageBits])
        {
            return p[a & kPageMask];
        }

        return static_cast<uint8_t>(RecordFault(a, false));
    }

    uint16_t Read16(uint32_t a)
    {
        uint8_t* p = pages_[a >> kPageBits];
        if (p && (a & kPageMask) <= kPageSize - 2)
        {
            return xsflib::LoadLe16(p + (a & kPageMask));
        }

        return static_cast<uint16_t>(Read8(a) | (Read8(a + 1) << 8));
    }

    uint32_t Read32(uint32_t a)
    {
        uint8_t* p = pages_[a >> kPageBits];
        if (p && (a & kPageMask) <= kPageSize - 4)
        {
            return xsflib::LoadLe32(p + (a & kPageMask));
        }

        return Read16(a) | (static_cast<uint32_t>(Read16(a + 2)) << 16);
    }

    void Write8(uint32_t a, uint8_t v)
    {
        if (uint8_t* p = write_pages_[a >> kPageBits])
        {
            p[a & kPageMask] = v;
        }
        else
        {
            WriteSlow8(a, v);
        }
    }

    void Write16(uint32_t a, uint16_t v)
    {
        uint8_t* p = write_pages_[a >> kPageBits];
        if (p && (a & kPageMask) <= kPageSize - 2)
        {
            xsflib::StoreLe16(p + (a & kPageMask), v);
            return;
        }

        Write8(a, static_cast<uint8_t>(v));
        Write8(a + 1, static_cast<uint8_t>(v >> 8));
    }

    void Write32(uint32_t a, uint32_t v)
    {
        uint8_t* p = write_pages_[a >> kPageBits];
        if (p && (a & kPageMask) <= kPageSize - 4)
        {
            xsflib::StoreLe32(p + (a & kPageMask), v);
            return;
        }

        Write16(a, static_cast<uint16_t>(v));
        Write16(a + 2, static_cast<uint16_t>(v >> 16));
    }

    // Copy between host buffers and virtual memory, for the kernel HLE. They stop at the first unmapped page, without
    // setting the fault flag.
    void ReadBlock(uint32_t vaddr, void* dst, std::size_t size) const;
    void WriteBlock(uint32_t vaddr, const void* src, std::size_t size);
    void ZeroBlock(uint32_t vaddr, std::size_t size);
    std::string ReadCString(uint32_t vaddr, std::size_t max) const;

    // The bytes of [vaddr, vaddr + size) that a block copy reaches: those before the first unmapped page.
    std::size_t MappedLength(uint32_t vaddr, std::size_t size) const;

    // Takes a snapshot, reusing `previous`'s runs if nothing has been mapped or unmapped since.
    Snapshot Save(const Snapshot* previous) const;

    void Restore(const Snapshot& snapshot);

    // Set by the first access to unmapped memory. Cpu::Run stops with StopReason::kFault, and the caller clears the
    // flag once it has handled the fault.
    bool Fault() const
    {
        return fault_;
    }

    void ClearFault()
    {
        fault_ = false;
    }

    // The address of the first access to unmapped memory, and whether it was a write.
    uint32_t FaultAddress() const
    {
        return fault_address_;
    }

    bool FaultWrite() const
    {
        return fault_write_;
    }

private:
    // A write to a watched page (see WatchWrites) or an unmapped one.
    void WriteSlow8(uint32_t a, uint8_t v);

    // Calls fn(host, offset, chunk) for each piece of [vaddr, vaddr + size) that lies in one page, up to the first
    // unmapped page. host is the address of the piece in host memory, offset the number of bytes before the piece and
    // chunk the size of the piece.
    template <typename Fn>
    void ForEachMappedChunk(uint32_t vaddr, std::size_t size, Fn fn) const;

    bool Watched(const uint8_t* host) const
    {
        return host >= watch_start_ && host < watch_end_;
    }

    uint32_t RecordFault(uint32_t a, bool write)
    {
        if (!fault_)
        {
            fault_ = true;
            fault_address_ = a;
            fault_write_ = write;
        }

        return 0;
    }

    bool fault_ = false;
    uint32_t fault_address_ = 0;
    bool fault_write_ = false;
    std::unique_ptr<uint8_t*[]> pages_;

    // The same pages for writing, except that watched pages are missing, so that their writes take the slow path.
    std::unique_ptr<uint8_t*[]> write_pages_;

    const uint8_t* watch_start_ = nullptr;
    const uint8_t* watch_end_ = nullptr;
    std::function<void()> on_watched_write_;

    // Mapping generation. Map and Unmap assign a new number, so snapshots with the same number share the same mapping.
    uint64_t generation_ = 0;
    uint64_t next_generation_ = 1;
};

} // namespace threesf::arm
