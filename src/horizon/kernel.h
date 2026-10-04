// SPDX-License-Identifier: MIT

// A high-level emulation of the 3DS (Horizon) kernel for one application process, enough to run a game's startup code
// and sound library: memory regions, threads with priority scheduling, the synchronisation objects, the SVC interface
// and synchronous IPC to HLE services.
//
// There's a single emulated CPU core. Time is counted in ARM11 cycles (268111856 Hz) and advances with executed
// instructions. An optional "device" (the DSP) is stepped in lock-step with the CPU. When every thread is waiting, time
// moves on towards the next timed event in short slices, so that the device's interrupts still wake threads promptly.
//
// Behaviour follows 3dbrew's documentation of the Horizon kernel; SVC register conventions, result codes and object
// semantics were checked against Azahar's behaviour.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "arm/cpu.h"
#include "common/memory_image.h"

namespace threesf::horizon
{

using Handle = uint32_t;
constexpr Handle kCurrentThread = 0xffff8000;

constexpr uint64_t kArmClock = 268111856;

// Result codes
constexpr uint32_t kResultSuccess = 0;
constexpr uint32_t kResultTimeout = 0x09401bfe;
constexpr uint32_t kResultInvalidHandle = 0xd8e007f7;
constexpr uint32_t kResultNotFound = 0xd8e007fa;
constexpr uint32_t kResultOutOfMemory = 0xd86007f3;
constexpr uint32_t kResultInvalidAddress = 0xe0e01bf5;
constexpr uint32_t kResultMisalignedAddress = 0xe0e01bf1;
constexpr uint32_t kResultMisalignedSize = 0xe0e01bf2;
constexpr uint32_t kResultOutOfRange = 0xd8e007fd;
constexpr uint32_t kResultNotLockOwner = 0xd8e0041f;
constexpr uint32_t kResultInvalidEnumValue = 0xd8e093ed;
constexpr uint32_t kResultNotImplemented = 0xf8601bfd;

// Address space
constexpr uint32_t kHeapBase = 0x08000000;
constexpr uint32_t kHeapEnd = 0x0e000000;
constexpr uint32_t kStackTop = 0x10000000;
constexpr uint32_t kLinearBase = 0x14000000;
constexpr uint32_t kDspRamBase = 0x1ff00000;
constexpr uint32_t kConfigMemBase = 0x1ff80000;
constexpr uint32_t kSharedPageBase = 0x1ff81000;
constexpr uint32_t kTlsBase = 0x1ff82000;
constexpr uint32_t kTlsSize = 0x200;
constexpr uint32_t kIpcCommandOffset = 0x80;

class Kernel;
class Thread;
class Service;

// Anything a handle can refer to. The kernel tells the kinds apart with dynamic_pointer_cast, and makes them with
// Kernel::Make, which keeps track of them for snapshots.
class Object : public std::enable_shared_from_this<Object>
{
public:
    virtual ~Object() = default;

    // A copy of the object, and copying one back into it, for snapshots (Kernel::Save). Pointers to other objects stay
    // as they are in the copy.
    virtual std::unique_ptr<Object> Clone() const = 0;
    virtual void Assign(const Object& from) = 0;

protected:
    Object() = default;
    Object(const Object&) = default;
    Object& operator=(const Object&) = default;
};

// Implements Clone and Assign for `Derived`, a kind of `Root` (Object or Service), by copying it.
template <typename Derived, typename Base, typename Root = Object>
class Cloneable : public Base
{
public:
    std::unique_ptr<Root> Clone() const override
    {
        return std::make_unique<Derived>(static_cast<const Derived&>(*this));
    }

    void Assign(const Root& from) override
    {
        static_cast<Derived&>(*this) = static_cast<const Derived&>(from);
    }
};

// An object a thread can wait on with WaitSynchronization.
class WaitObject : public Object
{
public:
    // True if `thread` would have to wait to acquire the object.
    virtual bool ShouldWait(const Thread* thread) const = 0;

    // Acquires the object for `thread` (only called when ShouldWait() is false).
    virtual void Acquire(Thread* thread) = 0;

    std::vector<std::shared_ptr<Thread>> waiters_;
};

enum class ResetType : uint32_t
{
    kOneShot = 0,
    kSticky = 1,
    kPulse = 2
};

class Event final : public Cloneable<Event, WaitObject>
{
public:
    bool ShouldWait(const Thread*) const override
    {
        return !signaled_;
    }

