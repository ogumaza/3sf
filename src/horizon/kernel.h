// SPDX-License-Identifier: MIT

// A high-level emulation of the Horizon kernel for one 3DS application process (3dbrew, "SVC" and "Kernel ABI"): the
// address space, the threads and their scheduling on one ARM11 core, the kernel objects and the handle table, the
// SVCs, and the clock with its timed events. The services that the process reaches through IPC are in services.h.
// The System (system.h) joins the kernel, the services and the DSP.
//
// Time counts ARM11 cycles. Each instruction takes one cycle and each SVC a fixed number. The kernel runs the CPU in
// slices of at most KernelConfig::slice_cycles. Each time the clock moves on, the kernel steps the DSP.
//
// State holds every object, thread and timed event. They refer to each other by index. A snapshot is therefore a copy
// of State, with images of the memory regions that the kernel made.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/memory_image.h"

#include "arm/cpu.h"

#if defined(__clang__) || defined(__GNUC__)
#define THREESF_PRINTF_FORMAT(format_index, first_argument) \
    __attribute__((format(printf, format_index, first_argument)))
#else
#define THREESF_PRINTF_FORMAT(format_index, first_argument)
#endif

namespace threesf::horizon
{

// The ARM11's clock: emulated cycles per second. The DSP runs at half of it.
constexpr uint64_t kArmClock = 268111856;

// An index that no object, thread or service has.
constexpr uint32_t kNone = 0xffffffff;

// The result code of a request or SVC that succeeds.
constexpr uint32_t kSuccess = 0;

// The kernel's settings.
struct KernelConfig
{
    // The application's memory in bytes: configuration memory shows it and the resource limit allows it. The System
    // gives FCRAM twice that size.
    uint32_t app_memory = 0x04000000;

    // Where the program's code, data and bss end. The System computes it. Callers leave it at 0.
    uint32_t program_end = 0;

    // The longest run of CPU cycles between two checks for timed events and DSP steps. It changes the output.
    uint64_t slice_cycles = 5000;
};

// The calls that the kernel makes to the services (services.h).
class ServiceHost
{
public:
    virtual ~ServiceHost() = default;

    // The service behind the port called `name`, or kNone for a port that doesn't exist.
    virtual uint32_t PortService(const std::string& name) const = 0;

    // Handles the request in the command buffer at `command_buffer` for a session to `service`.
    virtual void HandleRequest(uint32_t service, uint32_t command_buffer) = 0;

    // Runs the hook of the event `object`. The kernel calls it when the guest signals the event.
    virtual void RunSignalHook(uint32_t object) = 0;

    // Sets live[object] for each object that the services refer to.
    virtual void MarkObjects(std::vector<bool>& live) const = 0;
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

    // The kernel's state at one moment, with the contents of its memory regions.
    struct Snapshot;

    // Maps and fills configuration memory and the shared page.
    explicit Kernel(const KernelConfig& config);
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;
    ~Kernel();

    // Runs the process until the time reaches `until` (kOk), the process exits (kExited) or an error stops it
    // (kError). The time can end up past `until`.
    RunResult RunUntil(uint64_t until);

    // The emulated time in ARM11 cycles since the system started, without the cycles that SVCs have spent since the
    // last CPU run.
    uint64_t Ticks() const;

    // The process's address space.
    arm::Memory& Memory();

    // A guest word through Memory(). An unmapped address sets the memory's fault flag.
    uint32_t Read32(uint32_t address);
    void Write32(uint32_t address, uint32_t value);

    // The calls that build the process, made by the System: FCRAM for linear memory, new regions of zeros, the
    // services, the device step that runs each time the clock moves on to a new time, and threads.
    void SetLinearMemory(uint8_t* fcram, uint32_t size);
    uint8_t* MapNewRegion(uint32_t address, uint32_t size);
    void SetServiceHost(ServiceHost* host);
    void SetDeviceStep(std::function<void(uint64_t ticks)> step);
    uint32_t CreateThread(int32_t priority, uint32_t entry, uint32_t argument, uint32_t stack_top);

