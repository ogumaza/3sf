// SPDX-License-Identifier: MIT

// Horizon kernel HLE (see kernel.h).

#include "horizon/kernel.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/bytes.h"
#include "dsp/fcram.h"

namespace threesf::horizon
{
namespace
{

// Approximate cost of each SVC in ARM11 cycles, as measured on hardware (the figures Azahar uses).
uint32_t SvcCycles(uint32_t number)
{
    switch (number)
    {
    case 0x08:
        return 5214;
    case 0x0a:
        return 946;
    case 0x0b:
        return 616;
    case 0x0c:
        return 1812;
    case 0x14:
        return 1324;
    case 0x16:
        return 2713;
    case 0x17:
        return 4329;
    case 0x18:
        return 3285;
    case 0x19:
        return 1389;
    case 0x1b:
        return 5163;
    case 0x22:
        return 5664;
    case 0x23:
        return 2937;
    case 0x24:
        return 4005;
    case 0x25:
        return 6918;
    case 0x28:
        return 340;
    case 0x32:
        return 5825;
    case 0x37:
        return 677;
    default:
        return 1000;
    }
}

constexpr uint32_t kInitialFpscr = 0x03c00000; // default NaN, flush to zero, round towards zero
constexpr uint32_t kProcessId = 0x28;

// ARM11 cycles for a time in nanoseconds, rounded up; 0 for a time that isn't positive.
uint64_t NsToTicks(int64_t ns)
{
    if (ns <= 0)
    {
        return 0;
    }

    const uint64_t sec = static_cast<uint64_t>(ns) / 1000000000;
    const uint64_t rem = static_cast<uint64_t>(ns) % 1000000000;
    return sec * kArmClock + (rem * kArmClock + 999999999) / 1000000000;
}

// Takes `thread` off the wait lists of the objects it waits on.
void RemoveFromWaitLists(Thread& thread)
{
    for (auto& obj : thread.wait_objects_)
    {
        std::erase_if(obj->waiters_, [&](const std::shared_ptr<Thread>& t) { return t.get() == &thread; });
    }

    thread.wait_objects_.clear();
    thread.arbiter_ = nullptr;
}

// Releases references to threads waiting on `object` and, if it's a thread, to objects it waits on. These shared
// pointers form cycles that must be broken before the objects can be freed.
void ForgetWaits(Object& object)
{
    if (auto* w = dynamic_cast<WaitObject*>(&object))
    {
        w->waiters_.clear();
    }

    if (auto* t = dynamic_cast<Thread*>(&object))
    {
        t->wait_objects_.clear();
    }
}

} // namespace

bool Mutex::ShouldWait(const Thread* thread) const
{
    return holder_ != nullptr && holder_ != thread;
}

void Mutex::Acquire(Thread* thread)
{
    holder_ = thread;
    lock_count_++;
}

Kernel::Kernel(const KernelConfig& config) : cfg_(config), cpu_(mem_)
{
    // Configuration memory (values from firmware 11.17, as in Azahar).
    uint8_t* config_mem = MapRegion(kConfigMemBase, 0x1000);
    config_mem[0x02] = 0x3a; // kernel version 2.58
    config_mem[0x03] = 0x02;
    StoreLe32(config_mem + 0x08, 0x00008002);
    StoreLe32(config_mem + 0x0c, 0x00040130);
    StoreLe32(config_mem + 0x10, 2);
    config_mem[0x14] = 1; // retail
    config_mem[0x16] = 1;
    StoreLe32(config_mem + 0x18, 0x0000f450);
    StoreLe32(config_mem + 0x30, 0); // memory mode 0 (64 MiB)
    StoreLe32(config_mem + 0x40, cfg_.app_memory);
    StoreLe32(config_mem + 0x44, 0x02c00000);
    StoreLe32(config_mem + 0x48, 0x01400000);
    config_mem[0x62] = 0x3a;
    config_mem[0x63] = 0x02;
    StoreLe32(config_mem + 0x64, 2);
    StoreLe32(config_mem + 0x68, 0x0000f450);

    // Shared page
    uint8_t* shared = MapRegion(kSharedPageBase, 0x1000);
    shared[0x04] = 1;            // running hardware: product
    shared[0x85] = (5 << 2) | 3; // battery: charge level 5, adapter connected, charging
    shared[0x86] = 1;
}

Kernel::~Kernel()
{
    for (const std::weak_ptr<Object>& weak : objects_)
    {
        if (std::shared_ptr<Object> object = weak.lock())
        {
            ForgetWaits(*object);
        }
    }
}

void Kernel::Log(const char* fmt, ...) const
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (log_)
    {
        log_(buf);
    }
    else
    {
        std::fprintf(stderr, "%s\n", buf);
    }
}

// --------------------------------------------------------------------------------------------- Memory
uint8_t* Kernel::NewRegion(uint32_t size)
{
    auto region = std::make_shared<Region>();
    region->bytes = std::make_unique<uint8_t[]>(size);
    region->size = size;
    regions_.push_back(region);

    return region->bytes.get();
}

uint8_t* Kernel::MapRegion(uint32_t vaddr, uint32_t size)
{
    size = (size + arm::Memory::kPageMask) & ~arm::Memory::kPageMask;
    uint8_t* p = NewRegion(size);
    mem_.Map(vaddr, p, size);

    return p;
}