    void Acquire(Thread*) override
    {
        if (reset_type_ == ResetType::kOneShot)
        {
            signaled_ = false;
        }
    }

    ResetType reset_type_ = ResetType::kOneShot;
    bool signaled_ = false;

    // Called when the application signals the event (used by HLE services).
    std::function<void()> on_signal_;
};

class Mutex final : public Cloneable<Mutex, WaitObject>
{
public:
    bool ShouldWait(const Thread* thread) const override;
    void Acquire(Thread* thread) override;

    Thread* holder_ = nullptr;
    uint32_t lock_count_ = 0;
};

class Semaphore final : public Cloneable<Semaphore, WaitObject>
{
public:
    bool ShouldWait(const Thread*) const override
    {
        return count_ <= 0;
    }

    void Acquire(Thread*) override
    {
        count_--;
    }

    int32_t count_ = 0;
    int32_t max_count_ = 0;
};

class Timer final : public Cloneable<Timer, WaitObject>
{
public:
    bool ShouldWait(const Thread*) const override
    {
        return !signaled_;
    }

    void Acquire(Thread*) override
    {
        if (reset_type_ == ResetType::kOneShot)
        {
            signaled_ = false;
        }
    }

    ResetType reset_type_ = ResetType::kOneShot;
    bool signaled_ = false;
    uint64_t interval_ticks_ = 0;
    uint64_t generation_ = 0; // bumped to cancel pending expiries
};

class AddressArbiter final : public Cloneable<AddressArbiter, Object>
{
};

class ResourceLimit final : public Cloneable<ResourceLimit, Object>
{
};

class SharedMemory final : public Cloneable<SharedMemory, Object>
{
public:
    uint32_t size_ = 0;
    uint32_t source_address_ = 0; // the creator's memory the block shares, or 0 when it has its own
    uint8_t* storage_ = nullptr;  // its own memory, in whole pages
};

// A client session to an HLE service.
class Session final : public Cloneable<Session, Object>
{
public:
    std::shared_ptr<Service> service_;
};

enum class ThreadStatus
{
    kReady,
    kWaitSync,
    kWaitArbiter,
    kSleeping,
    kDead
};

class Thread final : public Cloneable<Thread, WaitObject>
{
public:
    bool ShouldWait(const Thread*) const override
    {
        return status_ != ThreadStatus::kDead;
    }

    void Acquire(Thread*) override
    {
    }

    uint32_t id_ = 0;
    arm::CpuState context_;
    int32_t priority_ = 0x30;
    ThreadStatus status_ = ThreadStatus::kReady;
    uint32_t tls_address_ = 0;
    uint64_t ready_seq_ = 0; // FIFO order among threads of equal priority

    // Wait state
    std::vector<std::shared_ptr<WaitObject>> wait_objects_;
    bool wait_all_ = false;
    bool wait_sets_index_ = false; // WaitSynchronizationN reports the object index in r1
    uint32_t arbiter_address_ = 0;
    AddressArbiter* arbiter_ = nullptr;
    uint64_t wait_generation_ = 0; // bumped on every wake to cancel stale timeouts
};

// An HLE service. Handlers read and write the IPC command buffer of the calling thread. The kernel makes them with
// Kernel::MakeService, which keeps track of them for snapshots.
class Service
{
public:
    virtual ~Service() = default;

    // Handles one request. The command buffer is at `cmdbuf` in guest memory.
    virtual void HandleRequest(Kernel& kernel, uint32_t cmdbuf) = 0;

    // A copy of the service, and copying one back into it, for snapshots (see Object).
    virtual std::unique_ptr<Service> Clone() const = 0;
    virtual void Assign(const Service& from) = 0;

protected:
    Service() = default;
    Service(const Service&) = default;
    Service& operator=(const Service&) = default;
};

struct KernelConfig
{
    uint32_t app_memory = 0x04000000; // APPMEMALLOC and the commit limit
    uint32_t program_end = 0;         // the address where the program's code, data and bss end

    // The most CPU cycles that run between device steps and event checks; every instruction takes one cycle. The DSP
    // catches up after each slice, so an interrupt it raises reaches the CPU at the end of the slice. Shorter slices
    // give the same output, but longer ones delay the DSP's interrupts and change it.
    uint64_t slice_cycles = 5000;
};

class Kernel
{
public:
    enum class RunResult
    {
        kOk,
        kExited,
        kError
    };

