// SPDX-License-Identifier: MIT

// The synchronisation of the Horizon kernel HLE (see kernel.h): mutexes, semaphores, events, timers and address
// arbiters, the waits on them, and the states of the threads.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "arm/cpu.h"
#include "arm/memory.h"
#include "horizon/kernel.h"
#include "horizon/kernel_constants.h"

namespace threesf::horizon
{
namespace
{

// ArbitrateAddress's types.
constexpr uint32_t kArbitrateSignal = 0;
constexpr uint32_t kArbitrateWaitIfLess = 1;
constexpr uint32_t kArbitrateDecrementAndWaitIfLess = 2;
constexpr uint32_t kArbitrateWaitIfLessTimeout = 3;
constexpr uint32_t kArbitrateDecrementAndWaitIfLessTimeout = 4;

constexpr uint64_t kNsPerSecond = 1'000'000'000;

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

// A mutex that the calling thread holds from the start when r1 isn't 0.
void Kernel::SvcCreateMutex()
{
    arm::CpuState& r = cpu_.State();
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
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> mutex = LookupForSvc(r.r[0], ObjectKind::kMutex);
    if (!mutex)
    {
        return;
    }

    Object& object = state_.objects[*mutex];
    if (object.holder != state_.current)
    {
        r.r[0] = kNotOwner;
        return;
    }

    if (--object.lock_count == 0)
    {
        object.holder = kNone;
        WakeWaiters(*mutex);
    }

    r.r[0] = kSuccess;
}

void Kernel::SvcCreateSemaphore()
{
    arm::CpuState& r = cpu_.State();
    const uint32_t semaphore = NewSemaphore(static_cast<int32_t>(r.r[1]), static_cast<int32_t>(r.r[2]));
    r.r[0] = kSuccess;
    r.r[1] = NewHandle(semaphore);
}

void Kernel::SvcReleaseSemaphore()
{
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> semaphore = LookupForSvc(r.r[1], ObjectKind::kSemaphore);
    if (!semaphore)
    {
        return;
    }

    Object& object = state_.objects[*semaphore];
    const auto release = static_cast<int32_t>(r.r[2]);
    if (release < 0 || int64_t{object.count} + release > object.maximum)
    {
        r.r[0] = kOutOfRange;
        return;
    }

    r.r[1] = static_cast<uint32_t>(object.count);
    object.count += release;
    WakeWaiters(*semaphore);
    r.r[0] = kSuccess;
}

void Kernel::SvcCreateEvent()
{
    arm::CpuState& r = cpu_.State();
    const uint32_t event = NewEvent(r.r[1]);
    r.r[0] = kSuccess;
    r.r[1] = NewHandle(event);
}

// A hooked event runs the services' hook before the signal.
void Kernel::SvcSignalEvent()
{
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> event = LookupForSvc(r.r[0], ObjectKind::kEvent);
    if (!event)
    {
        return;
    }

    if (state_.objects[*event].hooked && host_)
    {
        host_->RunSignalHook(*event);
    }

    Signal(*event);
    r.r[0] = kSuccess;
}

void Kernel::SvcClearEvent()
{
    ClearSignal(ObjectKind::kEvent);
}

void Kernel::SvcCreateTimer()
{
    arm::CpuState& r = cpu_.State();
    const uint32_t timer = NewEventOrTimer(ObjectKind::kTimer, r.r[1]);
    r.r[0] = kSuccess;
    r.r[1] = NewHandle(timer);
}

// Sets a timer to expire after the initial time, and then after each interval. The expiries scheduled before become
// stale.
void Kernel::SvcSetTimer()
{
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> timer = LookupForSvc(r.r[0], ObjectKind::kTimer);
    if (!timer)
    {
        return;
    }

    Object& object = state_.objects[*timer];
    object.generation++;
    object.interval = NsToTicks(Nanoseconds(r.r[1], r.r[4]));
    ScheduleExpiry(*timer, state_.ticks + NsToTicks(Nanoseconds(r.r[2], r.r[3])));
    r.r[0] = kSuccess;
}

// The expiries scheduled before become stale. The signal stays.
void Kernel::SvcCancelTimer()
{
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> timer = LookupForSvc(r.r[0], ObjectKind::kTimer);
    if (!timer)
    {
        return;
    }

    state_.objects[*timer].generation++;
    r.r[0] = kSuccess;
}

void Kernel::SvcClearTimer()
{
    ClearSignal(ObjectKind::kTimer);
}

// Clears the signal of the event or timer that r0 refers to: ClearEvent and ClearTimer.
void Kernel::ClearSignal(ObjectKind kind)
{
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> object = LookupForSvc(r.r[0], kind);
    if (!object)
    {
        return;
    }

    state_.objects[*object].signalled = false;
    r.r[0] = kSuccess;
}

void Kernel::SvcCreateAddressArbiter()
{
    arm::CpuState& r = cpu_.State();
    const uint32_t arbiter = NewObject(ObjectKind::kArbiter);
    r.r[0] = kSuccess;
    r.r[1] = NewHandle(arbiter);
}

// Signals the threads that wait on an arbiter at an address or makes the calling thread wait there.
void Kernel::SvcArbitrateAddress()
{
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> arbiter = LookupForSvc(r.r[0], ObjectKind::kArbiter);
    if (!arbiter)
    {
        return;
    }

    const uint32_t address = r.r[1];
    const uint32_t type = r.r[2];
    const auto value = static_cast<int32_t>(r.r[3]);
    switch (type)
    {
    case kArbitrateSignal:
        SignalArbiter(*arbiter, address, value);
        r.r[0] = kSuccess;
        break;

    case kArbitrateWaitIfLess:
    case kArbitrateDecrementAndWaitIfLess:
    case kArbitrateWaitIfLessTimeout:
    case kArbitrateDecrementAndWaitIfLessTimeout:
        r.r[0] = WaitOnArbiter(*arbiter, address, type, value, Nanoseconds(r.r[4], r.r[5]));
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

void Kernel::SvcWaitSynchronization1()
{
    arm::CpuState& r = cpu_.State();
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
    arm::CpuState& r = cpu_.State();
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

// The thread waits for the objects, joining each one's waiter list once for each place it has in the list. A negative
// time means no limit.
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
    return thread == state_.current ? cpu_.State() : state_.threads[thread].registers;
}

} // namespace threesf::horizon