void Kernel::MapExternal(uint32_t vaddr, uint8_t* host, uint32_t size)
{
    mem_.Map(vaddr, host, size);
}

void Kernel::SetFcram(uint8_t* base, uint32_t size)
{
    fcram_ = base;
    fcram_size_ = size;
    fcram_used_.assign(size / arm::Memory::kPageSize, false);
}

uint32_t Kernel::ControlMemory(uint32_t& out_addr, uint32_t op, uint32_t addr0, uint32_t addr1, uint32_t size)
{
    if ((addr0 | addr1) & arm::Memory::kPageMask)
    {
        return kResultMisalignedAddress;
    }

    const uint32_t kind = op & 0xff;
    const bool linear = (op & 0x10000) != 0;
    size = (size + arm::Memory::kPageMask) & ~arm::Memory::kPageMask;

    switch (kind)
    {
    case 3: // COMMIT
        {
            if (linear)
            {
                if (!fcram_)
                {
                    return kResultOutOfMemory;
                }

                const uint32_t pages = size / arm::Memory::kPageSize;
                uint32_t first = 0;
                if (addr0 != 0)
                {
                    first = (addr0 - kLinearBase) / arm::Memory::kPageSize;
                }
                else
                {
                    // First fit from the start of FCRAM.
                    uint32_t run = 0;
                    bool found = false;
                    for (uint32_t i = 0; i < fcram_used_.size(); i++)
                    {
                        run = fcram_used_[i] ? 0 : run + 1;
                        if (run == pages)
                        {
                            first = i + 1 - pages;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        return kResultOutOfMemory;
                    }
                }

                if (first + pages > fcram_used_.size())
                {
                    return kResultOutOfMemory;
                }

                for (uint32_t i = first; i < first + pages; i++)
                {
                    fcram_used_[i] = true;
                }

                const uint32_t offset = first * arm::Memory::kPageSize;
                std::memset(fcram_ + offset, 0, size);
                out_addr = kLinearBase + offset;
                mem_.Map(out_addr, fcram_ + offset, size);
                linear_size_ += size;
                return kResultSuccess;
            }

            if (addr0 < kHeapBase || addr0 > kHeapEnd || size > kHeapEnd - addr0)
            {
                return kResultInvalidAddress;
            }

            MapRegion(addr0, size);
            heap_size_ += size;
            out_addr = addr0;
            return kResultSuccess;
        }

    case 1: // FREE
        {
            if (linear || (addr0 >= kLinearBase && addr0 < kLinearBase + fcram_size_))
            {
                const uint32_t first = (addr0 - kLinearBase) / arm::Memory::kPageSize;
                for (uint32_t i = 0; i < size / arm::Memory::kPageSize && first + i < fcram_used_.size(); i++)
                {
                    fcram_used_[first + i] = false;
                }
                linear_size_ -= std::min(linear_size_, size);
            }
            else
            {
                heap_size_ -= std::min(heap_size_, size);
            }

            mem_.Unmap(addr0, size);
            out_addr = addr0;
            return kResultSuccess;
        }

    case 4: // MAP addr1 to addr0
        {
            for (uint32_t off = 0; off < size; off += arm::Memory::kPageSize)
            {
                uint8_t* p = mem_.HostPtr(addr1 + off);
                if (!p)
                {
                    return kResultInvalidAddress;
                }

                mem_.Map(addr0 + off, p, arm::Memory::kPageSize);
            }

            out_addr = addr0;
            return kResultSuccess;
        }

    case 5: // UNMAP
        mem_.Unmap(addr0, size);
        out_addr = addr0;
        return kResultSuccess;

    case 6: // PROTECT
        out_addr = addr0;
        return kResultSuccess;

    default:
        Log("ControlMemory: unsupported operation %08x", op);
        return kResultNotImplemented;
    }
}

uint32_t Kernel::AllocateTls()
{
    if (!free_tls_.empty())
    {
        const uint32_t a = free_tls_.back();
        free_tls_.pop_back();
        mem_.ZeroBlock(a, kTlsSize);
        return a;
    }

    const uint32_t a = kTlsBase + tls_count_ * kTlsSize;
    if ((tls_count_ % (arm::Memory::kPageSize / kTlsSize)) == 0)
    {
        MapRegion(a, arm::Memory::kPageSize);
    }

    tls_count_++;

    return a;
}

// --------------------------------------------------------------------------------------------- Snapshots
std::shared_ptr<Kernel::Snapshot> Kernel::Save(const Snapshot* previous)
{
    auto snapshot = std::make_shared<Snapshot>();

    std::erase_if(objects_, [](const std::weak_ptr<Object>& o) { return o.expired(); });
    for (const std::weak_ptr<Object>& weak : objects_)
    {
        std::shared_ptr<Object> object = weak.lock();
        std::unique_ptr<Object> copy = object->Clone();
        snapshot->objects.emplace_back(std::move(object), std::move(copy));
    }

    std::erase_if(services_made_, [](const std::weak_ptr<Service>& s) { return s.expired(); });
    for (const std::weak_ptr<Service>& weak : services_made_)
    {
        std::shared_ptr<Service> service = weak.lock();
        std::unique_ptr<Service> copy = service->Clone();
        snapshot->services.emplace_back(std::move(service), std::move(copy));
    }

    snapshot->ticks = ticks_;
    snapshot->pending_cycles = pending_cycles_;
    snapshot->events = events_;
    snapshot->threads = threads_;
    snapshot->current = current_;
    snapshot->next_thread_id = next_thread_id_;
    snapshot->ready_counter = ready_counter_;
    snapshot->yielded = yielded_;
    snapshot->free_tls = free_tls_;
    snapshot->tls_count = tls_count_;
    snapshot->exited = exited_;
    snapshot->error = error_;
    snapshot->handles = handles_;
    snapshot->next_handle = next_handle_;

    snapshot->regions = regions_;
    for (std::size_t i = 0; i < regions_.size(); i++)
    {
        const MemoryImage* before = previous && i < previous->region_images.size() &&
                                            previous->regions[i] == regions_[i]
                                        ? &previous->region_images[i]
                                        : nullptr;
        snapshot->region_images.emplace_back(regions_[i]->bytes.get(), regions_[i]->size, before);
    }

    snapshot->fcram_used = fcram_used_;
    snapshot->heap_size = heap_size_;
    snapshot->linear_size = linear_size_;
    snapshot->memory = mem_.Save(previous ? &previous->memory : nullptr);
    snapshot->cpu = cpu_.Save();

    return snapshot;
}

std::size_t Kernel::Snapshot::Bytes(const Snapshot* previous) const
{
    std::size_t bytes = (objects.size() + services.size()) * 256 + events.size() * 64;
    for (std::size_t i = 0; i < region_images.size(); i++)
    {
        const bool same = previous && i < previous->regions.size() && previous->regions[i] == regions[i];
        bytes += region_images[i].BytesNotIn(same ? &previous->region_images[i] : nullptr);
    }

    if (!previous || previous->memory.runs != memory.runs)
    {
        bytes += memory.runs->size() * sizeof(arm::Memory::Snapshot::Run);
    }

    return bytes;
}

void Kernel::Restore(const Snapshot& snapshot)
{
    // Release references between objects absent from this snapshot. Other snapshots may keep them alive and restore
    // their saved values later.
    for (const std::weak_ptr<Object>& weak : objects_)
    {
        std::shared_ptr<Object> object = weak.lock();
        if (object && std::ranges::find(snapshot.objects, object, &Snapshot::ObjectCopy::first) == snapshot.objects.end())
        {
            ForgetWaits(*object);
        }
    }

    objects_.clear();
    for (const auto& [object, copy] : snapshot.objects)
    {
        object->Assign(*copy);
        objects_.push_back(object);
    }

    services_made_.clear();
    for (const auto& [service, copy] : snapshot.services)
    {
        service->Assign(*copy);
        services_made_.push_back(service);
    }

    ticks_ = snapshot.ticks;
    pending_cycles_ = snapshot.pending_cycles;
    events_ = snapshot.events;
    threads_ = snapshot.threads;
    current_ = snapshot.current;
    next_thread_id_ = snapshot.next_thread_id;
    ready_counter_ = snapshot.ready_counter;
    yielded_ = snapshot.yielded;
    free_tls_ = snapshot.free_tls;
    tls_count_ = snapshot.tls_count;
    exited_ = snapshot.exited;
    error_ = snapshot.error;
    handles_ = snapshot.handles;
    next_handle_ = snapshot.next_handle;

    regions_ = snapshot.regions;
    for (std::size_t i = 0; i < regions_.size(); i++)
    {
        snapshot.region_images[i].Restore(regions_[i]->bytes.get());
    }

    fcram_used_ = snapshot.fcram_used;
    heap_size_ = snapshot.heap_size;
    linear_size_ = snapshot.linear_size;
    mem_.Restore(snapshot.memory);
    cpu_.Restore(snapshot.cpu);
}

// --------------------------------------------------------------------------------------------- Handles and objects
Handle Kernel::AddHandle(std::shared_ptr<Object> object)
{
    const Handle h = next_handle_;
    next_handle_ += 0x10;
    handles_[h] = std::move(object);

    return h;
}

std::shared_ptr<Object> Kernel::GetObject(Handle handle)
{
    if (handle == kCurrentThread)
    {
        return current_ ? current_->shared_from_this() : nullptr;
    }

    auto it = handles_.find(handle);
    return it == handles_.end() ? nullptr : it->second;
}

bool Kernel::CloseHandle(Handle handle)
{
    return handles_.erase(handle) != 0;
}

void Kernel::RegisterService(const std::string& name, std::shared_ptr<Service> service)
{
    services_[name] = std::move(service);
}

std::shared_ptr<Service> Kernel::FindService(const std::string& name)
{
    auto it = services_.find(name);
    return it == services_.end() ? nullptr : it->second;
}

void Kernel::RegisterPort(const std::string& name, std::shared_ptr<Service> service)
{
    ports_[name] = std::move(service);
}

Handle Kernel::CreateSessionHandle(std::shared_ptr<Service> service)
{
    auto s = Make<Session>();
    s->service_ = std::move(service);

    return AddHandle(s);
}

// --------------------------------------------------------------------------------------------- Threads and scheduling
void Kernel::CreateMainThread(uint32_t entry, uint32_t stack_top, int32_t priority)
{
    Handle h;
    CreateThread(h, entry, 0, stack_top, priority);
}

uint32_t Kernel::CreateThread(Handle& out, uint32_t entry, uint32_t arg, uint32_t stack_top, int32_t priority)
{
    auto t = Make<Thread>();
    t->id_ = next_thread_id_++;
    t->priority_ = priority;
    t->context_.r[0] = arg;
    t->context_.r[13] = stack_top & ~7u;
    t->context_.r[15] = entry & ~1u;
    t->context_.thumb = (entry & 1) != 0;
    t->context_.fpscr = kInitialFpscr;
    t->tls_address_ = AllocateTls();
    t->context_.tls = t->tls_address_;
    MakeReady(*t);
    threads_.push_back(t);
    out = AddHandle(t);

    return kResultSuccess;
}

void Kernel::MakeReady(Thread& thread)
{
    thread.status_ = ThreadStatus::kReady;
    thread.ready_seq_ = ++ready_counter_;
}

Thread* Kernel::PickNext()
{
    // Among threads of equal priority, Horizon keeps the current thread running unless it yields. A yield moves it to
    // the end of the ready queue; the others run in the order they became ready.
    const auto first = [this](const Thread* a, const Thread* b)
    {
        const bool a_keeps = a == current_ && !yielded_;
        const bool b_keeps = b == current_ && !yielded_;
        return a_keeps != b_keeps ? a_keeps : a->ready_seq_ < b->ready_seq_;
    };

    Thread* best = nullptr;
    for (auto& t : threads_)
    {
        if (t->status_ != ThreadStatus::kReady)
        {
            continue;
        }

        if (!best || t->priority_ < best->priority_ || (t->priority_ == best->priority_ && first(t.get(), best)))
        {
            best = t.get();
        }
    }

    yielded_ = false;

    return best;
}

void Kernel::SwitchTo(Thread& thread)
{
    if (current_ == &thread)
    {
        return;
    }

    if (current_)
    {
        current_->context_ = cpu_.state_;
    }

    current_ = &thread;
    cpu_.state_ = thread.context_;
    cpu_.ClearExclusive();
}

void Kernel::Schedule(uint64_t time, std::function<void()> fn)
{
    events_.emplace(time, std::move(fn));
}

void Kernel::AdvanceTo(uint64_t time)
{
    if (time <= ticks_)
    {
        return;
    }

    ticks_ = time;

    if (device_step_)
    {
        device_step_(time);
    }
}

void Kernel::RunDueEvents()
{
    while (!events_.empty() && events_.begin()->first <= ticks_)
    {
        auto fn = std::move(events_.begin()->second);
        events_.erase(events_.begin());
        fn();
    }
}

void Kernel::ResumeThread(Thread& thread, uint32_t result, int32_t index)
{
    thread.wait_generation_++;

    if (thread.status_ == ThreadStatus::kWaitSync)
    {
        thread.context_.r[0] = result;
        if (thread.wait_sets_index_)
        {
            thread.context_.r[1] = static_cast<uint32_t>(index);
        }
    }

    MakeReady(thread);
}

void Kernel::StartTimeout(Thread& thread, int64_t timeout_ns)
{
    const uint64_t gen = thread.wait_generation_;
    auto self = std::static_pointer_cast<Thread>(thread.shared_from_this());
    const auto on_timeout = [this, self, gen]
    {
        if (self->wait_generation_ != gen)
        {
            return;
        }

        if (self->status_ == ThreadStatus::kWaitSync)
        {
            RemoveFromWaitLists(*self);
            ResumeThread(*self, kResultTimeout, -1);
        }
        else if (self->status_ == ThreadStatus::kWaitArbiter || self->status_ == ThreadStatus::kSleeping)
        {
            RemoveFromWaitLists(*self);
            ResumeThread(*self, 0, 0);
        }
    };
    Schedule(ticks_ + NsToTicks(timeout_ns), on_timeout);
}

void Kernel::WaitOn(Thread& thread, std::vector<std::shared_ptr<WaitObject>> objects, bool wait_all, int64_t timeout_ns,
                    bool sets_index)
{
    thread.wait_objects_ = std::move(objects);
    thread.wait_all_ = wait_all;
    thread.wait_sets_index_ = sets_index;
    thread.status_ = ThreadStatus::kWaitSync;
    auto self = std::static_pointer_cast<Thread>(thread.shared_from_this());
    for (auto& obj : thread.wait_objects_)
    {
        obj->waiters_.push_back(self);
    }

    if (timeout_ns >= 0)
    {
        StartTimeout(thread, timeout_ns);
    }
}

void Kernel::WakeWaiters(WaitObject& object)
{
    while (true)
    {
        Thread* best = nullptr;
        for (auto& t : object.waiters_)
        {
            if (t->status_ != ThreadStatus::kWaitSync)
            {
                continue;
            }

            bool can;
            if (t->wait_all_)
            {
                can = true;
                for (auto& o : t->wait_objects_)
                {
                    if (o->ShouldWait(t.get()))
                    {
                        can = false;
                    }
                }
            }
            else
            {
                can = !object.ShouldWait(t.get());
            }

            if (can && (!best || t->priority_ < best->priority_))
            {
                best = t.get();
            }
        }
        if (!best)
        {
            return;
        }

        int32_t index = -1;
        if (best->wait_all_)
        {
            for (auto& o : best->wait_objects_)
            {
                o->Acquire(best);
            }
        }
        else
        {
            object.Acquire(best);
            for (std::size_t i = 0; i < best->wait_objects_.size(); i++)
            {
                if (best->wait_objects_[i].get() == &object)
                {
                    index = static_cast<int32_t>(i);
                }
            }
        }

        RemoveFromWaitLists(*best);
        ResumeThread(*best, kResultSuccess, index);
    }
}

void Kernel::SignalEvent(Event& event)
{
    event.signaled_ = true;
    WakeWaiters(event);
    if (event.reset_type_ == ResetType::kPulse)
    {
        event.signaled_ = false;
    }
}

void Kernel::SignalTimer(const std::shared_ptr<Timer>& timer, uint64_t generation)
{
    if (timer->generation_ != generation)
    {
        return;
    }

    timer->signaled_ = true;
    WakeWaiters(*timer);
    if (timer->reset_type_ == ResetType::kPulse)
    {
        timer->signaled_ = false;
    }

    if (timer->interval_ticks_)
    {
        Schedule(ticks_ + timer->interval_ticks_, [this, timer, generation] { SignalTimer(timer, generation); });
    }
}

void Kernel::ExitThread(Thread& thread)
{
    thread.status_ = ThreadStatus::kDead;

    // Release the mutexes the thread holds.
    for (auto& [h, obj] : handles_)
    {
        if (auto m = std::dynamic_pointer_cast<Mutex>(obj); m && m->holder_ == &thread)
        {
            m->holder_ = nullptr;
            m->lock_count_ = 0;
            WakeWaiters(*m);
        }
    }

    WakeWaiters(thread);
    free_tls_.push_back(thread.tls_address_);
}

Kernel::RunResult Kernel::RunUntil(uint64_t until)
{
    while (true)
    {
        if (exited_)
        {
            // svcBreak ends the program with a reason in error_.
            return error_.empty() ? RunResult::kExited : RunResult::kError;
        }

        RunDueEvents();

        if (ticks_ >= until)
        {
            return RunResult::kOk;
        }

        Thread* next = PickNext();
        if (!next)
        {
            // Idle: advance in slices so that interrupts raised by the device (the DSP) wake threads promptly.
            uint64_t target = std::min(until, ticks_ + cfg_.slice_cycles);
            if (!events_.empty())
            {
                target = std::min(target, events_.begin()->first);
            }

            AdvanceTo(target);
            continue;
        }

        SwitchTo(*next);
        uint64_t limit = std::min(until, ticks_ + cfg_.slice_cycles);
        if (!events_.empty())
        {
            limit = std::min(limit, std::max(events_.begin()->first, ticks_ + 1));
        }

        const uint64_t before = cpu_.executed_;
        const arm::StopReason reason = cpu_.Run(limit - ticks_);
        const uint64_t cost = cpu_.executed_ - before + pending_cycles_;
        pending_cycles_ = 0;
        AdvanceTo(ticks_ + cost);

        switch (reason)
        {
        case arm::StopReason::kSvc:
            HandleSvc(*current_, cpu_.svc_number_);
            break;

        case arm::StopReason::kBudget:
        case arm::StopReason::kNone:
            break;

        case arm::StopReason::kYield:
            current_->ready_seq_ = ++ready_counter_;
            yielded_ = true;
            break;

        case arm::StopReason::kFault:
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "memory fault at %08x (%s) in thread %u at pc %08x",
                              mem_.fault_address_, mem_.fault_write_ ? "write" : "read", current_->id_, cpu_.stop_pc_);
                error_ = std::string(buf) + "\n" + cpu_.Describe();
                mem_.fault_ = false;
                return RunResult::kError;
            }

        case arm::StopReason::kUndefined:
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "undefined instruction %08x in thread %u at pc %08x", cpu_.stop_opcode_,
                              current_->id_, cpu_.stop_pc_);
                error_ = std::string(buf) + "\n" + cpu_.Describe();
                return RunResult::kError;
            }
        }

        if (current_ && current_->status_ != ThreadStatus::kReady)
        {
            current_->context_ = cpu_.state_;
            current_ = nullptr;
        }
    }
}