    explicit Kernel(const KernelConfig& config);
    ~Kernel();

    arm::Memory& Memory()
    {
        return mem_;
    }

    // Maps a zero-initialised region of host memory owned by the kernel.
    uint8_t* MapRegion(uint32_t vaddr, uint32_t size);

    // Maps caller-owned host memory (e.g. DSP RAM).
    void MapExternal(uint32_t vaddr, uint8_t* host, uint32_t size);

    // FCRAM backing for the linear heap (physical 0x20000000). Must be set before starting.
    void SetFcram(uint8_t* base, uint32_t size);

    // Creates the main thread (priority from the exheader) at `entry` with the given stack.
    void CreateMainThread(uint32_t entry, uint32_t stack_top, int32_t priority);

    // Runs until the given time (in ARM cycles), process exit or an error.
    RunResult RunUntil(uint64_t until);

    uint64_t Ticks() const
    {
        return ticks_;
    }

    void SignalEvent(Event& event);
    Handle AddHandle(std::shared_ptr<Object> object);

    // Makes a kernel object, or a service, and keeps track of it for snapshots. Every object and service is made this
    // way.
    template <typename T>
    std::shared_ptr<T> Make()
    {
        auto object = std::make_shared<T>();
        objects_.push_back(object);
        return object;
    }

    template <typename T, typename... Args>
    std::shared_ptr<T> MakeService(Args&&... args)
    {
        auto service = std::make_shared<T>(std::forward<Args>(args)...);
        services_made_.push_back(service);
        return service;
    }

    template <typename T>
    std::shared_ptr<T> Get(Handle handle)
    {
        return std::dynamic_pointer_cast<T>(GetObject(handle));
    }

    void RegisterService(const std::string& name, std::shared_ptr<Service> service);
    void RegisterPort(const std::string& name, std::shared_ptr<Service> service);
    std::shared_ptr<Service> FindService(const std::string& name);

    // Creates a client session handle to a service (for srv:GetServiceHandle).
    Handle CreateSessionHandle(std::shared_ptr<Service> service);

    Thread* CurrentThread()
    {
        return current_;
    }

    // Guest memory helpers
    uint32_t Read32(uint32_t a)
    {
        return mem_.Read32(a);
    }

    void Write32(uint32_t a, uint32_t v)
    {
        mem_.Write32(a, v);
    }

    // Formats a message and hands it to log_, or prints it to stderr when there's no log_.
    void Log(const char* fmt, ...) const;

    // Adds CPU time that passes outside the interpreter, such as the time an HLE service spends waiting for the DSP.
    void AddTicks(uint64_t cycles)
    {
        pending_cycles_ += cycles;
    }

    // Called whenever emulated time advances, with the new time. The System runs the DSP up to it.
    std::function<void(uint64_t to_ticks)> device_step_;

    // The error that made RunUntil return kError.
    std::string error_;

    // Receives the messages of Log: debug output from svcOutputDebugString and svcBreak, and warnings about requests
    // the services don't emulate.
    std::function<void(const std::string&)> log_;

    // Host memory the kernel maps: what MapRegion mapped, and memory blocks' own storage. A region stays where it is
    // while the kernel or a snapshot holds it.
    struct Region
    {
        std::unique_ptr<uint8_t[]> bytes;
        uint32_t size = 0;
    };

    // Snapshot of the kernel between runs: objects and services, scheduler, handles, timed events, kernel-owned memory
    // and its mapping, and the CPU. It keeps the objects alive and stores copies of their values. Pointers into kernel
    // memory restrict restoration to this kernel. The System saves FCRAM and the DSP separately.
    struct Snapshot
    {
        using ObjectCopy = std::pair<std::shared_ptr<Object>, std::unique_ptr<Object>>;
        using ServiceCopy = std::pair<std::shared_ptr<Service>, std::unique_ptr<Service>>;

        std::vector<ObjectCopy> objects;
        std::vector<ServiceCopy> services;

        uint64_t ticks = 0;
        uint64_t pending_cycles = 0;
        std::multimap<uint64_t, std::function<void()>> events;
        std::vector<std::shared_ptr<Thread>> threads;
        Thread* current = nullptr;
        uint32_t next_thread_id = 0;
        uint64_t ready_counter = 0;
        bool yielded = false;
        std::vector<uint32_t> free_tls;
        uint32_t tls_count = 0;
        bool exited = false;
        std::string error;
        std::map<Handle, std::shared_ptr<Object>> handles;
        Handle next_handle = 0;

