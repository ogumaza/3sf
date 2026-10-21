// SPDX-License-Identifier: MIT

// The Horizon kernel's high-level emulation (see kernel.h).

#include "horizon/kernel.h"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/memory_image.h"

#include "arm/cpu.h"
#include "horizon/ipc.h"

namespace threesf::horizon
{
namespace
{

// An SVC that the kernel implements (3dbrew, "SVC"): its number, the cycles that it adds to the clock and the function
// that runs it.
struct SvcEntry
{
    uint32_t number;
    uint64_t cycles;
    void (Kernel::*run)();
};

// The cycles of an SVC that the kernel doesn't implement.
constexpr uint64_t kUnimplementedSvcCycles = 1000;

// Result codes.
constexpr uint32_t kTimeout = 0x09401bfe;
constexpr uint32_t kInvalidHandle = 0xd8e007f7;
constexpr uint32_t kNotFound = 0xd8e007fa;
constexpr uint32_t kOutOfMemory = 0xd86007f3;
constexpr uint32_t kInvalidAddress = 0xe0e01bf5;
constexpr uint32_t kMisalignedAddress = 0xe0e01bf1;
constexpr uint32_t kMisalignedSize = 0xe0e01bf2;
constexpr uint32_t kOutOfRange = 0xd8e007fd;
constexpr uint32_t kNotOwner = 0xd8e0041f;
constexpr uint32_t kInvalidEnum = 0xd8e093ed;
constexpr uint32_t kNotImplemented = 0xf8601bfd;

// The handle that stands for the thread that calls an SVC, and the step between the handles of the table.
constexpr uint32_t kCurrentThread = 0xffff8000;
constexpr uint32_t kHandleStep = 0x10;

// The address map: the heap, linear memory (page n of FCRAM at kLinearBase + n pages), configuration memory, the
// shared page and the slots of thread-local storage, eight to a page.
constexpr uint32_t kHeapBase = 0x08000000;
constexpr uint32_t kHeapEnd = 0x0e000000;
constexpr uint32_t kLinearBase = 0x14000000;
constexpr uint32_t kConfigMemory = 0x1ff80000;
constexpr uint32_t kSharedPage = 0x1ff81000;
constexpr uint32_t kTlsBase = 0x1ff82000;
constexpr uint32_t kTlsSize = 0x200;
constexpr uint32_t kTlsSlotsPerPage = 8;

// The value that turns a linear address into a physical one (GetProcessInfo's type 20). FCRAM starts at 0x20000000 in
// physical memory.
constexpr uint32_t kLinearToPhysical = 0x20000000 - kLinearBase;

constexpr uint32_t kPageSize = arm::Memory::kPageSize;
constexpr uint32_t kPageMask = arm::Memory::kPageMask;

// A new thread's FPSCR: default NaN, flush to zero, round towards zero.
constexpr uint32_t kThreadFpscr = 0x03c00000;

// ControlMemory's operations, and the flag that selects linear memory.
constexpr uint32_t kOperationFree = 1;
constexpr uint32_t kOperationCommit = 3;
constexpr uint32_t kOperationMap = 4;
constexpr uint32_t kOperationUnmap = 5;
constexpr uint32_t kOperationProtect = 6;
constexpr uint32_t kOperationLinear = 0x10000;

// The reset type of an event or timer that a wait clears, and of one that a signal clears at once.
constexpr uint32_t kOneShot = 0;
constexpr uint32_t kPulse = 2;

// The index that WaitSynchronizationN reports when no single object ends the wait: when the thread gets all its
// objects, and on a timeout. It's -1 as a u32.
constexpr uint32_t kNoWaitIndex = 0xffffffff;

// ArbitrateAddress's types.
constexpr uint32_t kArbitrateSignal = 0;
constexpr uint32_t kArbitrateWaitIfLess = 1;
constexpr uint32_t kArbitrateDecrementAndWaitIfLess = 2;
constexpr uint32_t kArbitrateWaitIfLessTimeout = 3;
constexpr uint32_t kArbitrateDecrementAndWaitIfLessTimeout = 4;

// The process id that GetProcessId returns.
constexpr uint32_t kProcessId = 0x28;

// The most bytes that OutputDebugString reads.
constexpr uint32_t kDebugStringLimit = 4096;

// A log message's buffer: a longer message is cut to its first 1023 bytes.
constexpr std::size_t kLogBuffer = 1024;

// Objects are collected once the table grows to twice what was live after the last collection, and at least this.
constexpr std::size_t kCollectMinimum = 256;

constexpr uint64_t kNsPerSecond = 1'000'000'000;

// A time in nanoseconds from two registers: the s64 with `high` in its upper 32 bits.
int64_t Nanoseconds(uint32_t low, uint32_t high)
{
    return static_cast<int64_t>(uint64_t{high} << 32 | low);
}

// Nanoseconds in ticks, rounded up. A time of 0 or less is 0 ticks.
uint64_t NsToTicks(int64_t ns)
{
    if (ns <= 0)
    {
        return 0;
    }

    const auto n = static_cast<uint64_t>(ns);
    return n / kNsPerSecond * kArmClock + (n % kNsPerSecond * kArmClock + kNsPerSecond - 1) / kNsPerSecond;
}

} // namespace

Kernel::Kernel(const KernelConfig& config) : config_(config), cpu_(memory_)
{
    state_.collect_at = kCollectMinimum;

    // Configuration memory: the values of firmware 11.17, with the application's memory (3dbrew, "Configuration
    // Memory").
    uint8_t* const config_memory = MapNewRegion(kConfigMemory, kPageSize);
    config_memory[0x02] = 0x3a;                          // KERNEL_VERSIONMINOR
    config_memory[0x03] = 0x02;                          // KERNEL_VERSIONMAJOR
    xsflib::StoreLe32(config_memory + 0x08, 0x00008002); // NSTID
    xsflib::StoreLe32(config_memory + 0x0c, 0x00040130);
    xsflib::StoreLe32(config_memory + 0x10, 2);          // SYSCOREVER
    config_memory[0x14] = 1;                             // ENVINFO: retail
    config_memory[0x16] = 1;                             // PREV_FIRM
    xsflib::StoreLe32(config_memory + 0x18, 0x0000f450); // KERNEL_CTRSDKVERSION
    xsflib::StoreLe32(config_memory + 0x30, 0);          // APPMEMTYPE: memory mode 0
    xsflib::StoreLe32(config_memory + 0x40, config.app_memory);
    xsflib::StoreLe32(config_memory + 0x44, 0x02c00000); // SYSMEMALLOC
    xsflib::StoreLe32(config_memory + 0x48, 0x01400000); // BASEMEMALLOC
    config_memory[0x62] = 0x3a;                          // FIRM_VERSIONMINOR
    config_memory[0x63] = 0x02;                          // FIRM_VERSIONMAJOR
    xsflib::StoreLe32(config_memory + 0x64, 2);          // FIRM_SYSCOREVER
    xsflib::StoreLe32(config_memory + 0x68, 0x0000f450); // FIRM_CTRSDKVERSION

    // The shared page: a product, with the battery at charge level 5 and charging. Nothing updates it.
    uint8_t* const shared_page = MapNewRegion(kSharedPage, kPageSize);
    shared_page[0x04] = 1;    // RUNNING_HW
    shared_page[0x85] = 0x17; // the battery: (5 << 2) | 3
    shared_page[0x86] = 1;
}

Kernel::~Kernel() = default;

Kernel::RunResult Kernel::RunUntil(uint64_t until)
{
    State& s = state_;
    while (true)
    {
        if (s.exited)
        {
            return error_.empty() ? RunResult::kExited : RunResult::kError;
        }

        RunDueEvents();
        if (s.ticks >= until)
        {
            return RunResult::kOk;
        }

        // With no thread ready, the time moves on towards the next timed event in slices, and the DSP runs.
        const uint32_t next = PickThread();
        if (next == kNone)
        {
            uint64_t target = std::min(until, s.ticks + config_.slice_cycles);
            if (!s.events.empty())
            {
                target = std::min(target, s.events.front().due);
            }

            AdvanceTo(target);
            continue;
        }

        // The CPU runs until the slice ends, the next timed event is due or the thread stops. The SVCs' cycles reach
        // the clock with the instructions'.
        SwitchTo(next);
        uint64_t limit = std::min(until, s.ticks + config_.slice_cycles);
        if (!s.events.empty())
        {
            limit = std::min(limit, std::max(s.events.front().due, s.ticks + 1));
        }

        const uint64_t executed = cpu_.executed_;
        const arm::StopReason reason = cpu_.Run(limit - s.ticks);
        const uint64_t cost = cpu_.executed_ - executed + s.pending;
        s.pending = 0;
        AdvanceTo(s.ticks + cost);

        switch (reason)
        {
        case arm::StopReason::kSvc:
            HandleSvc();
            break;

        case arm::StopReason::kYield:
            Yield(s.current);
            break;

        case arm::StopReason::kFault:
            error_ = FaultMessage();
            memory_.fault_ = false;
            return RunResult::kError;

        case arm::StopReason::kUndefined:
            error_ = UndefinedMessage();
            return RunResult::kError;

        case arm::StopReason::kNone:
        case arm::StopReason::kBudget:
            break;
        }

        if (s.current != kNone && s.threads[s.current].state != ThreadState::kReady)
        {
            s.threads[s.current].registers = cpu_.state_;
            s.current = kNone;
        }
    }
}

uint64_t Kernel::Ticks() const
{
    return state_.ticks;
}

arm::Memory& Kernel::Memory()
{
    return memory_;
}

uint32_t Kernel::Read32(uint32_t address)
{
    return memory_.Read32(address);
}

void Kernel::Write32(uint32_t address, uint32_t value)
{
    memory_.Write32(address, value);
}

void Kernel::SetLinearMemory(uint8_t* fcram, uint32_t size)
{
    fcram_ = fcram;
    fcram_size_ = size;
    state_.fcram_used = std::make_shared<std::vector<uint8_t>>(size / kPageSize, 0);
}

uint8_t* Kernel::MapNewRegion(uint32_t address, uint32_t size)
{
    const Region& region = regions_[NewRegion(size)];
    memory_.Map(address, region.host.get(), region.size);

    return region.host.get();
}

void Kernel::SetServiceHost(ServiceHost* host)
{
    host_ = host;
}

void Kernel::SetDeviceStep(std::function<void(uint64_t ticks)> step)
{
    device_step_ = std::move(step);
}

uint32_t Kernel::CreateThread(int32_t priority, uint32_t entry, uint32_t argument, uint32_t stack_top)
{
    State& s = state_;
    Thread thread;
    thread.id = s.next_thread_id++;
    thread.priority = priority;
    thread.registers.r[0] = argument;
    thread.registers.r[13] = stack_top & ~7u;
    thread.registers.r[15] = entry & ~1u;
    thread.registers.thumb = (entry & 1) != 0;
    thread.registers.fpscr = kThreadFpscr;
    thread.tls = AllocateTls();
    thread.registers.tls = thread.tls;
    thread.ready_number = ++s.ready_counter;

    const auto index = static_cast<uint32_t>(s.threads.size());
    s.threads.push_back(std::move(thread));
    s.ready_threads++;
    const uint32_t object = NewObject(ObjectKind::kThread);
    s.objects[object].thread = index;
    s.threads[index].object = object;

    return NewHandle(object);
}

uint32_t Kernel::NewEvent(uint32_t reset_type)
{
    const uint32_t object = NewObject(ObjectKind::kEvent);
    state_.objects[object].reset_type = reset_type;

    return object;
}

uint32_t Kernel::NewMutex()
{
    return NewObject(ObjectKind::kMutex);
}

uint32_t Kernel::NewSemaphore(int32_t count, int32_t maximum)
{
    const uint32_t object = NewObject(ObjectKind::kSemaphore);
    state_.objects[object].count = count;
    state_.objects[object].maximum = maximum;

    return object;
}

uint32_t Kernel::NewSession(uint32_t service)
{
    const uint32_t object = NewObject(ObjectKind::kSession);
    state_.objects[object].service = service;

    return object;
}

uint32_t Kernel::NewHandle(uint32_t object)
{
    State& s = state_;
    const uint32_t handle = s.next_handle;
    s.next_handle += kHandleStep;
    s.handles.push_back({handle, object});

    return handle;
}

uint32_t Kernel::Lookup(uint32_t handle) const
{
    const State& s = state_;
    if (handle == kCurrentThread)
    {
        return s.current == kNone ? kNone : s.threads[s.current].object;
    }

    const auto entry = FindHandle(handle);
    return entry != s.handles.end() ? entry->object : kNone;
}

bool Kernel::IsEvent(uint32_t object) const
{
    return object != kNone && state_.objects[object].kind == ObjectKind::kEvent;
}

void Kernel::SetSignalHook(uint32_t event)
{
    state_.objects[event].hooked = true;
}

void Kernel::Signal(uint32_t object)
{
    state_.objects[object].signalled = true;
    WakeWaiters(object);
    if (state_.objects[object].reset_type == kPulse)
    {
        state_.objects[object].signalled = false;
    }
}

void Kernel::AddPending(uint64_t cycles)
{
    state_.pending += cycles;
}

uint32_t Kernel::CurrentThreadId() const
{
    return state_.current == kNone ? 0 : state_.threads[state_.current].id;
}

uint32_t Kernel::CurrentTls() const
{
    return state_.current == kNone ? 0 : state_.threads[state_.current].tls;
}

void Kernel::Log(const char* format, ...)
{
    char message[kLogBuffer];
    message[0] = 0;
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    if (log_)
    {
        log_(message);
    }
    else
    {
        std::fprintf(stderr, "%s\n", message);
    }
}

Kernel::Snapshot Kernel::Save(const Snapshot* previous) const
{
    Snapshot snapshot;
    snapshot.state = state_;
    snapshot.error = error_;
    snapshot.regions.reserve(regions_.size());
    for (std::size_t i = 0; i < regions_.size(); i++)
    {
        const xsflib::MemoryImage* before = previous && i < previous->regions.size() ? &previous->regions[i] : nullptr;
        snapshot.regions.emplace_back(regions_[i].host.get(), regions_[i].size, before);
    }

    snapshot.memory = memory_.Save(previous ? &previous->memory : nullptr);
    snapshot.cpu = cpu_.Save();

    return snapshot;
}

void Kernel::Restore(const Snapshot& snapshot)
{
    // Regions made after the snapshot stay. Nothing in the restored state refers to them.
    state_ = snapshot.state;
    error_ = snapshot.error;
    for (std::size_t i = 0; i < snapshot.regions.size(); i++)
    {
        snapshot.regions[i].Restore(regions_[i].host.get());
    }

    memory_.Restore(snapshot.memory);
    cpu_.Restore(snapshot.cpu);
}

// An event runs after another when it's due later. Events due at the same time run in the order they were scheduled.
bool Kernel::TimedEvent::RunsAfter(const TimedEvent& a, const TimedEvent& b)
{
    return a.due != b.due ? a.due > b.due : a.order > b.order;
}

// Runs the timed events that are due, the earliest first. One that an event schedules for a time already reached runs
// in the same pass.
void Kernel::RunDueEvents()
{
    State& s = state_;
    while (!s.events.empty() && s.events.front().due <= s.ticks)
    {
        std::pop_heap(s.events.begin(), s.events.end(), TimedEvent::RunsAfter);
        const TimedEvent event = s.events.back();
        s.events.pop_back();
        switch (event.kind)
        {
        case TimedEvent::Kind::kTimeout:
            ExpireTimeout(event.target, event.generation);
            break;
        case TimedEvent::Kind::kTimerExpiry:
            ExpireTimer(event.target, event.generation);
            break;
        }
    }
}

// A thread's timeout ends its wait or sleep. A wait for objects ends with the timeout result. A wait on an arbiter and
// a sleep end with the registers as they were. A wake since the timeout started makes it stale.
void Kernel::ExpireTimeout(uint32_t thread, uint64_t generation)
{
    if (state_.threads[thread].wait_generation != generation)
    {
        return;
    }

    switch (state_.threads[thread].state)
    {
    case ThreadState::kWaitObjects:
        EndWaitForObjects(thread, kTimeout, kNoWaitIndex);
        break;
    case ThreadState::kWaitArbiter:
    case ThreadState::kSleeping:
        Resume(thread);
        break;
    case ThreadState::kReady:
    case ThreadState::kDead:
        break;
    }
}

// A timer's expiry signals the timer. A periodic timer's next expiry counts from now. A set or cancel since the expiry
// was scheduled makes it stale.
void Kernel::ExpireTimer(uint32_t timer, uint64_t generation)
{
    if (state_.objects[timer].generation != generation)
    {
        return;
    }

    Signal(timer);
    const uint64_t interval = state_.objects[timer].interval;
    if (interval != 0)
    {
        ScheduleExpiry(timer, state_.ticks + interval);
    }
}

// The ready thread to run next: the lowest priority number, then the current thread unless it has just yielded, then
// the thread that became ready first.
uint32_t Kernel::PickThread()
{
    State& s = state_;
    const bool yielded = s.just_yielded;
    s.just_yielded = false;
    if (s.ready_threads == 0)
    {
        return kNone;
    }

    uint32_t best = kNone;
    for (uint32_t i = 0; i < s.threads.size(); i++)
    {
        const Thread& thread = s.threads[i];
        if (thread.state == ThreadState::kReady && (best == kNone || thread.priority < s.threads[best].priority))
        {
            best = i;
        }
    }

    const int32_t priority = s.threads[best].priority;
    if (!yielded && s.current != kNone && s.threads[s.current].state == ThreadState::kReady &&
        s.threads[s.current].priority == priority)
    {
        return s.current;
    }

    for (uint32_t i = best + 1; i < s.threads.size(); i++)
    {
        const Thread& thread = s.threads[i];
        if (thread.state == ThreadState::kReady && thread.priority == priority &&
            thread.ready_number < s.threads[best].ready_number)
        {
            best = i;
        }
    }

    return best;
}

void Kernel::SwitchTo(uint32_t thread)
{
    State& s = state_;
    if (thread == s.current)
    {
        return;
    }

    if (s.current != kNone)
    {
        s.threads[s.current].registers = cpu_.state_;
    }

    cpu_.state_ = s.threads[thread].registers;
    s.current = thread;
    cpu_.ClearExclusive();
}

// Moves the clock on to `ticks`, if that's later, and steps the devices to it.
void Kernel::AdvanceTo(uint64_t ticks)
{
    if (ticks > state_.ticks)
    {
        state_.ticks = ticks;
        if (device_step_)
        {
            device_step_(ticks);
        }
    }
}

std::string Kernel::FaultMessage() const
{
    char message[96];
    std::snprintf(message, sizeof(message), "memory fault at %08x (%s) in thread %u at pc %08x", memory_.fault_address_,
                  memory_.fault_write_ ? "write" : "read", CurrentThreadId(), cpu_.stop_pc_);

    return std::string(message) + "\n" + cpu_.Describe();
}

std::string Kernel::UndefinedMessage() const
{
    char message[96];
    std::snprintf(message, sizeof(message), "undefined instruction %08x in thread %u at pc %08x", cpu_.stop_opcode_,
                  CurrentThreadId(), cpu_.stop_pc_);

    return std::string(message) + "\n" + cpu_.Describe();
}

// Adds the SVC's cycles to the pending ones and runs the SVC. One that the kernel doesn't implement fails.
void Kernel::HandleSvc()
{
    // Every SVC that the kernel implements, in the order of their numbers.
    static constexpr SvcEntry kSvcs[] = {
        {0x01, 1000, &Kernel::SvcControlMemory},
        {0x03, 1000, &Kernel::SvcExitProcess},
        {0x08, 5214, &Kernel::SvcCreateThread},
        {0x09, 1000, &Kernel::SvcExitThread},
        {0x0a, 946, &Kernel::SvcSleepThread},
        {0x0b, 616, &Kernel::SvcGetThreadPriority},
        {0x0c, 1812, &Kernel::SvcSetThreadPriority},
        {0x13, 1000, &Kernel::SvcCreateMutex},
        {0x14, 1324, &Kernel::SvcReleaseMutex},
        {0x15, 1000, &Kernel::SvcCreateSemaphore},
        {0x16, 2713, &Kernel::SvcReleaseSemaphore},
        {0x17, 4329, &Kernel::SvcCreateEvent},
        {0x18, 3285, &Kernel::SvcSignalEvent},
        {0x19, 1389, &Kernel::SvcClearEvent},
        {0x1a, 1000, &Kernel::SvcCreateTimer},
        {0x1b, 5163, &Kernel::SvcSetTimer},
        {0x1c, 1000, &Kernel::SvcCancelTimer},
        {0x1d, 1000, &Kernel::SvcClearTimer},
        {0x1e, 1000, &Kernel::SvcCreateMemoryBlock},
        {0x1f, 1000, &Kernel::SvcMapMemoryBlock},
        {0x20, 1000, &Kernel::SvcUnmapMemoryBlock},
        {0x21, 1000, &Kernel::SvcCreateAddressArbiter},
        {0x22, 5664, &Kernel::SvcArbitrateAddress},
        {0x23, 2937, &Kernel::SvcCloseHandle},
        {0x24, 4005, &Kernel::SvcWaitSynchronization1},
        {0x25, 6918, &Kernel::SvcWaitSynchronizationN},
        {0x27, 1000, &Kernel::SvcDuplicateHandle},
        {0x28, 340, &Kernel::SvcGetSystemTick},
        {0x29, 1000, &Kernel::SvcGetHandleInfo},
        {0x2a, 1000, &Kernel::SvcGetSystemInfo},
        {0x2b, 1000, &Kernel::SvcGetProcessInfo},
        {0x2d, 1000, &Kernel::SvcConnectToPort},
        {0x32, 5825, &Kernel::SvcSendSyncRequest},
        {0x35, 1000, &Kernel::SvcGetProcessId},
        {0x37, 677, &Kernel::SvcGetThreadId},
        {0x38, 1000, &Kernel::SvcGetResourceLimit},
        {0x39, 1000, &Kernel::SvcGetResourceLimitLimitValues},
        {0x3a, 1000, &Kernel::SvcGetResourceLimitCurrentValues},
        {0x3c, 1000, &Kernel::SvcBreak},
        {0x3d, 1000, &Kernel::SvcOutputDebugString},
    };

    const uint32_t number = cpu_.svc_number_;
    const auto svc =
        std::find_if(std::begin(kSvcs), std::end(kSvcs), [number](const SvcEntry& s) { return s.number == number; });
    if (svc == std::end(kSvcs))
    {
        state_.pending += kUnimplementedSvcCycles;
        Log("unimplemented SVC 0x%02x at %08x", number, cpu_.stop_pc_);
        cpu_.state_.r[0] = kNotImplemented;
        return;
    }

    state_.pending += svc->cycles;
    (this->*svc->run)();
}

void Kernel::SvcControlMemory()
{
    arm::CpuState& r = cpu_.state_;
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

void Kernel::SvcCreateThread()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t handle = CreateThread(static_cast<int32_t>(r.r[0]), r.r[1], r.r[2], r.r[3]);
    r.r[0] = kSuccess;
    r.r[1] = handle;
}

void Kernel::SvcSleepThread()
{
    const arm::CpuState& r = cpu_.state_;
    const int64_t ns = Nanoseconds(r.r[0], r.r[1]);
    if (ns <= 0)
    {
        Yield(state_.current);
        return;
    }

    Sleep(state_.current, ns);
}

// The thread dies. The kernel releases each mutex that the thread holds and that a handle refers to, in the order of
// the handle table, and wakes the mutex's waiters. The thread's waiters then wake. The slot of thread-local storage
// goes back to the free list.
void Kernel::SvcExitThread()
{
    State& s = state_;
    const uint32_t thread = s.current;
    SetThreadState(thread, ThreadState::kDead);
    for (std::size_t i = 0; i < s.handles.size(); i++)
    {
        Object& object = s.objects[s.handles[i].object];
        if (object.kind == ObjectKind::kMutex && object.holder == thread)
        {
            object.holder = kNone;
            object.lock_count = 0;
            WakeWaiters(s.handles[i].object);
        }
    }

    WakeWaiters(s.threads[thread].object);
    s.free_tls.push_back(s.threads[thread].tls);
}

// The process exits and the calling thread dies. Nothing else happens.
void Kernel::SvcExitProcess()
{
    state_.exited = true;
    SetThreadState(state_.current, ThreadState::kDead);
}

void Kernel::SvcGetThreadPriority()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t object = LookupKind(r.r[1], ObjectKind::kThread);
    if (object == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    r.r[0] = kSuccess;
    r.r[1] = static_cast<uint32_t>(state_.threads[state_.objects[object].thread].priority);
}

// Nothing checks the priority. It counts from the next pick.
void Kernel::SvcSetThreadPriority()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t object = LookupKind(r.r[0], ObjectKind::kThread);
    if (object == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    state_.threads[state_.objects[object].thread].priority = static_cast<int32_t>(r.r[1]);
    r.r[0] = kSuccess;
}

// A mutex that the calling thread holds from the start when r1 isn't 0.
void Kernel::SvcCreateMutex()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t mutex = NewMutex();
    if (r.r[1] != 0)
    {
        Acquire(mutex, state_.current);
    }