// --------------------------------------------------------------------------------------------- SVCs
uint32_t Kernel::ArbitrateAddress(Thread& thread, Handle handle, uint32_t address, uint32_t type, int32_t value,
                                  int64_t ns)
{
    auto arbiter = Get<AddressArbiter>(handle);
    if (!arbiter)
    {
        return kResultInvalidHandle;
    }

    const auto block = [&](bool timeout)
    {
        thread.status_ = ThreadStatus::kWaitArbiter;
        thread.arbiter_ = arbiter.get();
        thread.arbiter_address_ = address;
        if (timeout)
        {
            StartTimeout(thread, ns);
        }
    };

    switch (type)
    {
    case 0: // signal
        {
            int32_t woken = 0;
            while (value < 0 || woken < value)
            {
                Thread* best = nullptr;
                for (auto& t : threads_)
                {
                    if (t->status_ == ThreadStatus::kWaitArbiter && t->arbiter_ == arbiter.get() &&
                        t->arbiter_address_ == address && (!best || t->priority_ < best->priority_))
                    {
                        best = t.get();
                    }
                }
                if (!best)
                {
                    break;
                }

                best->arbiter_ = nullptr;
                ResumeThread(*best, 0, 0);
                woken++;
            }

            return kResultSuccess;
        }

    case 1: // wait if less than
        if (static_cast<int32_t>(mem_.Read32(address)) < value)
        {
            block(false);
        }
        return kResultSuccess;

    case 2: // decrement and wait if less than
        {
            const int32_t v = static_cast<int32_t>(mem_.Read32(address));
            if (v < value)
            {
                mem_.Write32(address, static_cast<uint32_t>(v) - 1);
                block(false);
            }
            return kResultSuccess;
        }

    case 3: // wait if less than, with timeout
        if (static_cast<int32_t>(mem_.Read32(address)) < value)
        {
            block(true);
        }
        return kResultTimeout;

    case 4: // decrement and wait if less than, with timeout
        {
            const int32_t v = static_cast<int32_t>(mem_.Read32(address));
            if (v < value)
            {
                mem_.Write32(address, static_cast<uint32_t>(v) - 1);
                block(true);
            }
            return kResultTimeout;
        }

    default:
        return kResultInvalidEnumValue;
    }
}