    // The services' access to objects and handles. Objects are referred to by index. A new object has no handle.
    uint32_t NewEvent(uint32_t reset_type);
    uint32_t NewMutex();
    uint32_t NewSemaphore(int32_t count, int32_t maximum);
    uint32_t NewSession(uint32_t service);
    uint32_t NewHandle(uint32_t object);
    uint32_t Lookup(uint32_t handle) const;
    bool IsEvent(uint32_t object) const;
    void SetSignalHook(uint32_t event);
    void Signal(uint32_t object);

    // Adds cycles that a service spent waiting. They reach the clock at the next CPU run.
    void AddPending(uint64_t cycles);

    // The id and the thread-local storage of the thread that runs, or 0 when none does.
    uint32_t CurrentThreadId() const;
    uint32_t CurrentTls() const;

    // Formats a message as printf does, in at most 1023 bytes, and passes it to log_, or prints it to stderr.
    void Log(const char* format, ...) THREESF_PRINTF_FORMAT(2, 3);

    // A snapshot of the kernel. It may share unchanged memory with `previous`, an earlier snapshot of this kernel.
    Snapshot Save(const Snapshot* previous) const;
    void Restore(const Snapshot& snapshot);

    // The message of the error that ended a run with kError.
    std::string error_;

    // Receives each log message, without a newline. When it's empty, messages go to stderr.
    std::function<void(const std::string&)> log_;

private:
    enum class ThreadState : uint8_t
    {
        kReady,
        kWaitObjects,
        kWaitArbiter,
        kSleeping,
        kDead
    };

    enum class ObjectKind : uint8_t
    {
        kFree, // a slot that a collection has emptied
        kThread,
        kEvent,
        kTimer,
        kMutex,
        kSemaphore,
        kArbiter,
        kMemoryBlock,
        kResourceLimit,
        kSession
    };

    struct Thread
    {
        uint32_t id = 0;
        int32_t priority = 0; // a lower number runs first
        ThreadState state = ThreadState::kReady;
        arm::CpuState registers; // saved while the thread doesn't run
        uint32_t tls = 0;
        uint64_t ready_number = 0; // orders threads of equal priority
        uint32_t object = kNone;   // the thread's kernel object

        // The wait: the objects in the order given, duplicates included, whether the wait is for all of them and
        // whether it reports an index, or the arbiter and address it waits on. The generation goes up with each wake.
        // A timeout started before the wake is then stale.
        std::vector<uint32_t> wait_objects;
        bool wait_all = false;
        bool wait_reports_index = false;
        uint32_t arbiter = kNone;
        uint32_t arbiter_address = 0;
        uint64_t wait_generation = 0;
    };

    // An object of any kind. Each kind uses only some of the fields.
    struct Object
    {
        ObjectKind kind = ObjectKind::kFree;
        std::vector<uint32_t> waiters; // threads, in the order they started waiting

        // Events and timers. A hooked event runs the services' hook when the guest signals it.
        bool signalled = false;
        bool hooked = false;
        uint32_t reset_type = 0; // 0 one-shot, 1 sticky, 2 pulse; any other value behaves as sticky
        uint64_t interval = 0;   // a timer's, in ticks
        uint64_t generation = 0; // a timer's: each set and cancel makes the expiries scheduled before it stale

        // Mutexes and semaphores.
        uint32_t holder = kNone;
        uint32_t lock_count = 0;
        int32_t count = 0;
        int32_t maximum = 0;

        // Threads, memory blocks and sessions.
        uint32_t thread = kNone;
        uint32_t address = 0;
        uint32_t size = 0;
        uint32_t storage = kNone; // the region of a memory block made without an address
        uint32_t service = kNone;
    };

    // A timeout of a thread's wait or sleep, or an expiry of a timer. The event holds the generation that the thread's
    // wait or the timer had when the event was scheduled. Once the generation has moved on, the event does nothing.
    struct TimedEvent
    {
        enum class Kind : uint8_t
        {
            kTimeout,    // the target is a thread
            kTimerExpiry // the target is a timer object
        };

