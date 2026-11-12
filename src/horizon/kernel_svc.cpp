// SPDX-License-Identifier: MIT

// The SVCs of the Horizon kernel HLE (see kernel.h): the table that HandleSvc dispatches by, and the SVCs for threads,
// handles, system and process information, resource limits, IPC and debugging.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/ctr/dsp/fcram.h"

#include "arm/cpu.h"
#include "arm/memory.h"
#include "horizon/ipc.h"
#include "horizon/kernel.h"
#include "horizon/kernel_constants.h"

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

// The value that turns a linear address into a physical one (GetProcessInfo's type 20). FCRAM starts at 0x20000000 in
// physical memory.
constexpr uint32_t kLinearToPhysical = ntdlib::ctr::kFcramBase - kLinearBase;

// The process id that GetProcessId returns.
constexpr uint32_t kProcessId = 0x28;

// The most bytes that OutputDebugString reads.
constexpr uint32_t kDebugStringLimit = 4096;

} // namespace

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

    const uint32_t number = cpu_.SvcNumber();
    const auto svc =
        std::find_if(std::begin(kSvcs), std::end(kSvcs), [number](const SvcEntry& s) { return s.number == number; });
    if (svc == std::end(kSvcs))
    {
        state_.pending += kUnimplementedSvcCycles;
        Log("unimplemented SVC 0x%02x at %08x", number, cpu_.StopPc());
        cpu_.State().r[0] = kNotImplemented;
        return;
    }

    state_.pending += svc->cycles;
    (this->*svc->run)();
}

void Kernel::SvcCreateThread()
{
    arm::CpuState& r = cpu_.State();
    const uint32_t handle = CreateThread(static_cast<int32_t>(r.r[0]), r.r[1], r.r[2], r.r[3]);
    r.r[0] = kSuccess;
    r.r[1] = handle;
}

void Kernel::SvcSleepThread()
{
    const arm::CpuState& r = cpu_.State();
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
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> object = LookupForSvc(r.r[1], ObjectKind::kThread);
    if (!object)
    {
        return;
    }

    r.r[0] = kSuccess;
    r.r[1] = static_cast<uint32_t>(state_.threads[state_.objects[*object].thread].priority);
}

// Nothing checks the priority. It counts from the next pick.
void Kernel::SvcSetThreadPriority()
{
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> object = LookupForSvc(r.r[0], ObjectKind::kThread);
    if (!object)
    {
        return;
    }

    state_.threads[state_.objects[*object].thread].priority = static_cast<int32_t>(r.r[1]);
    r.r[0] = kSuccess;
}

void Kernel::SvcCloseHandle()
{
    arm::CpuState& r = cpu_.State();
    const auto entry = FindHandle(r.r[0]);
    if (entry == state_.handles.end())
    {
        r.r[0] = kInvalidHandle;
        return;
    }

    state_.handles.erase(entry);
    r.r[0] = kSuccess;
}

void Kernel::SvcDuplicateHandle()
{
    arm::CpuState& r = cpu_.State();
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
    arm::CpuState& r = cpu_.State();
    const uint64_t ticks = state_.ticks + state_.pending;
    r.r[0] = static_cast<uint32_t>(ticks);
    r.r[1] = static_cast<uint32_t>(ticks >> 32);
}

// Every handle gives 0 for every type.
void Kernel::SvcGetHandleInfo()
{
    arm::CpuState& r = cpu_.State();
    r.r[0] = kSuccess;
    r.r[1] = 0;
    r.r[2] = 0;
}

void Kernel::SvcGetSystemInfo()
{
    arm::CpuState& r = cpu_.State();
    const uint32_t type = r.r[1];
    const uint32_t parameter = r.r[2];
    const uint64_t value = type == 0 && parameter == 1 ? state_.heap_size + state_.linear_size : 0;
    r.r[0] = kSuccess;
    r.r[1] = static_cast<uint32_t>(value);
    r.r[2] = static_cast<uint32_t>(value >> 32);
}

void Kernel::SvcGetProcessInfo()
{
    arm::CpuState& r = cpu_.State();
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
    arm::CpuState& r = cpu_.State();
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
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> session = LookupForSvc(r.r[0], ObjectKind::kSession);
    if (!session)
    {
        return;
    }

    const uint32_t service = state_.objects[*session].service;
    if (host_)
    {
        host_->HandleRequest(service, CurrentTls() + ipc::kCommandBuffer);
    }

    r.r[0] = kSuccess;
}

void Kernel::SvcGetProcessId()
{
    arm::CpuState& r = cpu_.State();
    r.r[0] = kSuccess;
    r.r[1] = kProcessId;
}

void Kernel::SvcGetThreadId()
{
    arm::CpuState& r = cpu_.State();
    const std::optional<uint32_t> object = LookupForSvc(r.r[1], ObjectKind::kThread);
    if (!object)
    {
        return;
    }

    r.r[0] = kSuccess;
    r.r[1] = state_.threads[state_.objects[*object].thread].id;
}

// Each call makes a new resource limit object.
void Kernel::SvcGetResourceLimit()
{
    arm::CpuState& r = cpu_.State();
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

// For each name at r2, writes one figure of its resource at r0 as a u64. `figure` picks the limit or the amount in use.
// The writes stop at the count in r3 or after the first access that faults.
void Kernel::WriteResourceFigures(uint64_t ResourceFigures::* figure)
{
    arm::CpuState& r = cpu_.State();
    const uint32_t values = r.r[0];
    const uint32_t names = r.r[2];
    const uint32_t count = r.r[3];
    for (uint32_t i = 0; i < count && !memory_.Fault(); i++)
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
    const arm::CpuState& r = cpu_.State();
    char message[64];
    std::snprintf(message, sizeof(message), "svcBreak(%u) called from %08x", r.r[0], r.r[14]);
    error_ = message;
    Log("%s", message);
    SvcExitProcess();
}

void Kernel::SvcOutputDebugString()
{
    arm::CpuState& r = cpu_.State();
    const uint32_t length = std::min(r.r[1], kDebugStringLimit);
    std::vector<char> text(length + 1, 0);
    memory_.ReadBlock(r.r[0], text.data(), length);
    Log("[debug] %s", text.data());
    r.r[0] = kSuccess;
}

} // namespace threesf::horizon
