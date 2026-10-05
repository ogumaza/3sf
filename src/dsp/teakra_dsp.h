// SPDX-License-Identifier: MIT

// The DSP as the ARM11 sees it: Teakra running a DSP1 firmware image, driven by the dsp::DSP service in game mode and
// by the nn::snd model in archive mode. The firmware reads wave data from FCRAM over the DSP's AHBM bus, and talks to
// the ARM11 through three command registers, three reply registers, a semaphore each way and the pipes it keeps in DSP
// RAM.
//
// The DSP runs inside Run, and while the ARM11 side waits for it: for a reply in ReadReply, LoadComponent and
// UnloadComponent, and for command register 2 to empty in ReadPipe, WritePipe and UnloadComponent. Interrupts reach the
// handler from inside those calls. The glue never runs the DSP from inside Teakra: a notification it sends to the
// firmware from there waits between two slices of a run if command register 2 is full.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "common/memory_image.h"
#include "dsp/dsp.h"
#include "dsp/fcram.h"

namespace Teakra
{

class Teakra;

} // namespace Teakra

namespace threesf::dsp
{

class TeakraDsp
{
public:
    using InterruptHandler = std::function<void(Interrupt interrupt, Pipe pipe)>;
    using SampleSink = std::function<void(std::array<int16_t, 2> sample)>;

    // A copy of the DSP's state: Teakra's registers and components, DSP RAM and the glue's state. The handlers stay as
    // they are.
    struct Snapshot
    {
        std::vector<uint8_t> teakra;
        MemoryImage ram;
        uint64_t cycles = 0;
        bool running = false;
        std::optional<uint16_t> pipe_table;
        bool pipe_reply = false;
        bool pipe_semaphore = false;
        std::optional<uint16_t> deferred_notification;
    };

    explicit TeakraDsp(Fcram& fcram); // `fcram` outlives the object
    ~TeakraDsp();

    // Waits for data in reply register `reg` (0 to 2) and reads it, which empties the register.
    uint16_t ReadReply(uint32_t reg);

    // Returns true if reply register `reg` (0 to 2) holds data. Doesn't run the DSP.
    bool ReplyReady(uint32_t reg) const;

    // Sets bits of the ARM11's semaphore towards the DSP.
    void SetSemaphore(uint16_t bits);

    // Returns the number of bytes waiting in the DSP-to-ARM11 ring of `pipe`.
    std::size_t PipeReadable(Pipe pipe) const;

    // Reads `size` bytes from the DSP-to-ARM11 ring of `pipe`, then tells the firmware. Only the low 16 bits of `size`
    // count, as in the dsp::DSP service. Throws if fewer bytes are waiting.
    std::vector<uint8_t> ReadPipe(Pipe pipe, std::size_t size);

    // Writes `data` to the ARM11-to-DSP ring of `pipe`, then tells the firmware. Throws if the ring fills up first.
    void WritePipe(Pipe pipe, std::span<const uint8_t> data);

    // Sets the function that gets the DSP's interrupts while firmware runs: data in reply register 0 or 1 (with
    // Pipe::kDebug) and the firmware's pipe notifications. It's called while the DSP runs, and it may read the reply
    // register or replace itself.
    void SetInterruptHandler(InterruptHandler handler);

    // Sets the function that gets each stereo sample of the DSP's audio output.
    void SetSampleSink(SampleSink sink);

    // Resets the DSP, loads a DSP1 firmware image and runs it until it tells where its pipe table is. Does nothing if
    // firmware is already running. Throws if the image is broken, before touching the DSP, or if the firmware doesn't
    // answer.
    void LoadComponent(std::span<const uint8_t> image);

    // Asks the running firmware to stop and holds the DSP in reset from then on. Does nothing if no firmware runs.
    void UnloadComponent();

    // Returns DSP RAM, kRamSize bytes of program memory and then data memory. It stays the same for the object's
    // lifetime.
    uint8_t* Ram();

    // Notifies Teakra of an external write to program memory in Ram().
    void ProgramWritten();

    // Runs the DSP for `cycles` cycles. While it's held in reset, the cycles only pass.
    void Run(uint64_t cycles);

    // Returns total DSP cycles: those passed to Run plus those run while the ARM11 side waits for the DSP.
    uint64_t Cycles() const;

    // Takes a snapshot, sharing unchanged DSP RAM pages with `previous`. Must not be called during a DSP run.
    Snapshot Save(const Snapshot* previous);

    // Restores a snapshot from this DSP or another. Must not be called during a DSP run.
    void Restore(const Snapshot& snapshot);

private:
    uint64_t RunUntil(const std::function<bool()>& done, uint64_t limit);
    uint64_t WaitForReply(uint8_t reg, uint64_t limit);
    void SendCommand(uint16_t value);
    void SendDeferredNotification();
    uint16_t PipeTable() const;
    void OnReply(uint8_t reg);
    void OnSemaphore();
    void OnPipeNotification();
    void Raise(Interrupt interrupt, Pipe pipe);

    std::unique_ptr<Teakra::Teakra> teakra_;
    InterruptHandler interrupt_handler_;
    uint64_t cycles_ = 0;
    bool running_ = false;               // true from the firmware's start until UnloadComponent
    std::optional<uint16_t> pipe_table_; // word address of the firmware's pipe table in data memory

    // Halves of a pipe notification that have arrived: data in reply register 2 and the pipe bit of the DSP's
    // semaphore. The firmware sends them in either order.
    bool pipe_reply_ = false;
    bool pipe_semaphore_ = false;

    // The notification that tells the firmware the debug pipe has been drained, while it waits for command register 2
    // to empty.
    std::optional<uint16_t> deferred_notification_;
};

} // namespace threesf::dsp