    r.r[0] = kSuccess;
    r.r[1] = NewHandle(mutex);
}

void Kernel::SvcReleaseMutex()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t mutex = LookupKind(r.r[0], ObjectKind::kMutex);
    if (mutex == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    Object& object = state_.objects[mutex];
    if (object.holder != state_.current)
    {
        r.r[0] = kNotOwner;
        return;
    }

    if (--object.lock_count == 0)
    {
        object.holder = kNone;
        WakeWaiters(mutex);
    }

    r.r[0] = kSuccess;
}

void Kernel::SvcCreateSemaphore()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t semaphore = NewSemaphore(static_cast<int32_t>(r.r[1]), static_cast<int32_t>(r.r[2]));
    r.r[0] = kSuccess;
    r.r[1] = NewHandle(semaphore);
}

void Kernel::SvcReleaseSemaphore()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t semaphore = LookupKind(r.r[1], ObjectKind::kSemaphore);
    if (semaphore == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    Object& object = state_.objects[semaphore];
    const auto release = static_cast<int32_t>(r.r[2]);
    if (release < 0 || int64_t{object.count} + release > object.maximum)
    {
        r.r[0] = kOutOfRange;
        return;
    }

    r.r[1] = static_cast<uint32_t>(object.count);
    object.count += release;
    WakeWaiters(semaphore);
    r.r[0] = kSuccess;
}

