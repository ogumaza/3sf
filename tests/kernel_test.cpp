// SPDX-License-Identifier: MIT

// Unit tests for the Horizon kernel HLE (src/horizon): runs small ARM programs in an emulated system and checks what
// the SVCs give them.

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "check.h"
#include "common/bytes.h"
#include "horizon/system.h"

using namespace threesf;
using namespace threesf::horizon;

// Words of an ARM program, stored little-endian.
static std::vector<uint8_t> Words(std::initializer_list<uint32_t> words)
{
    std::vector<uint8_t> out;
    for (uint32_t w : words)
    {
        for (int i = 0; i < 4; i++)
        {
            out.push_back(static_cast<uint8_t>(w >> (8 * i)));
        }
    }

    return out;
}

// Runs a program that asks GetResourceLimitCurrentValues for the committed memory, and returns the answer. The program
// is at 0x100000 and `size` bytes long; `extra` is a segment mapped apart from it, as a rip's driver is.
static uint64_t CommittedMemory(uint32_t size, const ProcessImage::Segment& extra)
{
    constexpr uint32_t kValues = 0x100100; // the s64 the SVC writes
    constexpr uint32_t kNames = 0x100108;  // the resources asked for: 1, commit

    std::vector<uint8_t> program = Words({
        0xe59f001c, // ldr r0, =0xffff8001 (the current process)
        0xef000038, // svc 0x38: GetResourceLimit, which leaves the handle in r1
        0xe59f000c, // ldr r0, =kValues
        0xe59f200c, // ldr r2, =kNames
        0xe3a03001, // mov r3, #1
        0xef00003a, // svc 0x3a: GetResourceLimitCurrentValues(r0 values, r1 handle, r2 names, r3 count)
        0xef000003, // svc 0x03: ExitProcess
        kValues,
        kNames,
        0xffff8001,
    });
    program.resize(size, 0);
    StoreLe32(program.data() + (kNames - 0x100000), 1);

    ProcessImage image;
    image.memory.push_back({0x100000, program});
    image.memory.push_back(extra);
    image.entry = 0x100000;

    System system(image, KernelConfig{});
    Kernel& k = system.GetKernel();
    THREESF_CHECK(k.RunUntil(1'000'000) == Kernel::RunResult::kExited);

    return k.Memory().Read32(kValues) | uint64_t{k.Memory().Read32(kValues + 4)} << 32;
}

// The committed memory counts the program up to the end of its last page, and not the segment mapped apart from it.
// Nothing has allocated heap or linear memory yet.
static void TestCommittedMemory()
{
    const ProcessImage::Segment kDriver = {0x0e000000, std::vector<uint8_t>(0x10, 0)};
    THREESF_CHECK(CommittedMemory(0x2345, kDriver) == 0x103000);
    THREESF_CHECK(CommittedMemory(0x3000, kDriver) == 0x103000);
    THREESF_CHECK(CommittedMemory(0x3001, kDriver) == 0x104000);

    // A segment that continues the program's region is part of it.
    const ProcessImage::Segment kBss = {0x104000, std::vector<uint8_t>(0x1800, 0)};
    THREESF_CHECK(CommittedMemory(0x3001, kBss) == 0x106000);
}

// Runs a program whose main thread starts a second thread at its own priority, then yields until the second one has set
// a flag, and returns true if it got to the end. The main thread yields with svcSleepThread(0), or with the YIELD hint
// if `hint` is true.
static bool YieldLetsTheOtherThreadRun(bool hint)
{
    constexpr uint32_t kFlag = 0x101000;
    constexpr uint32_t kStackTop = 0x102000;

    std::vector<uint8_t> program = Words({
        0xe59f003c,                     // ldr r0, =0x30 (the main thread's priority)
        0xe59f103c,                     // ldr r1, =second
        0xe3a02000,                     // mov r2, #0
        0xe59f3038,                     // ldr r3, =kStackTop
        0xef000008,                     // svc 0x08: CreateThread(r0 priority, r1 entry, r2 arg, r3 stack top)
        hint ? 0xe320f001 : 0xe3a00000, // wait: yield, or mov r0, #0
        hint ? 0xe1a00000 : 0xe3a01000, // nop, or mov r1, #0
        hint ? 0xe1a00000 : 0xef00000a, // nop, or svc 0x0a: SleepThread(0)
        0xe59f2028,                     // ldr r2, =kFlag
        0xe5922000,                     // ldr r2, [r2]
        0xe3520000,                     // cmp r2, #0
        0x0afffff8,                     // beq wait
        0xef000003,                     // svc 0x03: ExitProcess
        0xe59f0014,                     // second: ldr r0, =kFlag
        0xe3a01001,                     // mov r1, #1
        0xe5801000,                     // str r1, [r0]
        0xef000009,                     // svc 0x09: ExitThread
        0x30,
        0x100034,
        kStackTop,
        kFlag,
    });
    program.resize(kStackTop - 0x100000, 0);

    ProcessImage image;
    image.memory.push_back({0x100000, program});
    image.entry = 0x100000;

    System system(image, KernelConfig{});

    return system.GetKernel().RunUntil(10'000'000) == Kernel::RunResult::kExited;
}

// Horizon does not time-slice threads of equal priority. A yielding thread moves to the back of their queue.
static void TestYield()
{
    THREESF_CHECK(YieldLetsTheOtherThreadRun(false));
    THREESF_CHECK(YieldLetsTheOtherThreadRun(true));
}

int main()
{
    TestCommittedMemory();
    TestYield();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("kernel tests passed\n");

    return 0;
}
