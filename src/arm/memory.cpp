// SPDX-License-Identifier: MIT

// The address space of the ARM11 interpreter (see memory.h).

#include "arm/memory.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace threesf::arm
{

Memory::Memory() : pages_(new uint8_t*[kPageCount]()), write_pages_(new uint8_t*[kPageCount]())
{
}

void Memory::Map(uint32_t vaddr, uint8_t* host, uint32_t size)
{
    generation_ = next_generation_++;
    for (uint32_t off = 0; off < size; off += kPageSize)
    {
        pages_[(vaddr + off) >> kPageBits] = host + off;
        write_pages_[(vaddr + off) >> kPageBits] = Watched(host + off) ? nullptr : host + off;
    }
}

void Memory::Unmap(uint32_t vaddr, uint32_t size)
{
    generation_ = next_generation_++;
    for (uint32_t off = 0; off < size; off += kPageSize)
    {
        pages_[(vaddr + off) >> kPageBits] = nullptr;
        write_pages_[(vaddr + off) >> kPageBits] = nullptr;
    }
}

Memory::Snapshot Memory::Save(const Snapshot* previous) const
{
    Snapshot snapshot;
    snapshot.generation = generation_;
    snapshot.fault = fault_;
    snapshot.fault_address = fault_address_;
    snapshot.fault_write = fault_write_;

    if (previous && previous->generation == generation_)
    {
        snapshot.runs = previous->runs;
        return snapshot;
    }

    auto runs = std::make_shared<std::vector<Snapshot::Run>>();
    for (uint32_t page = 0; page < kPageCount; page++)
    {
        uint8_t* host = pages_[page];
        if (!host)
        {
            continue;
        }

        // Runs are at most 2 GiB, so that Restore can map each with a 32-bit size.
        if (!runs->empty())
        {
            Snapshot::Run& last = runs->back();
            const auto end = reinterpret_cast<std::uintptr_t>(last.host) + std::uintptr_t{last.pages} * kPageSize;
            if (last.first_page + last.pages == page && end == reinterpret_cast<std::uintptr_t>(host) &&
                last.pages < kPageCount / 2)
            {
                last.pages++;
                continue;
            }
        }

        runs->push_back({page, 1, host});
    }

    snapshot.runs = std::move(runs);

    return snapshot;
}

void Memory::Restore(const Snapshot& snapshot)
{
    fault_ = snapshot.fault;
    fault_address_ = snapshot.fault_address;
    fault_write_ = snapshot.fault_write;

    if (snapshot.generation == generation_)
    {
        return;
    }

    std::fill_n(pages_.get(), kPageCount, nullptr);
    std::fill_n(write_pages_.get(), kPageCount, nullptr);
    for (const Snapshot::Run& run : *snapshot.runs)
    {
        Map(run.first_page << kPageBits, run.host, run.pages << kPageBits);
    }

    generation_ = snapshot.generation;
}

void Memory::WatchWrites(const uint8_t* host, uint32_t size, std::function<void()> on_write)
{
    watch_start_ = host;
    watch_end_ = host + size;
    on_watched_write_ = std::move(on_write);
    for (uint32_t page = 0; page < kPageCount; ++page)
    {
        if (pages_[page] && Watched(pages_[page]))
        {
            write_pages_[page] = nullptr;
        }
    }
}

void Memory::WriteSlow8(uint32_t a, uint8_t v)
{
    uint8_t* p = pages_[a >> kPageBits];
    if (!p)
    {
        RecordFault(a, true);
        return;
    }

    p[a & kPageMask] = v;
    on_watched_write_();
}

template <typename Fn>
void Memory::ForEachMappedChunk(uint32_t vaddr, std::size_t size, Fn fn) const
{
    std::size_t offset = 0;
    while (offset < size)
    {
        uint8_t* p = HostPtr(vaddr);
        if (!p)
        {
            return;
        }

        const std::size_t chunk = std::min<std::size_t>(size - offset, kPageSize - (vaddr & kPageMask));
        fn(p, offset, chunk);
        vaddr += static_cast<uint32_t>(chunk);
        offset += chunk;
    }
}

void Memory::ReadBlock(uint32_t vaddr, void* dst, std::size_t size) const
{
    auto* d = static_cast<uint8_t*>(dst);
    const auto copy = [d](const uint8_t* host, std::size_t offset, std::size_t chunk)
    {
        std::memcpy(d + offset, host, chunk);
    };
    ForEachMappedChunk(vaddr, size, copy);
}

void Memory::WriteBlock(uint32_t vaddr, const void* src, std::size_t size)
{
    const auto* s = static_cast<const uint8_t*>(src);
    const auto copy = [this, s](uint8_t* host, std::size_t offset, std::size_t chunk)
    {
        std::memcpy(host, s + offset, chunk);
        if (Watched(host))
        {
            on_watched_write_();
        }
    };
    ForEachMappedChunk(vaddr, size, copy);
}

void Memory::ZeroBlock(uint32_t vaddr, std::size_t size)
{
    const auto zero = [this](uint8_t* host, std::size_t, std::size_t chunk)
    {
        std::memset(host, 0, chunk);
        if (Watched(host))
        {
            on_watched_write_();
        }
    };
    ForEachMappedChunk(vaddr, size, zero);
}

std::string Memory::ReadCString(uint32_t vaddr, std::size_t max) const
{
    std::string s;
    while (s.size() < max)
    {
        const uint8_t* p = HostPtr(vaddr++);
        if (!p || *p == 0)
        {
            break;
        }

        s.push_back(static_cast<char>(*p));
    }

    return s;
}

std::size_t Memory::MappedLength(uint32_t vaddr, std::size_t size) const
{
    uint64_t mapped = 0;
    while (mapped < size)
    {
        const uint32_t at = vaddr + static_cast<uint32_t>(mapped);
        if (!HostPtr(at))
        {
            break;
        }

        mapped += kPageSize - (at & kPageMask);
    }

    return static_cast<std::size_t>(std::min<uint64_t>(mapped, size));
}

} // namespace threesf::arm