void Kernel::SvcCreateEvent()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t event = NewEvent(r.r[1]);
    r.r[0] = kSuccess;
    r.r[1] = NewHandle(event);
}

// A hooked event runs the services' hook before the signal.
void Kernel::SvcSignalEvent()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t event = LookupKind(r.r[0], ObjectKind::kEvent);
    if (event == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    if (state_.objects[event].hooked && host_)
    {
        host_->RunSignalHook(event);
    }

    Signal(event);
    r.r[0] = kSuccess;
}

void Kernel::SvcClearEvent()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t event = LookupKind(r.r[0], ObjectKind::kEvent);
    if (event == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    state_.objects[event].signalled = false;
    r.r[0] = kSuccess;
}

void Kernel::SvcCreateTimer()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t timer = NewObject(ObjectKind::kTimer);
    state_.objects[timer].reset_type = r.r[1];
    r.r[0] = kSuccess;
    r.r[1] = NewHandle(timer);
}

// Sets a timer to expire after the initial time, and then after each interval. The expiries scheduled before become
// stale.
void Kernel::SvcSetTimer()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t timer = LookupKind(r.r[0], ObjectKind::kTimer);
    if (timer == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    Object& object = state_.objects[timer];
    object.generation++;
    object.interval = NsToTicks(Nanoseconds(r.r[1], r.r[4]));
    ScheduleExpiry(timer, state_.ticks + NsToTicks(Nanoseconds(r.r[2], r.r[3])));
    r.r[0] = kSuccess;
}