        // The order of the queue: whether `a` runs after `b`.
        static bool RunsAfter(const TimedEvent& a, const TimedEvent& b);

        uint64_t due = 0;
        uint64_t order = 0; // events due at the same time run in the order they were scheduled
        Kind kind = Kind::kTimeout;
        uint32_t target = 0;
        uint64_t generation = 0;
    };

    // An entry of the handle table: a handle and the object that it refers to.
    struct HandleEntry
    {
        uint32_t handle = 0;
        uint32_t object = kNone;
    };

    // Host memory that the kernel made and maps. Regions live as long as the kernel. Mappings and snapshots refer to
    // them.
    struct Region
    {
        std::unique_ptr<uint8_t[]> host;
        uint32_t size = 0;
    };

    // The figures of a resource that GetResourceLimitLimitValues and GetResourceLimitCurrentValues give: its limit and
    // the amount in use.
    struct ResourceFigures
    {
        uint64_t limit = 0;
        uint64_t current = 0;
    };

    // Everything that can change after construction, apart from memory, the CPU and error_.
    struct State
    {
        // The time, the cycles that haven't reached it yet, and the timed events in a heap with the earliest first.
        uint64_t ticks = 0;
        uint64_t pending = 0;
        std::vector<TimedEvent> events;
        uint64_t next_event_order = 0;

        // The threads in the order they were made, dead ones included.
        std::vector<Thread> threads;
        uint32_t current = kNone; // the running thread or kNone
        uint32_t next_thread_id = 1;
        uint32_t ready_threads = 0; // how many threads are ready
        uint64_t ready_counter = 0; // the last ready number that a thread got
        bool just_yielded = false;
        std::vector<uint32_t> free_tls; // given back by dead threads, the most recent last
        uint32_t new_tls_slots = 0;
        bool exited = false;

        // The handle table, sorted by handle, and the objects. Empty slots are reused after a collection.
        std::vector<HandleEntry> handles;
        uint32_t next_handle = 0x10;
        std::vector<Object> objects;
        std::vector<uint32_t> free_objects;
        std::size_t collect_at = 0; // NewObject collects once the objects fill this many slots

        // Memory: a flag for each page of FCRAM, set while the page is in use as linear memory, and the sizes of the
        // heap and of linear memory. Snapshots share the flags until they change (FcramUsedForChange).
        std::shared_ptr<std::vector<uint8_t>> fcram_used = std::make_shared<std::vector<uint8_t>>();
        uint32_t heap_size = 0;
        uint32_t linear_size = 0;
    };

    // The run loop's parts.
    void RunDueEvents();
    void ExpireTimeout(uint32_t thread, uint64_t generation);
    void ExpireTimer(uint32_t timer, uint64_t generation);
    uint32_t PickThread();
    void SwitchTo(uint32_t thread);
    void AdvanceTo(uint64_t ticks);
    std::string FaultMessage() const;
    std::string UndefinedMessage() const;