uint32_t Kernel::SendSyncRequest(Thread& thread, Handle handle)
{
    auto session = Get<Session>(handle);
    if (!session)
    {
        return kResultInvalidHandle;
    }

    const uint32_t cmdbuf = thread.tls_address_ + kIpcCommandOffset;
    session->service_->HandleRequest(*this, cmdbuf);

    return kResultSuccess;
}

void Kernel::HandleSvc(Thread& thread, uint32_t number)
{
    auto& r = cpu_.state_.r;
    pending_cycles_ += SvcCycles(number);

    const auto timeout_ns = [](uint32_t lo, uint32_t hi)
    {
        return static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | lo);
    };

    switch (number)
    {
    case 0x01: // ControlMemory
        {
            uint32_t out = 0;
            r[0] = ControlMemory(out, r[0], r[1], r[2], r[3]);
            r[1] = out;
            break;
        }

    case 0x03: // ExitProcess
        exited_ = true;
        thread.status_ = ThreadStatus::kDead;
        break;

    case 0x08: // CreateThread(priority, entry, arg, stack top, processor): one core, so the processor doesn't matter
        {
            Handle h = 0;
            const int32_t priority = static_cast<int32_t>(r[0]);
            r[0] = CreateThread(h, r[1], r[2], r[3], priority);
            r[1] = h;
            break;
        }

    case 0x09: // ExitThread
        ExitThread(thread);
        break;

    case 0x0a: // SleepThread
        {
            const int64_t ns = timeout_ns(r[0], r[1]);
            if (ns <= 0)
            {
                thread.ready_seq_ = ++ready_counter_; // yield to threads of equal priority
                yielded_ = true;
                break;
            }

            thread.status_ = ThreadStatus::kSleeping;
            StartTimeout(thread, ns);
            break;
        }

    case 0x0b: // GetThreadPriority
        {
            auto t = Get<Thread>(r[1]);
            if (!t)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            r[0] = kResultSuccess;
            r[1] = static_cast<uint32_t>(t->priority_);
            break;
        }

    case 0x0c: // SetThreadPriority
        {
            auto t = Get<Thread>(r[0]);
            if (!t)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            t->priority_ = static_cast<int32_t>(r[1]);
            r[0] = kResultSuccess;
            break;
        }

    case 0x13: // CreateMutex
        {
            auto m = Make<Mutex>();
            if (r[1])
            {
                m->holder_ = &thread;
                m->lock_count_ = 1;
            }
            r[0] = kResultSuccess;
            r[1] = AddHandle(m);
            break;
        }

    case 0x14: // ReleaseMutex
        {
            auto m = Get<Mutex>(r[0]);
            if (!m)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            if (m->holder_ != &thread)
            {
                r[0] = kResultNotLockOwner;
                break;
            }

            if (--m->lock_count_ == 0)
            {
                m->holder_ = nullptr;
                WakeWaiters(*m);
            }

            r[0] = kResultSuccess;
            break;
        }

    case 0x15: // CreateSemaphore
        {
            auto s = Make<Semaphore>();
            s->count_ = static_cast<int32_t>(r[1]);
            s->max_count_ = static_cast<int32_t>(r[2]);
            r[0] = kResultSuccess;
            r[1] = AddHandle(s);
            break;
        }

    case 0x16: // ReleaseSemaphore
        {
            auto s = Get<Semaphore>(r[1]);
            if (!s)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            const int32_t release = static_cast<int32_t>(r[2]);
            if (release < 0 || int64_t{s->count_} + release > s->max_count_)
            {
                r[0] = kResultOutOfRange;
                break;
            }

            const int32_t previous = s->count_;
            s->count_ += release;
            WakeWaiters(*s);
            r[0] = kResultSuccess;
            r[1] = static_cast<uint32_t>(previous);
            break;
        }

    case 0x17: // CreateEvent
        {
            auto e = Make<Event>();
            e->reset_type_ = static_cast<ResetType>(r[1]);
            r[0] = kResultSuccess;
            r[1] = AddHandle(e);
            break;
        }

    case 0x18: // SignalEvent
        {
            auto e = Get<Event>(r[0]);
            if (!e)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            r[0] = kResultSuccess;
            if (e->on_signal_)
            {
                e->on_signal_();
            }
            SignalEvent(*e);
            break;
        }

    case 0x19: // ClearEvent
        {
            auto e = Get<Event>(r[0]);
            if (!e)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            e->signaled_ = false;
            r[0] = kResultSuccess;
            break;
        }

    case 0x1a: // CreateTimer
        {
            auto t = Make<Timer>();
            t->reset_type_ = static_cast<ResetType>(r[1]);
            r[0] = kResultSuccess;
            r[1] = AddHandle(t);
            break;
        }

    case 0x1b: // SetTimer(handle, initial = r2:r3, interval = r1:r4)
        {
            auto t = Get<Timer>(r[0]);
            if (!t)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            const int64_t initial = timeout_ns(r[2], r[3]);
            const int64_t interval = timeout_ns(r[1], r[4]);
            const uint64_t gen = ++t->generation_;
            t->interval_ticks_ = NsToTicks(interval);
            Schedule(ticks_ + NsToTicks(initial), [this, t, gen] { SignalTimer(t, gen); });
            r[0] = kResultSuccess;
            break;
        }

    case 0x1c: // CancelTimer
        {
            auto t = Get<Timer>(r[0]);
            if (!t)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            t->generation_++;
            r[0] = kResultSuccess;
            break;
        }

    case 0x1d: // ClearTimer
        {
            auto t = Get<Timer>(r[0]);
            if (!t)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            t->signaled_ = false;
            r[0] = kResultSuccess;
            break;
        }

    case 0x1e: // CreateMemoryBlock(other_perm = r0, addr = r1, size = r2, my_perm = r3)
        {
            if (r[1] & arm::Memory::kPageMask)
            {
                r[0] = kResultInvalidAddress;
                break;
            }

            if (r[2] & arm::Memory::kPageMask)
            {
                r[0] = kResultMisalignedSize;
                break;
            }

            auto block = Make<SharedMemory>();
            block->size_ = r[2];
            block->source_address_ = r[1];
            if (r[1] == 0)
            {
                // The kernel keeps the memory, since its pages stay mapped after the block's handle is closed.
                block->storage_ = NewRegion(r[2]);
            }

            r[0] = kResultSuccess;
            r[1] = AddHandle(block);
            break;
        }

    case 0x1f: // MapMemoryBlock(handle, addr, my_perm, other_perm)
        {
            auto block = Get<SharedMemory>(r[0]);
            if (!block)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            if (r[1] & arm::Memory::kPageMask)
            {
                r[0] = kResultMisalignedAddress;
                break;
            }

            // A page at a time: the creator's pages needn't be one piece of host memory.
            r[0] = kResultSuccess;
            for (uint64_t off = 0; off < block->size_; off += arm::Memory::kPageSize)
            {
                const auto offset = static_cast<uint32_t>(off);
                uint8_t* page =
                    block->source_address_ ? mem_.HostPtr(block->source_address_ + offset) : block->storage_ + offset;
                if (!page)
                {
                    r[0] = kResultInvalidAddress;
                    break;
                }

                mem_.Map(r[1] + offset, page, arm::Memory::kPageSize);
            }
            break;
        }

    case 0x20: // UnmapMemoryBlock(handle, addr)
        {
            auto block = Get<SharedMemory>(r[0]);
            if (!block)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            mem_.Unmap(r[1], block->size_);
            r[0] = kResultSuccess;
            break;
        }

    case 0x21: // CreateAddressArbiter
        r[0] = kResultSuccess;
        r[1] = AddHandle(Make<AddressArbiter>());
        break;

    case 0x22: // ArbitrateAddress(handle, address, type, value, ns = r4:r5)
        r[0] = ArbitrateAddress(thread, r[0], r[1], r[2], static_cast<int32_t>(r[3]), timeout_ns(r[4], r[5]));
        break;

    case 0x23: // CloseHandle
        r[0] = CloseHandle(r[0]) ? kResultSuccess : kResultInvalidHandle;
        break;

    case 0x24: // WaitSynchronization1(handle, ns = r2:r3)
        {
            auto obj = std::dynamic_pointer_cast<WaitObject>(GetObject(r[0]));
            if (!obj)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            const int64_t ns = timeout_ns(r[2], r[3]);

            if (!obj->ShouldWait(&thread))
            {
                obj->Acquire(&thread);
                r[0] = kResultSuccess;
                break;
            }

            r[0] = kResultTimeout;
            if (ns != 0)
            {
                WaitOn(thread, {obj}, false, ns, false);
            }
            break;
        }

    case 0x25: // WaitSynchronizationN(ns lo = r0, handles = r1, count = r2, wait_all = r3, ns hi = r4)
        {
            const int64_t ns = timeout_ns(r[0], r[4]);
            const uint32_t count = r[2];
            const bool wait_all = r[3] != 0;
            std::vector<std::shared_ptr<WaitObject>> objects;
            for (uint32_t i = 0; i < count; i++)
            {
                auto obj = std::dynamic_pointer_cast<WaitObject>(GetObject(mem_.Read32(r[1] + 4 * i)));
                if (!obj)
                {
                    break;
                }

                objects.push_back(std::move(obj));
            }

            if (objects.size() != count)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            const auto ready = [&](const std::shared_ptr<WaitObject>& o)
            {
                return !o->ShouldWait(&thread);
            };
            if (wait_all && std::ranges::all_of(objects, ready))
            {
                for (auto& o : objects)
                {
                    o->Acquire(&thread);
                }

                r[0] = kResultSuccess;
                r[1] = 0xffffffff;
                break;
            }

            if (!wait_all)
            {
                if (const auto it = std::ranges::find_if(objects, ready); it != objects.end())
                {
                    (*it)->Acquire(&thread);
                    r[0] = kResultSuccess;
                    r[1] = static_cast<uint32_t>(it - objects.begin());
                    break;
                }
            }

            r[0] = kResultTimeout;
            r[1] = 0xffffffff;
            if (ns == 0)
            {
                break;
            }

            if (count == 0) // used as a sleep
            {
                thread.status_ = ThreadStatus::kSleeping;
                StartTimeout(thread, ns);
                break;
            }

            WaitOn(thread, std::move(objects), wait_all, ns, true);
            break;
        }

    case 0x27: // DuplicateHandle
        {
            auto obj = GetObject(r[1]);
            if (!obj)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            r[0] = kResultSuccess;
            r[1] = AddHandle(obj);
            break;
        }

    case 0x28: // GetSystemTick
        {
            const uint64_t now = ticks_ + pending_cycles_;
            r[0] = static_cast<uint32_t>(now);
            r[1] = static_cast<uint32_t>(now >> 32);
            break;
        }

    case 0x29: // GetHandleInfo
        r[0] = kResultSuccess;
        r[1] = 0;
        r[2] = 0;
        break;

    case 0x2a: // GetSystemInfo(type = r1, param = r2)
        {
            // Only the memory the application region uses (type 0, param 1) is reported; everything else is 0.
            const uint64_t value = r[1] == 0 && r[2] == 1 ? heap_size_ + linear_size_ : 0;
            r[0] = kResultSuccess;
            r[1] = static_cast<uint32_t>(value);
            r[2] = static_cast<uint32_t>(value >> 32);
            break;
        }

    case 0x2b: // GetProcessInfo(handle = r1, type = r2)
        {
            int64_t value = 0;
            switch (r[2])
            {
            case 0:
                value = heap_size_ + linear_size_;
                break;
            case 2:
            case 6: // linear memory usage
                value = linear_size_;
                break;
            case 20:
                value = static_cast<int64_t>(kFcramBase) - kLinearBase;
                break;
            default:
                break;
            }

            r[0] = kResultSuccess;
            r[1] = static_cast<uint32_t>(value);
            r[2] = static_cast<uint32_t>(static_cast<uint64_t>(value) >> 32);
            break;
        }

    case 0x2d: // ConnectToPort(name = r1)
        {
            const std::string name = mem_.ReadCString(r[1], 12);
            auto it = ports_.find(name);
            if (it == ports_.end())
            {
                Log("ConnectToPort: unknown port '%s'", name.c_str());
                r[0] = kResultNotFound;
                break;
            }

            r[0] = kResultSuccess;
            r[1] = CreateSessionHandle(it->second);
            break;
        }

    case 0x32: // SendSyncRequest
        r[0] = SendSyncRequest(thread, r[0]);
        break;

    case 0x35: // GetProcessId
        r[0] = kResultSuccess;
        r[1] = kProcessId;
        break;

    case 0x37: // GetThreadId
        {
            auto t = Get<Thread>(r[1]);
            if (!t)
            {
                r[0] = kResultInvalidHandle;
                break;
            }

            r[0] = kResultSuccess;
            r[1] = t->id_;
            break;
        }

    case 0x38: // GetResourceLimit
        r[0] = kResultSuccess;
        r[1] = AddHandle(Make<ResourceLimit>());
        break;

    case 0x39: // GetResourceLimitLimitValues(values, handle, names, count)
    case 0x3a: // GetResourceLimitCurrentValues
        {
            // A count that runs into unmapped memory stops at the fault, which ends the emulation.
            const bool limit = number == 0x39;
            for (uint32_t i = 0; i < r[3] && !mem_.fault_; i++)
            {
                const uint32_t name = mem_.Read32(r[2] + 4 * i);
                int64_t value = 0;
                switch (name)
                {
                case 0: // priority
                    value = 0x18;
                    break;
                case 1: // commit: the heap and linear memory, plus the address where the program ends
                    value = limit ? cfg_.app_memory : heap_size_ + linear_size_ + cfg_.program_end;
                    break;
                case 2: // threads
                    value = limit ? 0x20 : static_cast<int64_t>(threads_.size());
                    break;
                case 3: // events
                case 4: // mutexes
                    value = limit ? 0x20 : 0;
                    break;
                case 5: // semaphores
                case 6: // timers
                    value = limit ? 0x08 : 0;
                    break;
                case 7: // shared memory blocks
                    value = limit ? 0x10 : 0;
                    break;
                case 8: // address arbiters
                    value = limit ? 0x02 : 0;
                    break;
                default: // CPU time (9) and anything unknown: 0
                    break;
                }

                mem_.Write64(r[0] + 8 * i, static_cast<uint64_t>(value));
            }

            r[0] = kResultSuccess;
            break;
        }

    case 0x3c: // Break
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "svcBreak(%u) called from %08x", r[0], r[14]);
            error_ = buf;
            Log("%s", buf);
            exited_ = true;
            thread.status_ = ThreadStatus::kDead;
            break;
        }

    case 0x3d: // OutputDebugString
        {
            std::string s(r[1] < 4096 ? r[1] : 4096, '\0');
            mem_.ReadBlock(r[0], s.data(), s.size());
            Log("[debug] %s", s.c_str());
            r[0] = kResultSuccess;
            break;
        }

    default:
        Log("unimplemented SVC 0x%02x at %08x", number, cpu_.stop_pc_);
        r[0] = kResultNotImplemented;
        break;
    }
}

} // namespace threesf::horizon