// The expiries scheduled before become stale. The signal stays.
void Kernel::SvcCancelTimer()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t timer = LookupKind(r.r[0], ObjectKind::kTimer);
    if (timer == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    state_.objects[timer].generation++;
    r.r[0] = kSuccess;
}

void Kernel::SvcClearTimer()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t timer = LookupKind(r.r[0], ObjectKind::kTimer);
    if (timer == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    state_.objects[timer].signalled = false;
    r.r[0] = kSuccess;
}

// A block of memory to map elsewhere: the memory at an address, or with an address of 0 a new region.
void Kernel::SvcCreateMemoryBlock()
{
    arm::CpuState& r = cpu_.state_;
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
    arm::CpuState& r = cpu_.state_;
    const uint32_t block = LookupKind(r.r[0], ObjectKind::kMemoryBlock);
    if (block == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    const uint32_t address = r.r[1];
    if ((address & kPageMask) != 0)
    {
        r.r[0] = kMisalignedAddress;
        return;
    }

    const Object& object = state_.objects[block];
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
    arm::CpuState& r = cpu_.state_;
    const uint32_t block = LookupKind(r.r[0], ObjectKind::kMemoryBlock);
    if (block == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    memory_.Unmap(r.r[1], state_.objects[block].size);
    r.r[0] = kSuccess;
}

void Kernel::SvcCreateAddressArbiter()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t arbiter = NewObject(ObjectKind::kArbiter);
    r.r[0] = kSuccess;
    r.r[1] = NewHandle(arbiter);
}

// Signals the threads that wait on an arbiter at an address or makes the calling thread wait there.
void Kernel::SvcArbitrateAddress()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t arbiter = LookupKind(r.r[0], ObjectKind::kArbiter);
    if (arbiter == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    const uint32_t address = r.r[1];
    const uint32_t type = r.r[2];
    const auto value = static_cast<int32_t>(r.r[3]);
    switch (type)
    {
    case kArbitrateSignal:
        SignalArbiter(arbiter, address, value);
        r.r[0] = kSuccess;
        break;

    case kArbitrateWaitIfLess:
    case kArbitrateDecrementAndWaitIfLess:
    case kArbitrateWaitIfLessTimeout:
    case kArbitrateDecrementAndWaitIfLessTimeout:
        r.r[0] = WaitOnArbiter(arbiter, address, type, value, Nanoseconds(r.r[4], r.r[5]));
        break;

    default:
        r.r[0] = kInvalidEnum;
        break;
    }
}

// Wakes up to `value` threads that wait on the arbiter at the address, or all of them for a negative value: the lowest
// priority number first, then the thread made first.
void Kernel::SignalArbiter(uint32_t arbiter, uint32_t address, int32_t value)
{
    State& s = state_;
    for (int64_t woken = 0; value < 0 || woken < value; woken++)
    {
        uint32_t best = kNone;
        for (uint32_t i = 0; i < s.threads.size(); i++)
        {
            const Thread& thread = s.threads[i];
            if (thread.state == ThreadState::kWaitArbiter && thread.arbiter == arbiter &&
                thread.arbiter_address == address && (best == kNone || thread.priority < s.threads[best].priority))
            {
                best = i;
            }
        }

        if (best == kNone)
        {
            break;
        }

        Resume(best);
    }
}

// The calling thread waits on the arbiter at the address if the word there is less than the value. The decrementing
// types take 1 from the word only when the thread waits. The types with a timeout start the timeout even for a time of
// 0 or less. They return the timeout result in every case. Returns the result.
uint32_t Kernel::WaitOnArbiter(uint32_t arbiter, uint32_t address, uint32_t type, int32_t value, int64_t ns)
{
    State& s = state_;
    const bool timed = type == kArbitrateWaitIfLessTimeout || type == kArbitrateDecrementAndWaitIfLessTimeout;
    const bool decrement = type == kArbitrateDecrementAndWaitIfLess || type == kArbitrateDecrementAndWaitIfLessTimeout;
    const uint32_t result = timed ? kTimeout : kSuccess;
    const uint32_t word = memory_.Read32(address);
    if (static_cast<int32_t>(word) >= value)
    {
        return result;
    }

    if (decrement)
    {
        memory_.Write32(address, word - 1);
    }

    Thread& thread = s.threads[s.current];
    thread.arbiter = arbiter;
    thread.arbiter_address = address;
    SetThreadState(s.current, ThreadState::kWaitArbiter);
    if (timed)
    {
        StartTimeout(s.current, ns);
    }

    return result;
}

void Kernel::SvcCloseHandle()
{
    arm::CpuState& r = cpu_.state_;
    const auto entry = FindHandle(r.r[0]);
    if (entry == state_.handles.end())
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    state_.handles.erase(entry);
    r.r[0] = kSuccess;
}

void Kernel::SvcWaitSynchronization1()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t object = LookupWaitable(r.r[0]);
    if (object == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    const uint32_t thread = state_.current;
    if (!MustWait(object, thread))
    {
        Acquire(object, thread);
        r.r[0] = kSuccess;
        return;
    }

    // The result stays the timeout result until a wake replaces it.
    r.r[0] = kTimeout;
    const int64_t ns = Nanoseconds(r.r[2], r.r[3]);
    if (ns != 0)
    {
        WaitForObjects(thread, {object}, false, ns, false);
    }
}

// The calling thread waits for the objects of the r2 handles at r1. When r3 isn't 0, the thread waits for all of them.
// Otherwise any one of them ends the wait. r1 gets the index that the wait reports.
void Kernel::SvcWaitSynchronizationN()
{
    arm::CpuState& r = cpu_.state_;
    const bool all = r.r[3] != 0;
    const int64_t ns = Nanoseconds(r.r[0], r.r[4]);
    std::vector<uint32_t> objects;
    if (!ReadWaitObjects(r.r[1], r.r[2], objects))
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    // The objects that the thread gets at once end the wait before it starts.
    const uint32_t thread = state_.current;
    const std::optional<uint32_t> index = all ? TryAcquireAll(objects, thread) : TryAcquireAny(objects, thread);
    if (index)
    {
        r.r[0] = kSuccess;
        r.r[1] = *index;
        return;
    }

    // The result stays the timeout result until a wake replaces it. A wait on no handles sleeps for the time. A
    // negative time gives that sleep a timeout of 0.
    r.r[0] = kTimeout;
    r.r[1] = kNoWaitIndex;
    if (ns == 0)
    {
        return;
    }

    if (objects.empty())
    {
        Sleep(thread, ns);
        return;
    }

    WaitForObjects(thread, std::move(objects), all, ns, true);
}

// Reads a word for each of `count` handles at `address`. Appends each handle's object to `objects`. Returns false at
// the first handle that isn't a waitable object.
bool Kernel::ReadWaitObjects(uint32_t address, uint32_t count, std::vector<uint32_t>& objects)
{
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t object = LookupWaitable(memory_.Read32(address + 4 * i));
        if (object == kNone)
        {
            return false;
        }

        objects.push_back(object);
    }

    return true;
}

