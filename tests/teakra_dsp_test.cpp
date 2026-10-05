// SPDX-License-Identifier: MIT

// Checks the Teakra glue (src/dsp/teakra_dsp.*) with small firmwares built into the test: its checks on DSP1 images,
// its waits for the DSP and their time limit, the pipe rings, the firmware's pipe notifications and other interrupts,
// and starting and stopping the firmware.
//
// Each firmware's machine code comes after its source in Teak assembly. To assemble a source again, take "// " off the
// start of its lines, from "segment" on, and run it through Teakra's makedsp1, which puts the code at offset 0x300 of
// the DSP1 image it writes.

#include "dsp/teakra_dsp.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "common/bytes.h"
#include "dsp/dsp.h"
#include "dsp/fcram.h"

namespace
{

using threesf::Fcram;
using threesf::LoadLe16;
using threesf::StoreLe16;
using threesf::StoreLe32;
using threesf::dsp::DataOffset;
using threesf::dsp::Interrupt;
using threesf::dsp::Pipe;
using threesf::dsp::TeakraDsp;

// The interrupts a handler got, in order.
using Interrupts = std::vector<std::pair<Interrupt, Pipe>>;

// A wait for the DSP runs it in slices of 16,384 cycles, and gives up once it has run one second of DSP time
// (134,055,928 cycles) without an answer. That takes 8,183 slices.
constexpr uint64_t kWaitSlice = 16384;
constexpr uint64_t kWaitLimitCycles = 8183 * kWaitSlice;
static_assert(kWaitLimitCycles >= 134'055'928 && kWaitLimitCycles - kWaitSlice < 134'055'928);

// The size of a DSP1 header, and the header flag that makes the firmware report 1 on reply registers 0, 1 and 2 when it
// starts.
constexpr std::size_t kHeaderSize = 0x300;
constexpr uint8_t kReportsReady = 0x01;

// A firmware that spins and never answers.
//
// segment p 0000
// br 0x0000$0000 always // spin here and never answer
constexpr uint16_t kSilentFirmware[] = {0x4180, 0x0000};

// A firmware that writes 0 to reply register 0 over and over, and never sends its pipe table's address.
//
// segment p 0000
// mov 0x$80c0 r0 // reply register 0
// mov 0x$0000 r1
// mov r1 [r0] // 0x0004: write 0 to it, again and again
// br 0x0000$0004 always
constexpr uint16_t kZerosFirmware[] = {0x5E00, 0x80C0, 0x5E01, 0x0000, 0x1820, 0x4180, 0x0004};

// A firmware that sends 0x1000 on reply register 2 as its pipe table's word address, then loops. Each time round, it
// copies command register 2 to data word 0x0102. It answers a stop request (0x8000) with 0x0077 on reply register 2 and
// then spins. The test drives it through data words 0x0100 to 0x0104 (see Fire).
//
// segment p 0000
// mov 0x$1000 r1 // the pipe table's word address
// mov 0x$80c8 r0 // reply register 2
// mov r1 [r0]
// load 0x0001u8 page // data words 0x0100 to 0x01ff
// mov 0x$80ca r0 // 0x0006: main loop. Read command register 2, which empties it
// mov [r0] r1
// mov r1 [page:0x0002u8] // keep the value at data word 0x0102
// mov [page:0x0002u8] b0l
// cmpv 0x$8000 b0l // the stop request
// br 0x0000$0023 eq
// mov [page:0x0000u8] b0l // the trigger word, 0x0100
// br 0x0000$0006 eq
// mov [page:0x0004u8] b0l // the reply register's MMIO address, 0x0104, or 0 for none
// br 0x0000$0018 eq
// mov [page:0x0004u8] r0
// mov [page:0x0001u8] r1 // the reply, 0x0101
// mov r1 [r0]
// mov [page:0x0003u8] b0l // 0x0018: the semaphore bits, 0x0103, or 0 for none
// br 0x0000$001f eq
// mov [page:0x0003u8] r1
// mov 0x$80cc r0 // the DSP's semaphore
// mov r1 [r0]
// clr b0 always // 0x001f: clear the trigger
// mov b0l [page:0x0000u8]
// br 0x0000$0006 always
// mov 0x$0077 r1 // 0x0023: answer the stop request on reply register 2
// mov 0x$80c8 r0
// mov r1 [r0]
// br 0x0000$0028 always // 0x0028: then spin
constexpr uint16_t kNotifyFirmware[] = {
    0x5E01, 0x1000, 0x5E00, 0x80C8, 0x1820, 0x0401, 0x5E00, 0x80CA, 0x1C20, 0x2202, 0x6202, 0x8DF2, 0x8000, 0x4181,
    0x0023, 0x6200, 0x4181, 0x0006, 0x6204, 0x4181, 0x0018, 0x6004, 0x6401, 0x1820, 0x6203, 0x4181, 0x001F, 0x6403,
    0x5E00, 0x80CC, 0x1820, 0x6F60, 0x3000, 0x4180, 0x0006, 0x5E01, 0x0077, 0x5E00, 0x80C8, 0x1820, 0x4180, 0x0028,
};

// kNotifyFirmware's pipe table, and the data words the test drives it through.
constexpr uint32_t kPipeTable = 0x1000;
constexpr uint32_t kTriggerWord = 0x0100;   // set to 1 to make the firmware act, and cleared when it's done
constexpr uint32_t kReplyWord = 0x0101;     // the value it writes to the reply register
constexpr uint32_t kCommandWord = 0x0102;   // the last value it read from command register 2
constexpr uint32_t kSemaphoreWord = 0x0103; // the bits it then sets in its semaphore, or 0 for none
constexpr uint32_t kRegisterWord = 0x0104;  // the reply register's MMIO address, or 0 for none

// A firmware that sends 0x1000 on reply register 2 as its pipe table's word address, waits for a command on command
// register 2 without reading it, and then notifies the debug pipe's entry 0 while the command is still there. Once the
// trigger word is set, it reads command register 2 three times, waiting for each command, and keeps what it read in
// data words 0x0101 to 0x0103. Then it spins.
//
// segment p 0000
// mov 0x$1000 r1 // the pipe table's word address
// mov 0x$80c8 r0 // reply register 2
// mov r1 [r0]
// mov 0x$80d6 r0 // the status register
// tstb [r0] 0x000d // 0x0007: wait for a command on command register 2 (bit 13), and leave it there
// br 0x0000$0007 neq
// mov 0x$0000 r1 // notify entry 0, the debug pipe from the DSP
// mov 0x$80c8 r0
// mov r1 [r0]
// mov 0x$8000 r1 // with the pipe bit of the semaphore
// mov 0x$80cc r0
// mov r1 [r0]
// load 0x0001u8 page // data words 0x0100 to 0x01ff
// mov [page:0x0000u8] b0l // 0x0015: wait for the trigger word, 0x0100
// br 0x0000$0015 eq
// mov 0x$80ca r0 // read command register 2, which empties it
// mov [r0] r1
// mov r1 [page:0x0001u8] // keep the value at data word 0x0101
// mov 0x$80d6 r0
// tstb [r0] 0x000d // 0x001e: wait for the next command
// br 0x0000$001e neq
// mov 0x$80ca r0
// mov [r0] r1
// mov r1 [page:0x0002u8] // at 0x0102
// mov 0x$80d6 r0
// tstb [r0] 0x000d // 0x0027: and for the one after that
// br 0x0000$0027 neq
// mov 0x$80ca r0
// mov [r0] r1
// mov r1 [page:0x0003u8] // at 0x0103
// br 0x0000$002e always // 0x002e: spin
constexpr uint16_t kDebugPipeFirmware[] = {
    0x5E01, 0x1000, 0x5E00, 0x80C8, 0x1820, 0x5E00, 0x80D6, 0x9D20, 0x4182, 0x0007, 0x5E01, 0x0000,
    0x5E00, 0x80C8, 0x1820, 0x5E01, 0x8000, 0x5E00, 0x80CC, 0x1820, 0x0401, 0x6200, 0x4181, 0x0015,
    0x5E00, 0x80CA, 0x1C20, 0x2201, 0x5E00, 0x80D6, 0x9D20, 0x4182, 0x001E, 0x5E00, 0x80CA, 0x1C20,
    0x2202, 0x5E00, 0x80D6, 0x9D20, 0x4182, 0x0027, 0x5E00, 0x80CA, 0x1C20, 0x2203, 0x4180, 0x002E,
};

// The first of the data words, 0x0101 to 0x0103, where kDebugPipeFirmware keeps the commands it reads.
constexpr uint32_t kFirstCommandWord = 0x0101;

// A firmware that fills the end of program memory exactly, and sends 0x1000 on reply register 2 from there. The DSP
// gets there through the empty program memory before it, whose zeros are nop instructions.
//
// segment p 1fff9
// mov 0x$1000 r1 // the pipe table's word address
// mov 0x$80c8 r0 // reply register 2
// mov r1 [r0]
// br 0x0001$fffe always // 0x1fffe: spin
constexpr uint16_t kEndFirmware[] = {0x5E01, 0x1000, 0x5E00, 0x80C8, 0x1820, 0x4190, 0xFFFE};
constexpr uint32_t kEndFirmwareAddress = 0x1FFF9;
static_assert(2 * kEndFirmwareAddress + sizeof(kEndFirmware) == 0x40000);

// Returns a DSP1 image that loads `program` into program memory at word address `address`: a header with the magic, the
// flags and one segment record, then the code. The glue doesn't check the signature or the segment's SHA-256, so
// they're left as zeros.
std::vector<uint8_t> MakeImage(std::span<const uint16_t> program, uint8_t flags = 0, uint32_t address = 0)
{
    std::vector<uint8_t> image(kHeaderSize + program.size() * 2);
    std::memcpy(image.data() + 0x100, "DSP1", 4);
    image[0x10E] = 1; // the number of segments
    image[0x10F] = flags;

    // The segment record: the code's offset in the image, its word address, its size in bytes and its memory type (0,
    // program memory).
    uint8_t* record = image.data() + 0x120;
    StoreLe32(record, static_cast<uint32_t>(kHeaderSize));
    StoreLe32(record + 4, address);
    StoreLe32(record + 8, static_cast<uint32_t>(program.size() * 2));
    record[15] = 0;

    for (std::size_t i = 0; i < program.size(); i++)
    {
        StoreLe16(image.data() + kHeaderSize + i * 2, program[i]);
    }

    return image;
}

// Returns the message of the std::runtime_error that `f` throws, or an empty string if it throws none.
std::string ErrorOf(const std::function<void()>& f)
{
    try
    {
        f();
    }
    catch (const std::runtime_error& e)
    {
        return e.what();
    }

    return "";
}

// Returns true if `f` throws a std::runtime_error with one of the glue's messages, which start with "DSP: ".
bool ThrowsDspError(const std::function<void()>& f)
{
    return ErrorOf(f).starts_with("DSP: ");
}

// Returns word `word` of data memory.
uint16_t Word(TeakraDsp& dsp, uint32_t word)
{
    return LoadLe16(dsp.Ram() + DataOffset(word));
}

// Sets word `word` of data memory.
void SetWord(TeakraDsp& dsp, uint32_t word, uint16_t value)
{
    StoreLe16(dsp.Ram() + DataOffset(word), value);
}

// Returns the bytes of entry `number` of kNotifyFirmware's pipe table.
uint8_t* Entry(TeakraDsp& dsp, unsigned number)
{
    return dsp.Ram() + DataOffset(kPipeTable) + number * 10;
}

// Sets the ring of an entry (its word address and size in bytes) and the entry's read and write positions.
void SetEntry(TeakraDsp& dsp, unsigned number, uint16_t ring, uint16_t size, uint16_t read, uint16_t write)
{
    uint8_t* entry = Entry(dsp, number);
    StoreLe16(entry, ring);
    StoreLe16(entry + 2, size);
    StoreLe16(entry + 4, read);
    StoreLe16(entry + 6, write);
}

// Returns the read position of an entry.
uint16_t ReadPosition(TeakraDsp& dsp, unsigned number)
{
    return LoadLe16(Entry(dsp, number) + 4);
}

// Returns the write position of an entry.
uint16_t WritePosition(TeakraDsp& dsp, unsigned number)
{
    return LoadLe16(Entry(dsp, number) + 6);
}

// Starts kNotifyFirmware and numbers the entries of its pipe table, as a real firmware would. The table is all zeros
// otherwise, since the start clears DSP RAM.
void StartNotifyFirmware(TeakraDsp& dsp)
{
    dsp.LoadComponent(MakeImage(kNotifyFirmware));
    for (unsigned number = 0; number < 32; number++)
    {
        Entry(dsp, number)[8] = static_cast<uint8_t>(number);
    }
}

// Makes kNotifyFirmware write `value` to reply register `reg` (none if `reg` is negative) and then set `semaphore` in
// its semaphore (none if it's 0), and runs the DSP until the firmware has done it.
void Fire(TeakraDsp& dsp, int reg, uint16_t value, uint16_t semaphore)
{
    SetWord(dsp, kReplyWord, value);
    SetWord(dsp, kSemaphoreWord, semaphore);
    SetWord(dsp, kRegisterWord, static_cast<uint16_t>(reg < 0 ? 0 : 0x80C0 + 4 * reg));
    SetWord(dsp, kTriggerWord, 1);
    for (int i = 0; i < 100 && Word(dsp, kTriggerWord) != 0; i++)
    {
        dsp.Run(256);
    }

    THREESF_CHECK(Word(dsp, kTriggerWord) == 0);
}

// Broken DSP1 images are refused before the DSP is touched, and records of an unknown memory type are skipped.
void TestImages()
{
    const std::vector<uint8_t> good = MakeImage(kNotifyFirmware);
    Fcram fcram(1 << 20);
    TeakraDsp dsp(fcram);

    // A mark in DSP RAM, which a start would clear.
    dsp.Ram()[0x100] = 0xAB;

    // An image too short for its header, and one with the wrong magic.
    std::vector<uint8_t> image(good.data(), good.data() + kHeaderSize - 1);
    THREESF_CHECK(ThrowsDspError([&] { dsp.LoadComponent(image); }));
    image = good;
    image[0x103] = 'X';
    THREESF_CHECK(ThrowsDspError([&] { dsp.LoadComponent(image); }));

    // More than 10 segments.
    image = good;
    image[0x10E] = 11;
    THREESF_CHECK(ThrowsDspError([&] { dsp.LoadComponent(image); }));

    // A segment that runs past the end of the image, and one whose offset and size add up to more than 32 bits hold.
    image = good;
    StoreLe32(image.data() + 0x128, static_cast<uint32_t>(image.size()));
    THREESF_CHECK(ThrowsDspError([&] { dsp.LoadComponent(image); }));
    image = good;
    StoreLe32(image.data() + 0x120, 0xFFFFFFFF);
    THREESF_CHECK(ThrowsDspError([&] { dsp.LoadComponent(image); }));

    // A program segment that doesn't fit in program memory, and a data segment that doesn't fit in data memory.
    image = good;
    StoreLe32(image.data() + 0x124, 0x1FFFF);
    THREESF_CHECK(ThrowsDspError([&] { dsp.LoadComponent(image); }));
    image = good;
    image[0x12F] = 2;
    StoreLe32(image.data() + 0x124, 0xFFFFFFF0);
    THREESF_CHECK(ThrowsDspError([&] { dsp.LoadComponent(image); }));

    THREESF_CHECK(dsp.Ram()[0x100] == 0xAB);
    THREESF_CHECK(dsp.Cycles() == 0);

    // Records 1 and 2 are empty program segments, which are fine, and record 3 has an unknown memory type and nonsense
    // in every field, so it's skipped. The firmware then starts within one wait slice.
    image = good;
    image[0x10E] = 4;
    uint8_t* record = image.data() + 0x120 + 3 * 0x30;
    StoreLe32(record, 0xFFFFFFF0);
    StoreLe32(record + 4, 0xFFFFFFF0);
    StoreLe32(record + 8, 0xFFFFFFF0);
    record[15] = 3;
    THREESF_CHECK(ErrorOf([&] { dsp.LoadComponent(image); }).empty());
    THREESF_CHECK(dsp.Cycles() == kWaitSlice);
}

// A segment that ends exactly at the end of program memory is loaded there.
void TestSegmentFillsProgramMemory()
{
    Fcram fcram(1 << 20);
    TeakraDsp dsp(fcram);

    THREESF_CHECK(ErrorOf([&] { dsp.LoadComponent(MakeImage(kEndFirmware, 0, kEndFirmwareAddress)); }).empty());
    THREESF_CHECK(LoadLe16(dsp.Ram() + 0x3FFFE) == kEndFirmware[std::size(kEndFirmware) - 1]);
}

// Reply register numbers are checked, and the pipes need a firmware that has started. Nothing here runs the DSP.
void TestWithoutFirmware()
{
    Fcram fcram(1 << 20);
    TeakraDsp dsp(fcram);

    THREESF_CHECK(ThrowsDspError([&] { dsp.ReadReply(3); }));
    THREESF_CHECK(ThrowsDspError([&] { dsp.ReplyReady(3); }));
    THREESF_CHECK(ThrowsDspError([&] { dsp.ReadReply(0xFFFFFFFF); }));
    THREESF_CHECK(!dsp.ReplyReady(0));
    THREESF_CHECK(dsp.Cycles() == 0);

    const uint8_t kByte[1] = {1};
    THREESF_CHECK(ThrowsDspError([&] { dsp.PipeReadable(Pipe::kAudio); }));
    THREESF_CHECK(ThrowsDspError([&] { dsp.ReadPipe(Pipe::kAudio, 1); }));
    THREESF_CHECK(ThrowsDspError([&] { dsp.WritePipe(Pipe::kAudio, kByte); }));

    // Reading or writing nothing does nothing, so it needs no firmware.
    THREESF_CHECK(ErrorOf([&] { THREESF_CHECK(dsp.ReadPipe(Pipe::kAudio, 0).empty()); }).empty());
    THREESF_CHECK(ErrorOf([&] { dsp.WritePipe(Pipe::kAudio, {}); }).empty());

    // With no firmware, UnloadComponent does nothing, and Run only counts the cycles.
    dsp.UnloadComponent();
    THREESF_CHECK(dsp.Cycles() == 0);
    dsp.Run(5000);
    THREESF_CHECK(dsp.Cycles() == 5000);
}

// Reads and writes on the two rings of pipe 2, as the dsp::DSP service makes them.
void TestPipeRings()
{
    Fcram fcram(1 << 20);
    TeakraDsp dsp(fcram);
    StartNotifyFirmware(dsp);

    // The DSP-to-ARM11 ring (entry 4): 8 bytes at data word 0x2000, with the read position at 6 and the write position
    // at 2 on the next lap, so 4 bytes are waiting.
    SetEntry(dsp, 4, 0x2000, 8, 6, 0x8002);
    uint8_t* from_dsp = dsp.Ram() + DataOffset(0x2000);
    for (int i = 0; i < 8; i++)
    {
        from_dsp[i] = static_cast<uint8_t>(0x10 + i);
    }

    THREESF_CHECK(dsp.PipeReadable(Pipe::kAudio) == 4);
    THREESF_CHECK((dsp.ReadPipe(Pipe::kAudio, 3) == std::vector<uint8_t>{0x16, 0x17, 0x10}));
    THREESF_CHECK(ReadPosition(dsp, 4) == 0x8001);
    THREESF_CHECK(dsp.PipeReadable(Pipe::kAudio) == 1);

    // Reading more than is waiting fails and leaves the read position alone. Only the low 16 bits of a size count.
    THREESF_CHECK(ThrowsDspError([&] { dsp.ReadPipe(Pipe::kAudio, 2); }));
    THREESF_CHECK(ReadPosition(dsp, 4) == 0x8001);
    THREESF_CHECK(dsp.ReadPipe(Pipe::kAudio, 0x10000).empty());
    THREESF_CHECK(ReadPosition(dsp, 4) == 0x8001);
    THREESF_CHECK((dsp.ReadPipe(Pipe::kAudio, 0x10001) == std::vector<uint8_t>{0x11}));
    THREESF_CHECK(dsp.PipeReadable(Pipe::kAudio) == 0);
    THREESF_CHECK(ReadPosition(dsp, 4) == 0x8002);

    // A full ring holds as many bytes as its size.
    SetEntry(dsp, 4, 0x2000, 8, 0x0005, 0x8005);
    THREESF_CHECK(dsp.PipeReadable(Pipe::kAudio) == 8);

    // The ARM11-to-DSP ring (entry 5): 8 bytes at data word 0x2100, with the read position at 3 and the write position
    // at 1 on the next lap, so there's room for 2 bytes.
    SetEntry(dsp, 5, 0x2100, 8, 3, 0x8001);
    uint8_t* to_dsp = dsp.Ram() + DataOffset(0x2100);
    const uint8_t kTwo[2] = {0xA0, 0xA1};
    dsp.WritePipe(Pipe::kAudio, kTwo);
    THREESF_CHECK(to_dsp[1] == 0xA0 && to_dsp[2] == 0xA1);
    THREESF_CHECK(WritePosition(dsp, 5) == 0x8003);

    // Writing to the full ring fails and leaves the write position alone.
    const uint8_t kOne[1] = {0xB0};
    THREESF_CHECK(ThrowsDspError([&] { dsp.WritePipe(Pipe::kAudio, kOne); }));
    THREESF_CHECK(WritePosition(dsp, 5) == 0x8003);

    // A write goes back to the start of the ring at its end, and the lap bit flips.
    SetEntry(dsp, 5, 0x2100, 8, 0x8006, 0x8006);
    const uint8_t kFour[4] = {1, 2, 3, 4};
    dsp.WritePipe(Pipe::kAudio, kFour);
    THREESF_CHECK(to_dsp[6] == 1 && to_dsp[7] == 2 && to_dsp[0] == 3 && to_dsp[1] == 4);
    THREESF_CHECK(WritePosition(dsp, 5) == 0x0002);

    // A read from the other ring, so that the firmware reads what that write sent.
    THREESF_CHECK(dsp.ReadPipe(Pipe::kAudio, 1).size() == 1);

    // Each read and write that moved a position sent the entry's number on command register 2. The DSP doesn't run in
    // between, so each send after the first found the last one unread and waited one slice for the firmware to read it.
    // The firmware has read the last write's 5 by now.
    THREESF_CHECK(dsp.Cycles() == 5 * kWaitSlice);
    THREESF_CHECK(Word(dsp, kCommandWord) == 5);
}

// A pipe table entry with the wrong number, or with a position beyond the end of its ring, is an error. A position at
// the end of the ring is allowed, and pipes go up to 15.
void TestBrokenPipeTable()
{
    Fcram fcram(1 << 20);
    TeakraDsp dsp(fcram);
    StartNotifyFirmware(dsp);

    SetEntry(dsp, 4, 0x2000, 8, 0, 0);
    Entry(dsp, 4)[8] = 99;
    THREESF_CHECK(ThrowsDspError([&] { dsp.PipeReadable(Pipe::kAudio); }));
    Entry(dsp, 4)[8] = 4;
    SetEntry(dsp, 4, 0x2000, 8, 9, 0);
    THREESF_CHECK(ThrowsDspError([&] { dsp.PipeReadable(Pipe::kAudio); }));
    SetEntry(dsp, 4, 0x2000, 8, 0, 9);
    THREESF_CHECK(ThrowsDspError([&] { dsp.PipeReadable(Pipe::kAudio); }));
    SetEntry(dsp, 4, 0x2000, 8, 0x8008, 0x8008);
    THREESF_CHECK(ErrorOf([&] { dsp.PipeReadable(Pipe::kAudio); }).empty());

    THREESF_CHECK(ThrowsDspError([&] { dsp.PipeReadable(static_cast<Pipe>(16)); }));
    THREESF_CHECK(dsp.PipeReadable(static_cast<Pipe>(15)) == 0);
}

// The firmware's pipe notifications (data in reply register 2 plus the pipe bit of its semaphore, in either order) and
// the interrupts for reply registers 0 and 1.
void TestNotifications()
{
    Fcram fcram(1 << 20);
    TeakraDsp dsp(fcram);
    Interrupts interrupts;
    uint64_t reply0_wait = 1; // the cycles the DSP ran while the handler read reply register 0
    const auto on_interrupt = [&](Interrupt interrupt, Pipe pipe)
    {
        interrupts.push_back({interrupt, pipe});
        if (interrupt == Interrupt::kReply0)
        {
            const uint64_t before = dsp.Cycles();
            THREESF_CHECK(dsp.ReadReply(0) == 0x1234);
            reply0_wait = dsp.Cycles() - before;
        }
    };
    dsp.SetInterruptHandler(on_interrupt);

    // The start takes one slice, and the firmware's reply with the pipe table's address raises nothing. A second
    // LoadComponent while the firmware runs does nothing.
    StartNotifyFirmware(dsp);
    THREESF_CHECK(dsp.Cycles() == kWaitSlice);
    dsp.LoadComponent(MakeImage(kNotifyFirmware));
    THREESF_CHECK(dsp.Cycles() == kWaitSlice);
    THREESF_CHECK(interrupts.empty());

    // Data in reply register 2 alone raises nothing, and neither does a change of the semaphore without the pipe bit
    // (bit 15).
    Fire(dsp, 2, 4, 0);
    Fire(dsp, -1, 0, 0x0001);
    THREESF_CHECK(interrupts.empty());

    // The reply half stays pending while more data arrives, and the pipe bit completes the notification with the latest
    // data: entry 6, pipe 3 from the DSP.
    Fire(dsp, 2, 6, 0);
    THREESF_CHECK(interrupts.empty());
    Fire(dsp, -1, 0, 0x8000);
    THREESF_CHECK((interrupts == Interrupts{{Interrupt::kPipe, Pipe::kBinary}}));
    THREESF_CHECK(!dsp.ReplyReady(2));

    // An entry of an ARM11-to-DSP ring (an odd number) raises nothing. Only the ARM11 side clears the DSP's semaphore,
    // so the pipe bit stays set from now on, and any change of the semaphore completes a notification.
    interrupts.clear();
    Fire(dsp, 2, 5, 0x0001);
    THREESF_CHECK(interrupts.empty());
    THREESF_CHECK(!dsp.ReplyReady(2));

    // The debug pipe (entry 0) is drained and its bytes dropped, with no interrupt. The drain tells the firmware, so
    // command register 2 goes from the 5 of a write to pipe 2 to the debug entry's 0.
    SetEntry(dsp, 0, 0x2200, 16, 2, 5);
    SetEntry(dsp, 5, 0x2300, 16, 0, 0);
    const uint8_t kByte[1] = {0xC0};
    dsp.WritePipe(Pipe::kAudio, kByte);
    dsp.Run(1024);
    THREESF_CHECK(Word(dsp, kCommandWord) == 5);
    Fire(dsp, 2, 0, 0x0001);
    dsp.Run(1024);
    THREESF_CHECK(interrupts.empty());
    THREESF_CHECK(ReadPosition(dsp, 0) == 5);
    THREESF_CHECK(dsp.PipeReadable(Pipe::kDebug) == 0);
    THREESF_CHECK(Word(dsp, kCommandWord) == 0);

    // With nothing waiting in the debug pipe, the drain doesn't write the table or tell the firmware.
    dsp.WritePipe(Pipe::kAudio, kByte);
    dsp.Run(1024);
    Fire(dsp, 2, 0, 0x0001);
    dsp.Run(1024);
    THREESF_CHECK(interrupts.empty());
    THREESF_CHECK(ReadPosition(dsp, 0) == 5);
    THREESF_CHECK(Word(dsp, kCommandWord) == 5);

    // Data in reply register 0 raises kReply0 with the debug pipe, and the handler reads it without the DSP running.
    // Reply register 1 raises kReply1, and its data waits to be read.
    interrupts.clear();
    Fire(dsp, 0, 0x1234, 0);
    THREESF_CHECK((interrupts == Interrupts{{Interrupt::kReply0, Pipe::kDebug}}));
    THREESF_CHECK(reply0_wait == 0);
    interrupts.clear();
    Fire(dsp, 1, 0x55, 0);
    THREESF_CHECK((interrupts == Interrupts{{Interrupt::kReply1, Pipe::kDebug}}));
    THREESF_CHECK(dsp.ReplyReady(1));
    THREESF_CHECK(dsp.ReadReply(1) == 0x55);

    // The handler can replace itself while it runs. The glue calls a copy of it, so the captures of the old handler
    // (here a long string, which std::function keeps on the heap) last until it returns.
    std::string seen;
    const std::string text(1000, 'x');
    const auto replace_itself = [&dsp, &seen, text](Interrupt, Pipe)
    {
        dsp.SetInterruptHandler([&seen](Interrupt, Pipe) { seen += "second;"; });
        seen += text.substr(0, 5) + ";";
    };
    dsp.SetInterruptHandler(replace_itself);
    Fire(dsp, 2, 4, 0x0001);
    Fire(dsp, 2, 4, 0x0001);
    THREESF_CHECK(seen == "xxxxx;second;");

    // A notification for a pipe above 15 is an error.
    THREESF_CHECK(ThrowsDspError([&] { Fire(dsp, 2, 40, 0x0001); }));
}

// The debug pipe is drained from inside Teakra, where the glue can't run the DSP to wait for command register 2 to
// empty. When the register still holds a command, the drain's notification to the firmware waits, and goes out once the
// firmware has read the command, ahead of the next command from the ARM11 side.
void TestDebugPipeWhileCommandWaits()
{
    Fcram fcram(1 << 20);
    TeakraDsp dsp(fcram);
    dsp.LoadComponent(MakeImage(kDebugPipeFirmware));
    for (unsigned number = 0; number < 32; number++)
    {
        Entry(dsp, number)[8] = static_cast<uint8_t>(number);
    }

    for (uint32_t word = kFirstCommandWord; word < kFirstCommandWord + 3; word++)
    {
        SetWord(dsp, word, 0xFFFF);
    }

    // Three bytes of debug output wait in entry 0's ring. Entry 5 is the ring from the ARM11 side to pipe 2, and a
    // write to it leaves its number, 5, in command register 2, which the firmware answers with the debug notification.
    SetEntry(dsp, 0, 0x2200, 16, 2, 5);
    SetEntry(dsp, 5, 0x2300, 16, 0, 0);
    const uint8_t kByte[1] = {0xC0};
    dsp.WritePipe(Pipe::kAudio, kByte);
    THREESF_CHECK(ErrorOf([&] { dsp.Run(100000); }).empty());
    THREESF_CHECK(ReadPosition(dsp, 0) == 5);

    // Once triggered, the firmware reads the 5, then the debug entry's 0, then the 5 of the next write.
    SetWord(dsp, kTriggerWord, 1);
    dsp.WritePipe(Pipe::kAudio, kByte);
    dsp.Run(100000);
    THREESF_CHECK(Word(dsp, kFirstCommandWord) == 5);
    THREESF_CHECK(Word(dsp, kFirstCommandWord + 1) == 0);
    THREESF_CHECK(Word(dsp, kFirstCommandWord + 2) == 5);
}

// UnloadComponent sends the stop request and reads the firmware's answer, and a pending half of a notification survives
// both it and the next start.
void TestUnload()
{
    Fcram fcram(1 << 20);
    TeakraDsp dsp(fcram);
    Interrupts interrupts;
    dsp.SetInterruptHandler([&](Interrupt interrupt, Pipe pipe) { interrupts.push_back({interrupt, pipe}); });
    StartNotifyFirmware(dsp);

    // The answer comes within one slice, and it's read and dropped without an interrupt.
    uint64_t before = dsp.Cycles();
    THREESF_CHECK(ErrorOf([&] { dsp.UnloadComponent(); }).empty());
    THREESF_CHECK(dsp.Cycles() - before == kWaitSlice);
    THREESF_CHECK(Word(dsp, kCommandWord) == 0x8000);
    THREESF_CHECK(!dsp.ReplyReady(2));
    THREESF_CHECK(interrupts.empty());

    // Unloading again does nothing, and Run only counts the cycles while no firmware runs.
    before = dsp.Cycles();
    dsp.UnloadComponent();
    THREESF_CHECK(dsp.Cycles() == before);
    dsp.Run(100000);
    THREESF_CHECK(dsp.Cycles() == before + 100000);

    // Leave the reply half of a notification pending. The next unload takes that unread data in reply register 2 as the
    // firmware's answer, so it runs nothing.
    StartNotifyFirmware(dsp);
    Fire(dsp, 2, 4, 0);
    THREESF_CHECK(interrupts.empty());
    before = dsp.Cycles();
    dsp.UnloadComponent();
    THREESF_CHECK(dsp.Cycles() == before);

    // After another start, the semaphore half alone completes the pending notification. Reply register 2 still holds
    // 0x1000 from the start, which names pipe 0x800, so that's an error.
    StartNotifyFirmware(dsp);
    THREESF_CHECK(ThrowsDspError([&] { Fire(dsp, -1, 0, 0x8000); }));
}

// A firmware that never finishes starting makes LoadComponent give up after exactly one wait's time limit, whether it
// stays silent or keeps sending the wrong value.
void TestStartTimeouts()
{
    struct Case
    {
        std::span<const uint16_t> program;
        uint8_t flags;
    };
    const Case kCases[] = {
        {kSilentFirmware, 0},             // never sends the pipe table's address
        {kSilentFirmware, kReportsReady}, // never reports on reply register 0
        {kZerosFirmware, 0},              // sends 0 on reply register 0, and never the pipe table's address
        {kZerosFirmware, kReportsReady},  // keeps sending 0 where the start waits for 1
    };
    for (const Case& c : kCases)
    {
        Fcram fcram(1 << 20);
        TeakraDsp dsp(fcram);

        THREESF_CHECK(ThrowsDspError([&] { dsp.LoadComponent(MakeImage(c.program, c.flags)); }));
        THREESF_CHECK(dsp.Cycles() == kWaitLimitCycles);

        // The firmware never counted as running, so Run only counts the cycles.
        dsp.Run(1000);
        THREESF_CHECK(dsp.Cycles() == kWaitLimitCycles + 1000);
    }
}

} // namespace

int main()
{
    TestImages();
    TestSegmentFillsProgramMemory();
    TestWithoutFirmware();
    TestPipeRings();
    TestBrokenPipeTable();
    TestNotifications();
    TestDebugPipeWhileCommandWaits();
    TestUnload();
    TestStartTimeouts();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("teakra glue tests passed\n");

    return 0;
}
