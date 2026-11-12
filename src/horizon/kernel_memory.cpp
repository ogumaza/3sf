// SPDX-License-Identifier: MIT

// The memory management of the Horizon kernel HLE (see kernel.h): ControlMemory, the memory blocks and the host memory
// regions that the kernel maps.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "arm/cpu.h"
#include "arm/memory.h"
#include "horizon/kernel.h"
#include "horizon/kernel_constants.h"

namespace threesf::horizon
{
namespace
{

// ControlMemory's operations, and the flag that selects linear memory.
constexpr uint32_t kOperationFree = 1;
constexpr uint32_t kOperationCommit = 3;
constexpr uint32_t kOperationMap = 4;
constexpr uint32_t kOperationUnmap = 5;
constexpr uint32_t kOperationProtect = 6;
constexpr uint32_t kOperationLinear = 0x10000;

} // namespace

void Kernel::SvcControlMemory()
{
    arm::CpuState& r = cpu_.State();
    uint32_t address = 0;
    const uint32_t result = ControlMemory(r.r[0], r.r[1], r.r[2], r.r[3], address);
    r.r[0] = result;
    r.r[1] = result == kSuccess ? address : 0;
}

// Runs the operation in the low byte of `operation`. Every operation but a linear commit gives addr0 as the address.
uint32_t Kernel::ControlMemory(uint32_t operation, uint32_t addr0, uint32_t addr1, uint32_t size, uint32_t& address)
{
    if ((addr0 & kPageMask) != 0 || (addr1 & kPageMask) != 0)
    {
        return kMisalignedAddress;
    }

    const bool linear = (operation & kOperationLinear) != 0;
    size = arm::Memory::RoundUpToPage(size);
    address = addr0;
    switch (operation & 0xff)
    {
    case kOperationCommit:
        return linear ? CommitLinear(addr0, size, address) : CommitHeap(addr0, size);

    case kOperationFree:
        FreeMemory(addr0, size, linear);
        return kSuccess;

    case kOperationMap:
        // The pages of addr1 appear at addr0 too.
        return MapAlias(addr0, addr1, size) ? kSuccess : kInvalidAddress;

    case kOperationUnmap:
        memory_.Unmap(addr0, size);
        return kSuccess;

    case kOperationProtect:
        // Permissions don't exist: every page can be read, written and executed.
        return kSuccess;

    default:
        Log("ControlMemory: unsupported operation %08x", operation);
        return kNotImplemented;
    }
}

// Commits heap memory: a new region of zeros at addr0. Heap memory isn't part of FCRAM. A commit over mapped pages
// replaces them.
uint32_t Kernel::CommitHeap(uint32_t addr0, uint32_t size)
{
    if (addr0 < kHeapBase || addr0 > kHeapEnd || size > kHeapEnd - addr0)
    {
        return kInvalidAddress;
    }

    MapNewRegion(addr0, size);
    state_.heap_size += size;

    return kSuccess;
}

// Commits linear memory: FCRAM's pages at the address given, or at the first free run of pages for an address of 0.
// Nothing checks that pages at a given address are free.
uint32_t Kernel::CommitLinear(uint32_t addr0, uint32_t size, uint32_t& address)
{
    State& s = state_;
    const uint64_t pages = size / kPageSize;
    const std::optional<uint64_t> first = addr0 == 0 ? FirstFreePages(pages) : (addr0 - kLinearBase) / kPageSize;
    if (!first || *first + pages > s.fcram_used->size())
    {
        return kOutOfMemory;
    }

    std::vector<uint8_t>& used = FcramUsedForChange();
    std::fill_n(used.begin() + static_cast<std::ptrdiff_t>(*first), static_cast<std::size_t>(pages), 1);
    uint8_t* const host = fcram_ + static_cast<std::size_t>(*first * kPageSize);
    std::memset(host, 0, size);
    address = kLinearBase + static_cast<uint32_t>(*first * kPageSize);
    memory_.Map(address, host, size);
    s.linear_size += size;

    return kSuccess;
}

// The first page of the earliest run of `pages` free pages of FCRAM, or nothing when no run is that long. A run of 0
// pages starts just after the first page in use. There's none when no page is in use.
std::optional<uint64_t> Kernel::FirstFreePages(uint64_t pages) const
{
    const std::vector<uint8_t>& used = *state_.fcram_used;
    uint64_t run = 0;
    for (uint64_t page = 0; page < used.size(); page++)
    {
        run = used[static_cast<std::size_t>(page)] ? 0 : run + 1;
        if (run == pages)
        {
            return page + 1 - pages;
        }
    }

    return std::nullopt;
}

// Unmaps the pages at addr0 and takes their size off the heap size or the linear size. A free in linear memory gives
// FCRAM's pages back without clearing them.
void Kernel::FreeMemory(uint32_t addr0, uint32_t size, bool linear)
{
    State& s = state_;
    if (linear || (addr0 >= kLinearBase && addr0 - kLinearBase < fcram_size_))
    {
        const uint64_t first = (addr0 - kLinearBase) / kPageSize;
        const uint64_t end = std::min<uint64_t>(first + size / kPageSize, s.fcram_used->size());
        if (first < end)
        {
            std::vector<uint8_t>& used = FcramUsedForChange();
            std::fill(used.begin() + static_cast<std::ptrdiff_t>(first),
                      used.begin() + static_cast<std::ptrdiff_t>(end), 0);
        }

        s.linear_size -= std::min(size, s.linear_size);
    }
    else
    {
        s.heap_size -= std::min(size, s.heap_size);
    }

    memory_.Unmap(addr0, size);
}

// Maps the host pages behind [from, from + size) at `to` too. Each page is looked up as it's mapped now. Returns false
// at the first unmapped page. The pages mapped before it stay mapped.
bool Kernel::MapAlias(uint32_t to, uint32_t from, uint32_t size)
{
    for (uint64_t offset = 0; offset < size; offset += kPageSize)
    {
        uint8_t* const host = memory_.HostPtr(from + static_cast<uint32_t>(offset));
        if (!host)
        {
            return false;
        }

        memory_.Map(to + static_cast<uint32_t>(offset), host, kPageSize);
    }

    return true;
}

// FCRAM's page flags, ready for a change. While a snapshot shares them, the state gets a copy first.
std::vector<uint8_t>& Kernel::FcramUsedForChange()
{
    std::shared_ptr<std::vector<uint8_t>>& used = state_.fcram_used;
    if (used.use_count() > 1)
    {
        used = std::make_shared<std::vector<uint8_t>>(*used);
    }

    return *used;
}

// A block of memory to map elsewhere: the memory at an address, or with an address of 0 a new region.
void Kernel::SvcCreateMemoryBlock()
{
    arm::CpuState& r = cpu_.State();
    const uint32_t address = r.r[1];
    const uint32_t size = r.r[2];
    if ((address & kPageMask) != 0)
    {
        r.r[0] = kInvalidAddress;
        return;
    }

    if ((size & kPageMask) != 0)
    {
        r.r[0] = kMisalignedSize;
        return;
    }

    const uint32_t block = NewObject(ObjectKind::kMemoryBlock);
    state_.objects[block].address = address;
    state_.objects[block].size = size;
    if (address == 0)
    {
        const uint32_t storage = NewRegion(size);
        state_.objects[block].storage = storage;
    }

    r.r[0] = kSuccess;
    r.r[1] = NewHandle(block);
}

// Maps a block's pages at the address. A block made at an address takes the pages there as they're mapped now. The
// pages mapped before an unmapped one stay mapped. A block made without an address maps its storage.
void Kernel::SvcMapMemoryBlock()
{
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> block = LookupForSvc(r.r[0], ObjectKind::kMemoryBlock);
    if (!block)
    {
        return;
    }

    const uint32_t address = r.r[1];
    if ((address & kPageMask) != 0)
    {
        r.r[0] = kMisalignedAddress;
        return;
    }

    const Object& object = state_.objects[*block];
    if (object.address != 0)
    {
        r.r[0] = MapAlias(address, object.address, object.size) ? kSuccess : kInvalidAddress;
        return;
    }

    memory_.Map(address, regions_[object.storage].host.get(), object.size);
    r.r[0] = kSuccess;
}

// Nothing checks that the address is page aligned. arm::Memory unmaps from the start of the page that holds it.
void Kernel::SvcUnmapMemoryBlock()
{
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> block = LookupForSvc(r.r[0], ObjectKind::kMemoryBlock);
    if (!block)
    {
        return;
    }

    memory_.Unmap(r.r[1], state_.objects[*block].size);
    r.r[0] = kSuccess;
}

// A new region of zeros, rounded up to a page, mapped nowhere yet.
uint32_t Kernel::NewRegion(uint32_t size)
{
    const uint32_t rounded = arm::Memory::RoundUpToPage(size);
    regions_.push_back({std::make_unique<uint8_t[]>(rounded), rounded});

    return static_cast<uint32_t>(regions_.size() - 1);
}

} // namespace threesf::horizon