void Kernel::SvcDuplicateHandle()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t object = Lookup(r.r[1]);
    if (object == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    r.r[0] = kSuccess;
    r.r[1] = NewHandle(object);
}

// The time, with the cycles of the SVCs since the last CPU run, this one's included.
void Kernel::SvcGetSystemTick()
{
    arm::CpuState& r = cpu_.state_;
    const uint64_t ticks = state_.ticks + state_.pending;
    r.r[0] = static_cast<uint32_t>(ticks);
    r.r[1] = static_cast<uint32_t>(ticks >> 32);
}

// Every handle gives 0 for every type.
void Kernel::SvcGetHandleInfo()
{
    arm::CpuState& r = cpu_.state_;
    r.r[0] = kSuccess;
    r.r[1] = 0;
    r.r[2] = 0;
}

void Kernel::SvcGetSystemInfo()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t type = r.r[1];
    const uint32_t parameter = r.r[2];
    const uint64_t value = type == 0 && parameter == 1 ? state_.heap_size + state_.linear_size : 0;
    r.r[0] = kSuccess;
    r.r[1] = static_cast<uint32_t>(value);
    r.r[2] = static_cast<uint32_t>(value >> 32);
}

void Kernel::SvcGetProcessInfo()
{
    arm::CpuState& r = cpu_.state_;
    uint64_t value = 0;
    switch (r.r[2])
    {
    case 0:
        value = state_.heap_size + state_.linear_size;
        break;
    case 2:
    case 6:
        value = state_.linear_size;
        break;
    case 20:
        value = kLinearToPhysical;
        break;
    default:
        break;
    }

    r.r[0] = kSuccess;
    r.r[1] = static_cast<uint32_t>(value);
    r.r[2] = static_cast<uint32_t>(value >> 32);
}