        std::vector<std::shared_ptr<Region>> regions; // kept alive so the mapping's pointers remain valid
        std::vector<MemoryImage> region_images;
        std::vector<bool> fcram_used;
        uint32_t heap_size = 0;
        uint32_t linear_size = 0;
        arm::Memory::Snapshot memory;
        arm::Cpu::Snapshot cpu;

        // Approximate memory use, excluding pages shared with `previous`. Each object counts as a few hundred bytes.
        std::size_t Bytes(const Snapshot* previous) const;
    };

    // Takes a snapshot between runs, sharing the memory pages that haven't changed since `previous`.
    std::shared_ptr<Snapshot> Save(const Snapshot* previous);
    void Restore(const Snapshot& snapshot);

private:
    // Makes a region of `size` zeroed bytes.
    uint8_t* NewRegion(uint32_t size);

    std::shared_ptr<Object> GetObject(Handle handle);
    bool CloseHandle(Handle handle);
    void Schedule(uint64_t time, std::function<void()> fn);

    // Wakes threads that can now acquire `object`.
    void WakeWaiters(WaitObject& object);

    void HandleSvc(Thread& thread, uint32_t number);
    void SwitchTo(Thread& thread);
    Thread* PickNext();
    void AdvanceTo(uint64_t time);
    void RunDueEvents();
    void MakeReady(Thread& thread);

    // Puts `thread` to sleep until it can acquire `objects` (all of them, or any one), with a timeout unless
    // `timeout_ns` is negative. `sets_index` makes the wake-up report the object's index in r1.
    void WaitOn(Thread& thread, std::vector<std::shared_ptr<WaitObject>> objects, bool wait_all, int64_t timeout_ns,
                bool sets_index);

    void StartTimeout(Thread& thread, int64_t timeout_ns);
    void ResumeThread(Thread& thread, uint32_t result, int32_t index);
    void ExitThread(Thread& thread);
    void SignalTimer(const std::shared_ptr<Timer>& timer, uint64_t generation);

    // SVC implementations
    uint32_t ControlMemory(uint32_t& out_addr, uint32_t op, uint32_t addr0, uint32_t addr1, uint32_t size);
    uint32_t CreateThread(Handle& out, uint32_t entry, uint32_t arg, uint32_t stack_top, int32_t priority);
    uint32_t ArbitrateAddress(Thread& thread, Handle handle, uint32_t address, uint32_t type, int32_t value,
                              int64_t ns);
    uint32_t SendSyncRequest(Thread& thread, Handle handle);
    uint32_t AllocateTls();

    // Save and Restore copy every field below that changes after the system starts: add a new one there too.
    KernelConfig cfg_;
    arm::Memory mem_;
    arm::Cpu cpu_;

    uint64_t ticks_ = 0;
    uint64_t pending_cycles_ = 0;

    // Timed callbacks (sleep and wait timeouts, timer expiries) by the time they're due. The multimap keeps callbacks
    // due at the same time in the order they were scheduled.
    std::multimap<uint64_t, std::function<void()>> events_;

    std::vector<std::shared_ptr<Thread>> threads_;
    Thread* current_ = nullptr;
    uint32_t next_thread_id_ = 1;
    uint64_t ready_counter_ = 0;
    bool yielded_ = false; // the running thread has just yielded to threads of equal priority
    std::vector<uint32_t> free_tls_;
    uint32_t tls_count_ = 0;
    bool exited_ = false;

    // Ordered, so that ExitThread releases a thread's mutexes in the same order on every platform.
    std::map<Handle, std::shared_ptr<Object>> handles_;

    Handle next_handle_ = 0x10;

    std::map<std::string, std::shared_ptr<Service>> services_;
    std::map<std::string, std::shared_ptr<Service>> ports_;

    // Every object and service made, which snapshots copy while they're alive.
    std::vector<std::weak_ptr<Object>> objects_;
    std::vector<std::weak_ptr<Service>> services_made_;

    // Memory
    std::vector<std::shared_ptr<Region>> regions_;
    uint8_t* fcram_ = nullptr;
    uint32_t fcram_size_ = 0;
    std::vector<bool> fcram_used_; // per 4 KiB page, for linear allocations
    uint32_t heap_size_ = 0;
    uint32_t linear_size_ = 0;
};

} // namespace threesf::horizon
