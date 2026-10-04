// SPDX-License-Identifier: MIT

// The DSP (see teakra_dsp.h).

#include "dsp/teakra_dsp.h"

#include <teakra/teakra.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/bytes.h"
#include "dsp/dsp.h"
#include "dsp/fcram.h"

namespace threesf::dsp
{
namespace
{

// The fields of a DSP1 image's header that matter here (3dbrew, "DSP Binary"). The header and its table of at most 10
// segment records take the first 0x300 bytes.
constexpr std::size_t kHeaderSize = 0x300;
constexpr std::size_t kMagicOffset = 0x100;
constexpr std::size_t kSegmentCountOffset = 0x10E;
constexpr std::size_t kFlagsOffset = 0x10F;
constexpr std::size_t kSegmentTableOffset = 0x120;
constexpr std::size_t kSegmentRecordSize = 0x30;
constexpr std::size_t kSegmentTypeOffset = 15; // in a record, after the offset, word address, size and padding
constexpr unsigned kMaxSegments = 10;

// Header flag: the firmware reports 1 on reply registers 0, 1 and 2 once it runs. Flag bit 1 asks for a special segment
// filled from the console's configuration. That one isn't loaded: 3SF never has, and loading it could change the
// output.
constexpr uint8_t kReportsReady = 0x01;

// Segment memory types: 0 and 1 are program memory, 2 is data memory, and segments of any other type are skipped.
constexpr uint8_t kDataMemoryType = 2;

// The size of program memory, and of data memory.
constexpr uint64_t kMemorySize = kDataMemoryOffset;

// The ARM11 side waits for the DSP by running it in slices of kWaitSlice cycles. A wait gives up once it has run one
// second of DSP time (the DSP runs at half the ARM11's 268,111,856 Hz).
constexpr unsigned kWaitSlice = 16384;
constexpr uint64_t kWaitLimit = 134'055'928;

// Register 2 carries the pipe notifications both ways, the stop request to the firmware and the firmware's answers.
constexpr uint8_t kPipeRegister = 2;
constexpr uint16_t kStopRequest = 0x8000;

// The bit of the DSP's semaphore that goes with a pipe notification in reply register 2.
constexpr uint16_t kPipeSemaphoreBit = 0x8000;

// The pipe table has a 10-byte entry for each of 16 pipes in each direction, entry 2 * pipe + direction. An entry holds
// the ring buffer's word address in data memory, its size in bytes, the read and write positions, the entry's number
// and a flags byte.
constexpr unsigned kPipeCount = 16;
constexpr unsigned kDspToArm = 0;
constexpr unsigned kArmToDsp = 1;
constexpr uint32_t kPipeEntrySize = 10;
constexpr uint32_t kEntryRing = 0;
constexpr uint32_t kEntrySize = 2;
constexpr uint32_t kEntryRead = 4;
constexpr uint32_t kEntryWrite = 6;
constexpr uint32_t kEntryNumber = 8;

// A ring position is a byte offset in bits 0-14 and a lap bit, which flips each time the offset goes back to 0.
constexpr uint16_t kOffsetMask = 0x7FFF;
constexpr uint16_t kLapBit = 0x8000;

// The table address, the ring addresses and the offsets are at most 16 bits wide, so every table entry and ring byte
// they can name is inside DSP RAM.
static_assert(DataOffset(0xFFFF) + 2 * kPipeCount * kPipeEntrySize <= kRamSize);
static_assert(DataOffset(0xFFFF) + kOffsetMask < kRamSize);

// A segment of a DSP1 image and its place in DSP RAM.
struct Segment
{
    std::size_t source;      // byte offset in the image
    std::size_t destination; // byte offset in DSP RAM
    std::size_t size;        // in bytes
};

// Checks a DSP1 image. Returns the segments to copy into DSP RAM, in table order, and throws if the image is broken.
std::vector<Segment> ReadSegments(std::span<const uint8_t> image)
{
    if (image.size() < kHeaderSize || std::memcmp(image.data() + kMagicOffset, "DSP1", 4) != 0)
    {
        throw std::runtime_error("DSP: the firmware isn't a DSP1 image");
    }

    const unsigned count = image[kSegmentCountOffset];
    if (count > kMaxSegments)
    {
        throw std::runtime_error("DSP: the firmware has " + std::to_string(count) +
                                 " segments; at most 10 are supported");
    }

    std::vector<Segment> segments;
    for (unsigned i = 0; i < count; i++)
    {
        const uint8_t* record = image.data() + kSegmentTableOffset + i * kSegmentRecordSize;
        const uint8_t type = record[kSegmentTypeOffset];
        if (type > kDataMemoryType)
        {
            continue;
        }

        const uint64_t offset = LoadLe32(record);
        const uint64_t address = LoadLe32(record + 4);
        const uint64_t size = LoadLe32(record + 8);
        if (offset + size > image.size())
        {
            throw std::runtime_error("DSP: firmware segment " + std::to_string(i) + " lies outside the image");
        }

        if (address * 2 + size > kMemorySize)
        {
            throw std::runtime_error("DSP: firmware segment " + std::to_string(i) + " doesn't fit in " +
                                     (type == kDataMemoryType ? "data" : "program") + " memory");
        }

        const uint64_t memory = type == kDataMemoryType ? kDataMemoryOffset : 0;
        segments.push_back({static_cast<std::size_t>(offset), static_cast<std::size_t>(memory + address * 2),
                            static_cast<std::size_t>(size)});
    }

    return segments;
}

// Throws unless `reg` is the number of a reply register. The number can come from the game's code.
void CheckReplyRegister(uint32_t reg)
{
    if (reg > 2)
    {
        throw std::runtime_error("DSP: there's no reply register " + std::to_string(reg));
    }
}

// A pipe's entry in the firmware's pipe table.
struct PipeEntry
{
    uint32_t offset; // byte offset of the entry in DSP RAM
    uint8_t number;  // 2 * pipe + direction
    uint32_t ring;   // byte offset of the ring buffer in DSP RAM
    uint16_t size;   // ring size in bytes
    uint16_t read;   // read position
    uint16_t write;  // write position
};

// Reads the entry for `pipe` in `direction` from the pipe table at word `table` of data memory. Throws if there's no
// such pipe or the entry is broken.
PipeEntry LoadPipeEntry(const uint8_t* ram, uint16_t table, Pipe pipe, unsigned direction)
{
    const unsigned index = static_cast<unsigned>(pipe);
    if (index >= kPipeCount)
    {
        throw std::runtime_error("DSP: there's no pipe " + std::to_string(index));
    }

    PipeEntry entry;
    entry.number = static_cast<uint8_t>(2 * index + direction);
    entry.offset = DataOffset(table) + entry.number * kPipeEntrySize;
    const uint8_t* fields = ram + entry.offset;
    entry.ring = DataOffset(LoadLe16(fields + kEntryRing));
    entry.size = LoadLe16(fields + kEntrySize);
    entry.read = LoadLe16(fields + kEntryRead);
    entry.write = LoadLe16(fields + kEntryWrite);
    if (fields[kEntryNumber] != entry.number)
    {
        throw std::runtime_error("DSP: the pipe table is broken: entry " + std::to_string(entry.number) +
                                 " has number " + std::to_string(fields[kEntryNumber]));
    }

    if ((entry.read & kOffsetMask) > entry.size || (entry.write & kOffsetMask) > entry.size)
    {
        throw std::runtime_error("DSP: the pipe table is broken: a position in entry " + std::to_string(entry.number) +
                                 " lies beyond the end of its ring");
    }

    return entry;
}

// Advances a ring position by one byte.
uint16_t Advance(uint16_t position, uint16_t size)
{
    const unsigned offset = (position & kOffsetMask) + 1u;
    if (offset >= size)
    {
        return static_cast<uint16_t>((position & kLapBit) ^ kLapBit);
    }

    return static_cast<uint16_t>((position & kLapBit) | (offset & kOffsetMask));
}

// Returns the number of bytes in a ring, from its read position to its write position.
uint16_t BytesInRing(const PipeEntry& entry)
{
    uint16_t bytes = static_cast<uint16_t>(entry.write - entry.read);
    if ((entry.write ^ entry.read) & kLapBit)
    {
        bytes = static_cast<uint16_t>(bytes + entry.size);
    }

    return bytes & kOffsetMask;
}

// Takes `count` bytes from the ring of `entry`, the DSP-to-ARM11 entry of `pipe`, and moves the entry's read position
// past them. Returns the bytes. Throws if the ring holds fewer.
std::vector<uint8_t> TakeFromRing(uint8_t* ram, const PipeEntry& entry, Pipe pipe, uint16_t count)
{
    std::vector<uint8_t> data(count);
    uint16_t read = entry.read;
    for (uint8_t& byte : data)
    {
        if (read == entry.write)
        {
            throw std::runtime_error("DSP: pipe " + std::to_string(static_cast<unsigned>(pipe)) + " holds fewer than " +
                                     std::to_string(count) + " bytes");
        }

        byte = ram[entry.ring + (read & kOffsetMask)];
        read = Advance(read, entry.size);
    }

    StoreLe16(ram + entry.offset + kEntryRead, read);

    return data;
}

} // namespace

TeakraDsp::TeakraDsp(Fcram& fcram) : teakra_(std::make_unique<Teakra::Teakra>(Teakra::UserConfig{}))
{
    // The AHBM bus reaches FCRAM by physical address. An access that doesn't fit inside it reads 0 and writes nothing.
    Teakra::AHBMCallback bus;
    bus.read8 = [&fcram](uint32_t address) -> uint8_t
    {
        const uint8_t* p = fcram.At(address, 1);
        return p ? *p : 0;
    };
    bus.write8 = [&fcram](uint32_t address, uint8_t value)
    {
        if (uint8_t* p = fcram.At(address, 1))
        {
            *p = value;
        }
    };
    bus.read16 = [&fcram](uint32_t address) -> uint16_t
    {
        const uint8_t* p = fcram.At(address, 2);
        return p ? LoadLe16(p) : 0;
    };
    bus.write16 = [&fcram](uint32_t address, uint16_t value)
    {
        if (uint8_t* p = fcram.At(address, 2))
        {
            StoreLe16(p, value);
        }
    };
    bus.read32 = [&fcram](uint32_t address) -> uint32_t
    {
        const uint8_t* p = fcram.At(address, 4);
        return p ? LoadLe32(p) : 0;
    };
    bus.write32 = [&fcram](uint32_t address, uint32_t value)
    {
        if (uint8_t* p = fcram.At(address, 4))
        {
            StoreLe32(p, value);
        }
    };
    teakra_->SetAHBMCallback(bus);

    for (uint8_t reg = 0; reg < 3; reg++)
    {
        teakra_->SetRecvDataHandler(reg, [this, reg] { OnReply(reg); });
    }

    teakra_->SetSemaphoreHandler([this] { OnSemaphore(); });
}

TeakraDsp::~TeakraDsp() = default;

uint16_t TeakraDsp::ReadReply(uint32_t reg)
{
    CheckReplyRegister(reg);
    WaitForReply(static_cast<uint8_t>(reg), kWaitLimit);

    return teakra_->RecvData(static_cast<uint8_t>(reg));
}

bool TeakraDsp::ReplyReady(uint32_t reg) const
{
    CheckReplyRegister(reg);

    return teakra_->RecvDataIsReady(static_cast<uint8_t>(reg));
}

void TeakraDsp::SetSemaphore(uint16_t bits)
{
    teakra_->SetSemaphore(bits);
}

std::size_t TeakraDsp::PipeReadable(Pipe pipe) const
{
    return BytesInRing(LoadPipeEntry(teakra_->GetDspMemory(), PipeTable(), pipe, kDspToArm));
}

std::vector<uint8_t> TeakraDsp::ReadPipe(Pipe pipe, std::size_t size)
{
    const uint16_t count = static_cast<uint16_t>(size); // the dsp::DSP service's sizes are 16-bit
    if (count == 0)
    {
        return {};
    }

    uint8_t* ram = teakra_->GetDspMemory();
    const PipeEntry entry = LoadPipeEntry(ram, PipeTable(), pipe, kDspToArm);
    std::vector<uint8_t> data = TakeFromRing(ram, entry, pipe, count);
    SendCommand(entry.number);

    return data;
}

void TeakraDsp::WritePipe(Pipe pipe, std::span<const uint8_t> data)
{
    if (data.empty())
    {
        return;
    }

    uint8_t* ram = teakra_->GetDspMemory();
    const PipeEntry entry = LoadPipeEntry(ram, PipeTable(), pipe, kArmToDsp);
    uint16_t write = entry.write;
    for (const uint8_t byte : data)
    {
        // The ring is full when the offsets are equal and the lap bits differ.
        if ((write ^ entry.read) == kLapBit)
        {
            throw std::runtime_error("DSP: pipe " + std::to_string(static_cast<unsigned>(pipe)) + " has no room for " +
                                     std::to_string(data.size()) + " bytes");
        }

        ram[entry.ring + (write & kOffsetMask)] = byte;
        write = Advance(write, entry.size);
    }

    StoreLe16(ram + entry.offset + kEntryWrite, write);
    SendCommand(entry.number);
}

void TeakraDsp::SetInterruptHandler(InterruptHandler handler)
{
    interrupt_handler_ = std::move(handler);
}

void TeakraDsp::SetSampleSink(SampleSink sink)
{
    teakra_->SetAudioCallback(std::move(sink));
}

void TeakraDsp::LoadComponent(std::span<const uint8_t> image)
{
    if (running_)
    {
        return;
    }

    const std::vector<Segment> segments = ReadSegments(image);

    teakra_->Reset();
    pipe_table_.reset();
    deferred_notification_.reset();
    uint8_t* ram = teakra_->GetDspMemory();
    for (const Segment& segment : segments)
    {
        std::copy_n(image.data() + segment.source, segment.size, ram + segment.destination);
    }

    if (image[kFlagsOffset] & kReportsReady)
    {
        // The firmware reports 1 on reply registers 0, 1 and 2, in order. Discard other values. All reads from one
        // register share a timeout so unexpected replies cannot keep the player waiting indefinitely.
        for (uint8_t reg = 0; reg < 3; reg++)
        {
            uint64_t time_left = kWaitLimit;
            while (true)
            {
                time_left -= std::min(time_left, WaitForReply(reg, time_left));
                if (teakra_->RecvData(reg) == 1)
                {
                    break;
                }
            }
        }
    }

    WaitForReply(kPipeRegister, kWaitLimit);
    pipe_table_ = teakra_->RecvData(kPipeRegister);

    // Enable interrupt handling after the startup handshake.
    running_ = true;
}

void TeakraDsp::UnloadComponent()
{
    if (!running_)
    {
        return;
    }

    // Ignore further interrupts, including the reply to the stop request.
    running_ = false;
    SendCommand(kStopRequest);
    WaitForReply(kPipeRegister, kWaitLimit);
    teakra_->RecvData(kPipeRegister);
}

uint8_t* TeakraDsp::Ram()
{
    return teakra_->GetDspMemory();
}

void TeakraDsp::ProgramWritten()
{
    teakra_->NotifyProgramWrite();
}

void TeakraDsp::Run(uint64_t cycles)
{
    // Teakra takes an unsigned count, so a longer run takes more than one call. While a notification waits for command
    // register 2, the DSP runs in short slices, so that it goes out soon after the firmware empties the register.
    for (uint64_t left = cycles; running_ && left > 0;)
    {
        const uint64_t most = deferred_notification_ ? kWaitSlice : std::numeric_limits<unsigned>::max();
        const unsigned slice = static_cast<unsigned>(std::min(left, most));
        teakra_->Run(slice);
        left -= slice;
        SendDeferredNotification();
    }

    cycles_ += cycles;
}

uint64_t TeakraDsp::Cycles() const
{
    return cycles_;
}

TeakraDsp::Snapshot TeakraDsp::Save(const Snapshot* previous)
{
    Snapshot snapshot;
    snapshot.teakra = teakra_->SaveState();
    snapshot.ram = MemoryImage(teakra_->GetDspMemory(), kRamSize, previous ? &previous->ram : nullptr);
    snapshot.cycles = cycles_;
    snapshot.running = running_;
    snapshot.pipe_table = pipe_table_;
    snapshot.pipe_reply = pipe_reply_;
    snapshot.pipe_semaphore = pipe_semaphore_;
    snapshot.deferred_notification = deferred_notification_;

    return snapshot;
}

void TeakraDsp::Restore(const Snapshot& snapshot)
{
    if (snapshot.ram.Size() != kRamSize)
    {
        throw std::invalid_argument("DSP: a snapshot without DSP RAM");
    }

    // Teakra checks the code it has translated against the restored program memory.
    teakra_->LoadState(snapshot.teakra);
    snapshot.ram.Restore(teakra_->GetDspMemory());
    cycles_ = snapshot.cycles;
    running_ = snapshot.running;
    pipe_table_ = snapshot.pipe_table;
    pipe_reply_ = snapshot.pipe_reply;
    pipe_semaphore_ = snapshot.pipe_semaphore;
    deferred_notification_ = snapshot.deferred_notification;
}

// Runs the DSP in slices, as the ARM11 side does while it waits for the DSP, until `done` returns true. Throws once it
// has run `limit` cycles and `done` still returns false. Returns the cycles it ran.
uint64_t TeakraDsp::RunUntil(const std::function<bool()>& done, uint64_t limit)
{
    uint64_t ran = 0;
    while (!done())
    {
        if (ran >= limit)
        {
            throw std::runtime_error("DSP: the firmware has stopped answering");
        }

        teakra_->Run(kWaitSlice);
        cycles_ += kWaitSlice;
        ran += kWaitSlice;
        SendDeferredNotification();
    }

    return ran;
}

// Waits for data in reply register `reg`, for at most `limit` cycles. Returns the cycles the DSP ran.
uint64_t TeakraDsp::WaitForReply(uint8_t reg, uint64_t limit)
{
    return RunUntil([this, reg] { return teakra_->RecvDataIsReady(reg); }, limit);
}

// Sends `value` on command register 2 once the firmware has read what's there, and after the notification waiting in
// deferred_notification_, if there is one.
void TeakraDsp::SendCommand(uint16_t value)
{
    RunUntil([this] { return !deferred_notification_ && teakra_->SendDataIsEmpty(kPipeRegister); }, kWaitLimit);
    teakra_->SendData(kPipeRegister, value);
}

// Sends the notification waiting in deferred_notification_ if command register 2 is empty.
void TeakraDsp::SendDeferredNotification()
{
    if (deferred_notification_ && teakra_->SendDataIsEmpty(kPipeRegister))
    {
        teakra_->SendData(kPipeRegister, *deferred_notification_);
        deferred_notification_.reset();
    }
}

// Returns the word address of the pipe table. Throws if no firmware has started since the last reset.
uint16_t TeakraDsp::PipeTable() const
{
    if (!pipe_table_)
    {
        throw std::runtime_error("DSP: no firmware has started, so there are no pipes");
    }

    return *pipe_table_;
}

// Handles data arriving in reply register `reg`.
void TeakraDsp::OnReply(uint8_t reg)
{
    if (!running_)
    {
        return;
    }

    if (reg == kPipeRegister)
    {
        pipe_reply_ = true;
        OnPipeNotification();
    }
    else
    {
        Raise(reg == 0 ? Interrupt::kReply0 : Interrupt::kReply1, Pipe::kDebug);
    }
}

// Handles a change of the DSP's semaphore. Only the pipe bit counts.
void TeakraDsp::OnSemaphore()
{
    if (!running_ || !(teakra_->GetSemaphore() & kPipeSemaphoreBit))
    {
        return;
    }

    pipe_semaphore_ = true;
    OnPipeNotification();
}

// Acts on a pipe notification once both halves have arrived. Reply register 2 holds the number of the entry the
// firmware has moved a position in.
void TeakraDsp::OnPipeNotification()
{
    if (!pipe_reply_ || !pipe_semaphore_)
    {
        return;
    }

    pipe_reply_ = false;
    pipe_semaphore_ = false;
    const uint16_t number = teakra_->RecvData(kPipeRegister);
    const unsigned pipe = number / 2;
    if (pipe >= kPipeCount)
    {
        throw std::runtime_error("DSP: the firmware notified pipe " + std::to_string(pipe) + ", which doesn't exist");
    }

    // The firmware has read from an ARM11-to-DSP ring, which needs no answer.
    if (number % 2 == kArmToDsp)
    {
        return;
    }

    // The system drains the debug pipe itself and throws its contents away. This runs inside Teakra, which can't be run
    // again from here to wait for command register 2 to empty, so if the register still holds a command, the
    // notification that tells the firmware waits in deferred_notification_ until the firmware has read it.
    if (pipe == static_cast<unsigned>(Pipe::kDebug))
    {
        uint8_t* ram = teakra_->GetDspMemory();
        const PipeEntry entry = LoadPipeEntry(ram, PipeTable(), Pipe::kDebug, kDspToArm);
        const uint16_t count = BytesInRing(entry);
        if (count > 0)
        {
            TakeFromRing(ram, entry, Pipe::kDebug, count);
            deferred_notification_ = entry.number;
            SendDeferredNotification();
        }

        return;
    }

    Raise(Interrupt::kPipe, static_cast<Pipe>(pipe));
}

// Passes an interrupt to the handler, if there is one.
void TeakraDsp::Raise(Interrupt interrupt, Pipe pipe)
{
    if (!interrupt_handler_)
    {
        return;
    }

    // Copy the handler because it may replace itself through SetInterruptHandler.
    const InterruptHandler handler = interrupt_handler_;
    handler(interrupt, pipe);
}

} // namespace threesf::dsp