void Kernel::SvcConnectToPort()
{
    arm::CpuState& r = cpu_.state_;
    const std::string name = memory_.ReadCString(r.r[1], 12);
    const uint32_t service = host_ ? host_->PortService(name) : kNone;
    if (service == kNone)
    {
        Log("ConnectToPort: unknown port '%s'", name.c_str());
        r.r[0] = kNotFound;
        return;
    }

    r.r[0] = kSuccess;
    r.r[1] = NewHandle(NewSession(service));
}

// The session's service handles the request at once, in the calling thread's command buffer.
void Kernel::SvcSendSyncRequest()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t session = LookupKind(r.r[0], ObjectKind::kSession);
    if (session == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    const uint32_t service = state_.objects[session].service;
    if (host_)
    {
        host_->HandleRequest(service, CurrentTls() + ipc::kCommandBuffer);
    }

    r.r[0] = kSuccess;
}

void Kernel::SvcGetProcessId()
{
    arm::CpuState& r = cpu_.state_;
    r.r[0] = kSuccess;
    r.r[1] = kProcessId;
}

void Kernel::SvcGetThreadId()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t object = LookupKind(r.r[1], ObjectKind::kThread);
    if (object == kNone)
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    r.r[0] = kSuccess;
    r.r[1] = state_.threads[state_.objects[object].thread].id;
}

// Each call makes a new resource limit object.
void Kernel::SvcGetResourceLimit()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t limit = NewObject(ObjectKind::kResourceLimit);
    r.r[0] = kSuccess;
    r.r[1] = NewHandle(limit);
}

void Kernel::SvcGetResourceLimitLimitValues()
{
    WriteResourceFigures(&ResourceFigures::limit);
}

void Kernel::SvcGetResourceLimitCurrentValues()
{
    WriteResourceFigures(&ResourceFigures::current);
}

// The figures of the resource called `name`. Names above 8 give 0 for both. Name 9 is CPU time.
Kernel::ResourceFigures Kernel::Resource(uint32_t name) const
{
    const State& s = state_;
    switch (name)
    {
    case 0: // priority
        return {0x18, 0x18};
    case 1: // commit
        return {config_.app_memory, s.heap_size + s.linear_size + config_.program_end};
    case 2: // threads
        return {0x20, s.threads.size()};
    case 3: // events
    case 4: // mutexes
        return {0x20, 0};
    case 5: // semaphores
    case 6: // timers
        return {8, 0};
    case 7: // shared memory
        return {0x10, 0};
    case 8: // address arbiters
        return {2, 0};
    default:
        return {};
    }
}

