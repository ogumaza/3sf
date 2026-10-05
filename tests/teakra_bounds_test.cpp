// SPDX-License-Identifier: MIT

// Checks that a rip's DSP firmware can't access memory outside the DSP or hang the player through DMA. 3SF's Teakra
// changes wrap DMA addresses and channel numbers and limit transfer sizes. Tests use the MMIO registers just as
// firmware would. No game data needed.

#include <teakra/teakra.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include "check.h"
#include "common/bytes.h"

namespace
{

using threesf::LoadLe16;
using threesf::StoreLe16;

// DMA registers, as offsets into the MMIO region.
constexpr uint16_t kDmaChannel = 0x1BE;
constexpr uint16_t kDmaSourceLow = 0x1C0;
constexpr uint16_t kDmaSourceHigh = 0x1C2;
constexpr uint16_t kDmaDestinationLow = 0x1C4;
constexpr uint16_t kDmaDestinationHigh = 0x1C6;
constexpr uint16_t kDmaSize0 = 0x1C8;
constexpr uint16_t kDmaSize1 = 0x1CA;
constexpr uint16_t kDmaSize2 = 0x1CC;
constexpr uint16_t kDmaSpaces = 0x1DA;
constexpr uint16_t kDmaControl = 0x1DE;

// Written to the control register, starts the transfer.
constexpr uint16_t kDmaStart = 0x40C0;

// In the spaces register: move two words at a time.
constexpr uint16_t kDmaDwordMode = 1 << 10;

// DSP memory in 16-bit words: 512 KB, with data memory from word 0x20000. DMA addresses in the data space count from
// there.
constexpr uint32_t kMemoryWords = 0x40000;
constexpr uint32_t kDataMemoryWord = 0x20000;

// Returns word `word` of DSP memory.
uint16_t ReadWord(const Teakra::Teakra& teakra, uint32_t word)
{
    return LoadLe16(teakra.GetDspMemory() + word * 2);
}

// Starts a transfer within the data space with DMA channel 0.
void Transfer(Teakra::Teakra& teakra, uint32_t source, uint32_t destination, uint16_t size0, uint16_t size1,
              uint16_t size2, uint16_t spaces)
{
    teakra.MMIOWrite(kDmaChannel, 0);
    teakra.MMIOWrite(kDmaSourceLow, static_cast<uint16_t>(source));
    teakra.MMIOWrite(kDmaSourceHigh, static_cast<uint16_t>(source >> 16));
    teakra.MMIOWrite(kDmaDestinationLow, static_cast<uint16_t>(destination));
    teakra.MMIOWrite(kDmaDestinationHigh, static_cast<uint16_t>(destination >> 16));
    teakra.MMIOWrite(kDmaSize0, size0);
    teakra.MMIOWrite(kDmaSize1, size1);
    teakra.MMIOWrite(kDmaSize2, size2);
    teakra.MMIOWrite(kDmaSpaces, spaces);
    teakra.MMIOWrite(kDmaControl, kDmaStart);
}

// Copies one word within the data space with DMA channel 0.
void CopyWord(Teakra::Teakra& teakra, uint32_t source, uint32_t destination)
{
    Transfer(teakra, source, destination, 1, 1, 1, 0);
}

// A dword transfer of 0xFFFF words ends. Its counter steps by two, so a 16-bit one went from 0xFFFE to 0 and never
// reached the size.
void TestDwordTransferEnds()
{
    Teakra::Teakra teakra(Teakra::UserConfig{});
    constexpr uint32_t kSource = 0x100;
    constexpr uint32_t kDestination = 0x8000;
    StoreLe16(teakra.GetDspMemory() + (kDataMemoryWord + kSource) * 2, 0x1234);

    Transfer(teakra, kSource, kDestination, 0xFFFF, 1, 1, kDmaDwordMode);

    THREESF_CHECK(ReadWord(teakra, kDataMemoryWord + kDestination) == 0x1234);
}

// A transfer longer than the DSP could make is refused with an error, instead of running for years.
void TestLongTransferRefused()
{
    Teakra::Teakra teakra(Teakra::UserConfig{});

    bool refused = false;
    try
    {
        Transfer(teakra, 0x100, 0x8000, 0xFFFF, 0xFFFF, 0xFFFF, 0);
    }
    catch (const std::runtime_error&)
    {
        refused = true;
    }

    THREESF_CHECK(refused);
}

// A destination far past the end of DSP memory wraps around to a word inside it.
void TestDmaDestinationWraps()
{
    Teakra::Teakra teakra(Teakra::UserConfig{});
    constexpr uint32_t kSource = 0x100;
    constexpr uint32_t kDestination = 0x123456;
    StoreLe16(teakra.GetDspMemory() + (kDataMemoryWord + kSource) * 2, 0xBEEF);

    CopyWord(teakra, kSource, kDestination);

    THREESF_CHECK(ReadWord(teakra, (kDataMemoryWord + kDestination) % kMemoryWords) == 0xBEEF);
}

// A source far past the end of DSP memory wraps around too.
void TestDmaSourceWraps()
{
    Teakra::Teakra teakra(Teakra::UserConfig{});
    constexpr uint32_t kSource = 0x7654321;
    constexpr uint32_t kDestination = 0x200;
    StoreLe16(teakra.GetDspMemory() + ((kDataMemoryWord + kSource) % kMemoryWords) * 2, 0xCAFE);

    CopyWord(teakra, kSource, kDestination);

    THREESF_CHECK(ReadWord(teakra, kDataMemoryWord + kDestination) == 0xCAFE);
}

// The DMA channel number wraps around to one of the eight channels.
void TestDmaChannelWraps()
{
    Teakra::Teakra teakra(Teakra::UserConfig{});

    teakra.MMIOWrite(kDmaChannel, 9);

    THREESF_CHECK(teakra.MMIORead(kDmaChannel) == 1);
}

} // namespace

int main()
{
    TestDmaDestinationWraps();
    TestDmaSourceWraps();
    TestDmaChannelWraps();
    TestDwordTransferEnds();
    TestLongTransferRefused();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("teakra bounds tests passed\n");

    return 0;
}
