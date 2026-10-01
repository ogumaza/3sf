// SPDX-License-Identifier: MIT

// The 3DS DSP as the ARM11 sees it: its pipes and interrupts, the layout of DSP RAM and the timing of its audio output.
// TeakraDsp (teakra_dsp.h) is the DSP itself.

#pragma once

#include <cstdint>

namespace threesf::dsp
{

// Pipes of the audio firmware (the pipe number used with the dsp::DSP pipe commands).
enum class Pipe : uint8_t
{
    kDebug = 0, // firmware debug output; the system drains and discards it
    kDma = 1,
    kAudio = 2,  // nn::snd's command and reply pipe, also signalled at the end of each frame
    kBinary = 3, // codec requests (AAC streams); unused by sequenced sound
};

// The events an interrupt from the DSP signals.
enum class Interrupt : uint8_t
{
    kReply0 = 0, // reply register 0 received data
    kReply1 = 1, // reply register 1 received data
    kPipe = 2,   // the firmware notified a pipe
};

// The DSP runs at half the ARM11 clock (268111856 / 2 Hz). The audio firmware outputs one stereo sample every 4096 DSP
// cycles, 160 samples per frame.
constexpr uint64_t kCyclesPerSample = 4096;
constexpr uint32_t kSamplesPerFrame = 160;
constexpr uint64_t kCyclesPerFrame = kCyclesPerSample * kSamplesPerFrame;

// The native output rate, 268111856 / 8192 Hz (about 32728.498 Hz).
constexpr double kSampleRate = 268111856.0 / 8192.0;

// DSP RAM is 0x40000 bytes of program memory followed by 0x40000 bytes of data memory. The DSP addresses both in 16-bit
// words.
constexpr uint32_t kRamSize = 0x80000;
constexpr uint32_t kDataMemoryOffset = 0x40000;

// Byte offset in DSP RAM of data-memory word `word_address`. The ARM11 sees that word at 0x1FF40000 + 2 * word_address
// (dsp::DSP ConvertProcessAddressFromDspDram).
constexpr uint32_t DataOffset(uint32_t word_address)
{
    return kDataMemoryOffset + word_address * 2;
}

} // namespace threesf::dsp