// For each name at r2, writes one figure of its resource at r0 as a u64. `figure` picks the limit or the amount in
// use. The writes stop at the count in r3 or after the first access that faults.
void Kernel::WriteResourceFigures(uint64_t ResourceFigures::* figure)
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t values = r.r[0];
    const uint32_t names = r.r[2];
    const uint32_t count = r.r[3];
    for (uint32_t i = 0; i < count && !memory_.fault_; i++)
    {
        const uint64_t value = Resource(memory_.Read32(names + 4 * i)).*figure;
        memory_.Write32(values + 8 * i, static_cast<uint32_t>(value));
        memory_.Write32(values + 8 * i + 4, static_cast<uint32_t>(value >> 32));
    }

    r.r[0] = kSuccess;
}

// The error gives the reason and the caller. The process then exits as for ExitProcess.
void Kernel::SvcBreak()
{
    const arm::CpuState& r = cpu_.state_;
    char message[64];
    std::snprintf(message, sizeof(message), "svcBreak(%u) called from %08x", r.r[0], r.r[14]);
    error_ = message;
    Log("%s", message);
    SvcExitProcess();
}

void Kernel::SvcOutputDebugString()
{
    arm::CpuState& r = cpu_.state_;
    const uint32_t length = std::min(r.r[1], kDebugStringLimit);
    std::vector<char> text(length + 1, 0);
    memory_.ReadBlock(r.r[0], text.data(), length);
    Log("[debug] %s", text.data());
    r.r[0] = kSuccess;
}

void Kernel::SetThreadState(uint32_t thread, ThreadState state)
{
    State& s = state_;
    const bool was_ready = s.threads[thread].state == ThreadState::kReady;
    const bool ready = state == ThreadState::kReady;
    if (was_ready != ready)
    {
        if (ready)
        {
            s.ready_threads++;
        }
        else
        {
            s.ready_threads--;
        }
    }

    s.threads[thread].state = state;
}

void Kernel::MakeReady(uint32_t thread)
{
    SetThreadState(thread, ThreadState::kReady);
    state_.threads[thread].ready_number = ++state_.ready_counter;
}

// The thread goes behind the other ready threads of its priority. The current thread then doesn't come first in the
// next pick.
void Kernel::Yield(uint32_t thread)
{
    state_.threads[thread].ready_number = ++state_.ready_counter;
    state_.just_yielded = true;
}

// The thread sleeps until a timeout of `ns` nanoseconds wakes it. A time of 0 or less wakes it at the next run of due
// events.
void Kernel::Sleep(uint32_t thread, int64_t ns)
{
    SetThreadState(thread, ThreadState::kSleeping);
    StartTimeout(thread, ns);
}

// Ends a thread's wait or sleep. The thread becomes ready with its registers as they are. A timeout started before then
// becomes stale.
void Kernel::Resume(uint32_t thread)
{
    Thread& t = state_.threads[thread];
    t.wait_generation++;
    t.arbiter = kNone;
    MakeReady(thread);
}

// The thread waits for the objects, joining each one's waiter list once for each place it has in the list. A
// negative time means no limit.
void Kernel::WaitForObjects(uint32_t thread, std::vector<uint32_t> objects, bool all, int64_t ns, bool reports_index)
{
    State& s = state_;
    Thread& t = s.threads[thread];
    t.wait_objects = std::move(objects);
    t.wait_all = all;
    t.wait_reports_index = reports_index;
    SetThreadState(thread, ThreadState::kWaitObjects);
    for (const uint32_t object : t.wait_objects)
    {
        s.objects[object].waiters.push_back(thread);
    }

    if (ns >= 0)
    {
        StartTimeout(thread, ns);
    }
}

// Ends a thread's wait for objects. The thread leaves the objects' waiter lists and gets the result in r0. A wait that
// reports an index gives it in r1.
void Kernel::EndWaitForObjects(uint32_t thread, uint32_t result, uint32_t index)
{
    LeaveWaiterLists(thread);
    arm::CpuState& registers = RegistersOf(thread);
    registers.r[0] = result;
    if (state_.threads[thread].wait_reports_index)
    {
        registers.r[1] = index;
    }

    Resume(thread);
}

void Kernel::LeaveWaiterLists(uint32_t thread)
{
    State& s = state_;
    Thread& t = s.threads[thread];
    for (const uint32_t object : t.wait_objects)
    {
        std::vector<uint32_t>& waiters = s.objects[object].waiters;
        waiters.erase(std::remove(waiters.begin(), waiters.end(), thread), waiters.end());
    }

    t.wait_objects.clear();
}

// A timeout counts from the time without the pending cycles. A wake makes it stale.
void Kernel::StartTimeout(uint32_t thread, int64_t ns)
{
    Schedule(state_.ticks + NsToTicks(ns), TimedEvent::Kind::kTimeout, thread, state_.threads[thread].wait_generation);
}

// A set or cancel of the timer makes the expiry stale.
void Kernel::ScheduleExpiry(uint32_t timer, uint64_t due)
{
    Schedule(due, TimedEvent::Kind::kTimerExpiry, timer, state_.objects[timer].generation);
}

void Kernel::Schedule(uint64_t due, TimedEvent::Kind kind, uint32_t target, uint64_t generation)
{
    State& s = state_;
    s.events.push_back({due, s.next_event_order++, kind, target, generation});
    std::push_heap(s.events.begin(), s.events.end(), TimedEvent::RunsAfter);
}

// Wakes the waiters of an object for as long as one can have it: the lowest priority number first, then the one that
// started waiting first. A thread that waits for all its objects wakes once none of them requires a wait. The thread
// then acquires them all.
void Kernel::WakeWaiters(uint32_t object)
{
    State& s = state_;
    while (true)
    {
        uint32_t best = kNone;
        for (const uint32_t waiter : s.objects[object].waiters)
        {
            const Thread& thread = s.threads[waiter];
            if (thread.state != ThreadState::kWaitObjects ||
                (best != kNone && thread.priority >= s.threads[best].priority))
            {
                continue;
            }

            const bool can_wake =
                thread.wait_all ? CanAcquireAll(thread.wait_objects, waiter) : !MustWait(object, waiter);
            if (can_wake)
            {
                best = waiter;
            }
        }

        if (best == kNone)
        {
            return;
        }

        // The index is the object's last place in the thread's list.
        Thread& thread = s.threads[best];
        uint32_t index = kNoWaitIndex;
        if (thread.wait_all)
        {
            AcquireAll(thread.wait_objects, best);
        }
        else
        {
            Acquire(object, best);
            for (uint32_t i = 0; i < thread.wait_objects.size(); i++)
            {
                if (thread.wait_objects[i] == object)
                {
                    index = i;
                }
            }
        }

        EndWaitForObjects(best, kSuccess, index);
    }
}