    // SVCs.
    void HandleSvc();
    void SvcControlMemory();
    uint32_t ControlMemory(uint32_t operation, uint32_t addr0, uint32_t addr1, uint32_t size, uint32_t& address);
    uint32_t CommitHeap(uint32_t addr0, uint32_t size);
    uint32_t CommitLinear(uint32_t addr0, uint32_t size, uint32_t& address);
    std::optional<uint64_t> FirstFreePages(uint64_t pages) const;
    void FreeMemory(uint32_t addr0, uint32_t size, bool linear);
    bool MapAlias(uint32_t to, uint32_t from, uint32_t size);
    std::vector<uint8_t>& FcramUsedForChange();
    void SvcCreateThread();
    void SvcSleepThread();
    void SvcExitThread();
    void SvcExitProcess();
    void SvcGetThreadPriority();
    void SvcSetThreadPriority();
    void SvcCreateMutex();
    void SvcReleaseMutex();
    void SvcCreateSemaphore();
    void SvcReleaseSemaphore();
    void SvcCreateEvent();
    void SvcSignalEvent();
    void SvcClearEvent();
    void SvcCreateTimer();
    void SvcSetTimer();
    void SvcCancelTimer();
    void SvcClearTimer();
    void SvcCreateMemoryBlock();
    void SvcMapMemoryBlock();
    void SvcUnmapMemoryBlock();
    void SvcCreateAddressArbiter();
    void SvcArbitrateAddress();
    void SignalArbiter(uint32_t arbiter, uint32_t address, int32_t value);
    uint32_t WaitOnArbiter(uint32_t arbiter, uint32_t address, uint32_t type, int32_t value, int64_t ns);
    void SvcCloseHandle();
    void SvcWaitSynchronization1();
    void SvcWaitSynchronizationN();
    bool ReadWaitObjects(uint32_t address, uint32_t count, std::vector<uint32_t>& objects);
    void SvcDuplicateHandle();
    void SvcGetSystemTick();
    void SvcGetHandleInfo();
    void SvcGetSystemInfo();
    void SvcGetProcessInfo();
    void SvcConnectToPort();
    void SvcSendSyncRequest();
    void SvcGetProcessId();
    void SvcGetThreadId();
    void SvcGetResourceLimit();
    void SvcGetResourceLimitLimitValues();
    void SvcGetResourceLimitCurrentValues();
    ResourceFigures Resource(uint32_t name) const;
    void WriteResourceFigures(uint64_t ResourceFigures::* figure);
    void SvcBreak();
    void SvcOutputDebugString();

    // Threads and waits.
    void SetThreadState(uint32_t thread, ThreadState state);
    void MakeReady(uint32_t thread);
    void Yield(uint32_t thread);
    void Sleep(uint32_t thread, int64_t ns);
    void Resume(uint32_t thread);
    void WaitForObjects(uint32_t thread, std::vector<uint32_t> objects, bool all, int64_t ns, bool reports_index);
    void EndWaitForObjects(uint32_t thread, uint32_t result, uint32_t index);
    void LeaveWaiterLists(uint32_t thread);
    void StartTimeout(uint32_t thread, int64_t ns);
    void ScheduleExpiry(uint32_t timer, uint64_t due);
    void Schedule(uint64_t due, TimedEvent::Kind kind, uint32_t target, uint64_t generation);
    void WakeWaiters(uint32_t object);
    bool MustWait(uint32_t object, uint32_t thread) const;
    void Acquire(uint32_t object, uint32_t thread);
    bool CanAcquireAll(const std::vector<uint32_t>& objects, uint32_t thread) const;
    void AcquireAll(const std::vector<uint32_t>& objects, uint32_t thread);
    std::optional<uint32_t> TryAcquireAll(const std::vector<uint32_t>& objects, uint32_t thread);
    std::optional<uint32_t> TryAcquireAny(const std::vector<uint32_t>& objects, uint32_t thread);
    uint32_t AllocateTls();
    arm::CpuState& RegistersOf(uint32_t thread);

    // Objects and handles.
    uint32_t NewObject(ObjectKind kind);
    void CollectObjects();
    std::vector<HandleEntry>::const_iterator FindHandle(uint32_t handle) const;
    uint32_t LookupKind(uint32_t handle, ObjectKind kind) const;
    uint32_t LookupWaitable(uint32_t handle) const;
    uint32_t NewRegion(uint32_t size);

    KernelConfig config_;
    State state_;
    std::vector<Region> regions_;
    arm::Memory memory_;
    arm::Cpu cpu_;
    uint8_t* fcram_ = nullptr;
    uint32_t fcram_size_ = 0;
    ServiceHost* host_ = nullptr;
    std::function<void(uint64_t)> device_step_;
};

struct Kernel::Snapshot
{
    State state;
    std::string error;
    std::vector<xsflib::MemoryImage> regions; // the contents of the regions that existed, in the order they were made
    arm::Memory::Snapshot memory;
    arm::Cpu::Snapshot cpu;
};

} // namespace threesf::horizon
