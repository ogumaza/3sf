// SPDX-License-Identifier: MIT

// The Horizon kernel's high-level emulation (see kernel.h).

#include "horizon/kernel.h"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/memory_image.h"

#include "arm/cpu.h"
#include "arm/memory.h"
#include "horizon/kernel_constants.h"

namespace threesf::horizon
{
namespace
{

// The handle that stands for the thread that calls an SVC, and the step between the handles of the table.
constexpr uint32_t kCurrentThread = 0xffff8000;
constexpr uint32_t kHandleStep = 0x10;

// A new thread's FPSCR: default NaN, flush to zero, round towards zero.
constexpr uint32_t kThreadFpscr = 0x03c00000;

// A log message's buffer: a longer message is cut to its first 1023 bytes.
constexpr std::size_t kLogBuffer = 1024;

// Objects are collected once the table grows to twice what was live after the last collection, and at least this.
constexpr std::size_t kCollectMinimum = 256;

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

        const uint64_t executed = cpu_.Executed();
        const arm::StopReason reason = cpu_.Run(limit - s.ticks);
        const uint64_t cost = cpu_.Executed() - executed + s.pending;
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
            memory_.ClearFault();
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
            s.threads[s.current].registers = cpu_.State();
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
    return NewEventOrTimer(ObjectKind::kEvent, reset_type);
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
        s.threads[s.current].registers = cpu_.State();
    }

    cpu_.State() = s.threads[thread].registers;
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
    std::snprintf(message, sizeof(message), "memory fault at %08x (%s) in thread %u at pc %08x", memory_.FaultAddress(),
                  memory_.FaultWrite() ? "write" : "read", CurrentThreadId(), cpu_.StopPc());

    return std::string(message) + "\n" + cpu_.Describe();
}

std::string Kernel::UndefinedMessage() const
{
    char message[96];
    std::snprintf(message, sizeof(message), "undefined instruction %08x in thread %u at pc %08x", cpu_.StopOpcode(),
                  CurrentThreadId(), cpu_.StopPc());

    return std::string(message) + "\n" + cpu_.Describe();
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

uint32_t Kernel::NewEventOrTimer(ObjectKind kind, uint32_t reset_type)
{
    const uint32_t object = NewObject(kind);
    state_.objects[object].reset_type = reset_type;

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

std::optional<uint32_t> Kernel::LookupForSvc(uint32_t handle, ObjectKind kind)
{
    const uint32_t object = LookupKind(handle, kind);
    if (object == kNone)
    {
        cpu_.State().r[0] = kInvalidHandle;
        return std::nullopt;
    }

    return object;
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

} // namespace threesf::horizon