bool Kernel::MustWait(uint32_t object, uint32_t thread) const
{
    const Object& o = state_.objects[object];
    switch (o.kind)
    {
    case ObjectKind::kEvent:
    case ObjectKind::kTimer:
        return !o.signalled;
    case ObjectKind::kMutex:
        return o.holder != kNone && o.holder != thread;
    case ObjectKind::kSemaphore:
        return o.count <= 0;
    case ObjectKind::kThread:
        return state_.threads[o.thread].state != ThreadState::kDead;
    default:
        return true;
    }
}

void Kernel::Acquire(uint32_t object, uint32_t thread)
{
    Object& o = state_.objects[object];
    switch (o.kind)
    {
    case ObjectKind::kEvent:
    case ObjectKind::kTimer:
        if (o.reset_type == kOneShot)
        {
            o.signalled = false;
        }
        break;

    case ObjectKind::kMutex:
        o.holder = thread;
        o.lock_count++;
        break;

    case ObjectKind::kSemaphore:
        o.count--;
        break;

    default:
        break;
    }
}

// Whether the thread can have every one of the objects: it doesn't have to wait for any of them.
bool Kernel::CanAcquireAll(const std::vector<uint32_t>& objects, uint32_t thread) const
{
    return std::none_of(objects.begin(), objects.end(), [&](uint32_t object) { return MustWait(object, thread); });
}

// The thread acquires each of the objects in order.
void Kernel::AcquireAll(const std::vector<uint32_t>& objects, uint32_t thread)
{
    for (const uint32_t object : objects)
    {
        Acquire(object, thread);
    }
}

// The thread acquires all the objects if it doesn't have to wait for any of them. Each is tested before any is
// acquired. Returns the index that such a wait reports, or nothing when the thread has to wait.
std::optional<uint32_t> Kernel::TryAcquireAll(const std::vector<uint32_t>& objects, uint32_t thread)
{
    if (!CanAcquireAll(objects, thread))
    {
        return std::nullopt;
    }

    AcquireAll(objects, thread);

    return kNoWaitIndex;
}

// The thread acquires the first of the objects that it doesn't have to wait for. Returns that object's index, or
// nothing when the thread has to wait for each of them.
std::optional<uint32_t> Kernel::TryAcquireAny(const std::vector<uint32_t>& objects, uint32_t thread)
{
    for (uint32_t i = 0; i < objects.size(); i++)
    {
        if (!MustWait(objects[i], thread))
        {
            Acquire(objects[i], thread);
            return i;
        }
    }

    return std::nullopt;
}

// A slot of thread-local storage: the one that a dead thread gave back last, zeroed, or else a new one. Every eighth
// new slot starts a new page.
uint32_t Kernel::AllocateTls()
{
    State& s = state_;
    if (!s.free_tls.empty())
    {
        const uint32_t tls = s.free_tls.back();
        s.free_tls.pop_back();
        memory_.ZeroBlock(tls, kTlsSize);
        return tls;
    }

    const uint32_t slot = s.new_tls_slots++;
    const uint32_t tls = kTlsBase + slot * kTlsSize;
    if (slot % kTlsSlotsPerPage == 0)
    {
        MapNewRegion(tls, kPageSize);
    }

    return tls;
}

arm::CpuState& Kernel::RegistersOf(uint32_t thread)
{
    return thread == state_.current ? cpu_.state_ : state_.threads[thread].registers;
}

// A new object in an empty slot, after a collection once the table has grown enough.
uint32_t Kernel::NewObject(ObjectKind kind)
{
    State& s = state_;
    if (s.free_objects.empty() && s.objects.size() >= s.collect_at)
    {
        CollectObjects();
    }

    uint32_t object;
    if (!s.free_objects.empty())
    {
        object = s.free_objects.back();
        s.free_objects.pop_back();
        s.objects[object] = Object();
    }
    else
    {
        object = static_cast<uint32_t>(s.objects.size());
        s.objects.emplace_back();
    }

    s.objects[object].kind = kind;

    return object;
}

// Empties the slots of the objects that nothing refers to any more: no handle, thread, timed event or service.
void Kernel::CollectObjects()
{
    State& s = state_;
    std::vector<bool> live(s.objects.size(), false);
    for (const HandleEntry& entry : s.handles)
    {
        live[entry.object] = true;
    }

    for (const Thread& thread : s.threads)
    {
        if (thread.object != kNone)
        {
            live[thread.object] = true;
        }

        for (const uint32_t object : thread.wait_objects)
        {
            live[object] = true;
        }

        if (thread.arbiter != kNone)
        {
            live[thread.arbiter] = true;
        }
    }

    for (const TimedEvent& event : s.events)
    {
        if (event.kind == TimedEvent::Kind::kTimerExpiry)
        {
            live[event.target] = true;
        }
    }

    if (host_)
    {
        host_->MarkObjects(live);
    }

    std::size_t live_count = 0;
    s.free_objects.clear();
    for (std::size_t i = 0; i < s.objects.size(); i++)
    {
        if (live[i])
        {
            live_count++;
        }
        else
        {
            s.objects[i] = Object();
            s.free_objects.push_back(static_cast<uint32_t>(i));
        }
    }

    s.collect_at = std::max(kCollectMinimum, 2 * live_count);
}

// The entry of `handle` in the handle table, or the table's end. The table is sorted by handle.
std::vector<Kernel::HandleEntry>::const_iterator Kernel::FindHandle(uint32_t handle) const
{
    const std::vector<HandleEntry>& handles = state_.handles;
    const auto entry = std::lower_bound(handles.begin(), handles.end(), handle,
                                        [](const HandleEntry& e, uint32_t h) { return e.handle < h; });

    return entry != handles.end() && entry->handle == handle ? entry : handles.end();
}

uint32_t Kernel::LookupKind(uint32_t handle, ObjectKind kind) const
{
    const uint32_t object = Lookup(handle);
    return object != kNone && state_.objects[object].kind == kind ? object : kNone;
}

// The object of a handle if a thread can wait for it: a thread, an event, a timer, a mutex or a semaphore. kNone for
// any other handle.
uint32_t Kernel::LookupWaitable(uint32_t handle) const
{
    const uint32_t object = Lookup(handle);
    if (object == kNone)
    {
        return kNone;
    }

    switch (state_.objects[object].kind)
    {
    case ObjectKind::kThread:
    case ObjectKind::kEvent:
    case ObjectKind::kTimer:
    case ObjectKind::kMutex:
    case ObjectKind::kSemaphore:
        return object;
    default:
        return kNone;
    }
}

// A new region of zeros, rounded up to a page, mapped nowhere yet.
uint32_t Kernel::NewRegion(uint32_t size)
{
    const uint32_t rounded = arm::Memory::RoundUpToPage(size);
    regions_.push_back({std::make_unique<uint8_t[]>(rounded), rounded});

    return static_cast<uint32_t>(regions_.size() - 1);
}

} // namespace threesf::horizon
