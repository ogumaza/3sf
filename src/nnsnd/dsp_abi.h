// SPDX-License-Identifier: MIT

// The audio firmware's shared-memory structures as nn::snd reads and writes them.
//
// The firmware keeps two copies of every structure (two regions, used alternately frame by frame) and reports where the
// region-0 copies are through the audio pipe (DspLink::Initialize). The layouts below are what Pokemon X's nn::snd
// accesses (code.bin addresses in voice.cpp and sound_system.cpp). The DSP is word-addressed and stores 32-bit integers
// high half first; floats are plain little-endian.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace threesf::nnsnd
{

static_assert(std::endian::native == std::endian::little, "these layouts are copied to and from DSP RAM as they are");

constexpr int kNumSources = 24;

// A 32-bit integer in DSP memory: the high 16 bits come first.
class DspU32
{
public:
    DspU32& operator=(uint32_t value)
    {
        high_ = static_cast<uint16_t>(value >> 16);
        low_ = static_cast<uint16_t>(value);

        return *this;
    }

    operator uint32_t() const
    {
        return (static_cast<uint32_t>(high_) << 16) | low_;
    }

private:
    uint16_t high_;
    uint16_t low_;
};

static_assert(sizeof(DspU32) == 4);

// One of the four queued wave buffers that follow a source's first buffer.
struct QueuedBuffer
{
    DspU32 address;        // physical address of the samples
    DspU32 length;         // in samples
    uint16_t adpcm_ps;     // DSP-ADPCM predictor and scale at the start
    uint16_t adpcm_yn[2];  // DSP-ADPCM history at the start
    uint8_t adpcm_context; // 1: adpcm_ps and adpcm_yn are valid
    uint8_t looping;
    uint16_t buffer_id;
    uint16_t unused;
};

static_assert(sizeof(QueuedBuffer) == 20);

// A source's (voice's) parameters, written by the ARM11. `dirty` says which fields changed; the DSP clears it when it
// takes them.
struct SourceParams
{
    void SetAdpcmContext(bool valid)
    {
        buffer_flags = static_cast<uint16_t>((buffer_flags & ~1u) | (valid ? 1u : 0u));
    }

    void SetLooping(bool looping)
    {
        buffer_flags = static_cast<uint16_t>((buffer_flags & ~2u) | (looping ? 2u : 0u));
    }

    uint32_t dirty;
    float gain[3][4];         // [main, aux A, aux B][front left, front right, rear left, rear right]
    float rate;               // samples consumed per output sample
    uint8_t interpolation;    // 0 polyphase, 1 linear, 2 none
    uint8_t polyphase_kernel; // the polyphase filter; nn::snd derives it from the rate
    uint16_t filters;         // bit 0 one-pole filter, bit 1 biquad
    int16_t one_pole_b0;
    int16_t one_pole_a1;
    int16_t biquad_a2;
    int16_t biquad_a1;
    int16_t biquad_b2;
    int16_t biquad_b1;
    int16_t biquad_b0;
    uint16_t queue_dirty; // bit i: queue[i] changed
    QueuedBuffer queue[4];
    DspU32 unknown_9c;
    uint8_t enable;
    uint8_t unused_a1;
    uint16_t sync;        // echoed back in SourceReport::sync once the DSP has seen it
    DspU32 play_position; // starting offset (samples) into the first buffer
    uint8_t unused_a8[4];

    // The first buffer.
    DspU32 address;
    DspU32 length;
    uint16_t format; // bits 0-1 channels (1 mono, 2 stereo), 2-3 encoding (0 PCM8, 1 PCM16, 2 ADPCM), 4 front bypass
    uint16_t adpcm_ps;
    uint16_t adpcm_yn[2];
    uint16_t buffer_flags; // bit 0: the ADPCM fields are valid; bit 1: looping
    uint16_t buffer_id;
};

static_assert(sizeof(SourceParams) == 0xC0);
static_assert(offsetof(SourceParams, rate) == 0x34);
static_assert(offsetof(SourceParams, interpolation) == 0x38);
static_assert(offsetof(SourceParams, polyphase_kernel) == 0x39);
static_assert(offsetof(SourceParams, filters) == 0x3A);
static_assert(offsetof(SourceParams, one_pole_b0) == 0x3C);
static_assert(offsetof(SourceParams, biquad_a2) == 0x40);
static_assert(offsetof(SourceParams, queue_dirty) == 0x4A);
static_assert(offsetof(SourceParams, queue) == 0x4C);
static_assert(offsetof(SourceParams, enable) == 0xA0);
static_assert(offsetof(SourceParams, sync) == 0xA2);
static_assert(offsetof(SourceParams, play_position) == 0xA4);
static_assert(offsetof(SourceParams, address) == 0xAC);
static_assert(offsetof(SourceParams, length) == 0xB0);
static_assert(offsetof(SourceParams, format) == 0xB4);
static_assert(offsetof(SourceParams, adpcm_ps) == 0xB6);
static_assert(offsetof(SourceParams, adpcm_yn) == 0xB8);
static_assert(offsetof(SourceParams, buffer_flags) == 0xBC);
static_assert(offsetof(SourceParams, buffer_id) == 0xBE);

// The DSP's per-frame report on a source.
struct SourceReport
{
    uint8_t enabled;
    uint8_t buffer_changed;  // nonzero when current_buffer changed
    uint16_t sync;           // the last SourceParams::sync the DSP took
    DspU32 position;         // samples into the current buffer
    uint16_t current_buffer; // id of the buffer playing (0: none)
    uint16_t last_buffer;    // id of the last buffer of the queue that finished
};

static_assert(sizeof(SourceReport) == 12);
static_assert(offsetof(SourceReport, position) == 4);
static_assert(offsetof(SourceReport, current_buffer) == 8);

// The firmware's delay on an aux bus, as nn::snd's DSP delay writes it (Luigi's Mansion of 2018, code.bin 0x26c2cc).
// MixerParams holds one for each bus. The delay line lies in FCRAM.
struct FirmwareDelayParams
{
    uint16_t dirty;  // bit 0: enable; bit 1: buffer; bit 2: the fields from channels to damping_b
    uint16_t enable; // 1: the delay runs
    uint16_t unused_04;
    uint16_t channels;  // 2, or 4 with surround
    DspU32 buffer;      // physical address of the delay line: frames × channels × 160 32-bit samples
    uint16_t frames;    // the delay in frames of 160 samples
    uint16_t feedback;  // × 128
    uint16_t damping_a; // (1 - damping) × 128
    uint16_t damping_b; // damping × 128
};

static_assert(sizeof(FirmwareDelayParams) == 20);
static_assert(offsetof(FirmwareDelayParams, buffer) == 0x08);
static_assert(offsetof(FirmwareDelayParams, frames) == 0x0C);

// The firmware's reverb on an aux bus. nn::snd's DSP reverb writes this record (StreetPass Mii Plaza's update v14.6.0,
// code.bin 0x2eeda8 and 0x161848). MixerParams holds one for each bus. The five lines lie in FCRAM, one after another.
struct FirmwareReverbParams
{
    uint16_t dirty;  // bit 0: enable; bit 1: lines; bit 2: channels and the fields from frames on
    uint16_t enable; // 1: the reverb runs
    uint16_t unused_04;
    uint16_t channels;      // 2
    DspU32 lines[5];        // physical addresses: early reflections, pre-delay, comb filters 0 and 1, all-pass
    uint16_t frames[5];     // each line's length in frames of 160 samples, in the same order
    uint16_t early_gain;    // × 128
    uint16_t fused_gain;    // × 128
    uint16_t all_pass_gain; // the coloration × 128
    int16_t comb_gains[2];  // 10^(-3·length/(fused time·32728)) × 128
    int16_t damping_a;      // (1 - damping) × 128, negated with the high-pass damping
    int16_t damping_b;      // damping × 128, negated with the high-pass damping
};

static_assert(sizeof(FirmwareReverbParams) == 0x34);
static_assert(offsetof(FirmwareReverbParams, lines) == 0x08);
static_assert(offsetof(FirmwareReverbParams, frames) == 0x1C);
static_assert(offsetof(FirmwareReverbParams, early_gain) == 0x26);
static_assert(offsetof(FirmwareReverbParams, comb_gains) == 0x2C);
static_assert(offsetof(FirmwareReverbParams, damping_b) == 0x32);

// Output-stage parameters, written by the ARM11.
struct MixerParams
{
    uint32_t dirty;
    float master_volume;
    float aux_return[2];
    uint16_t output_buffers;
    uint16_t unused_12[2];
    uint16_t output_mode;   // 0 mono, 1 stereo, 2 surround
    uint16_t clipping_mode; // 0 limiter off, 1 limiter on (nn::snd sets 1)
    uint16_t headphones;
    uint16_t surround_depth;
    uint16_t speaker_position;
    uint16_t unused_20;
    uint16_t rear_ratio;
    uint16_t aux_bypass[2];
    uint16_t aux_enable[2];
    FirmwareDelayParams delay[2];   // the firmware's delays on buses A and B (dirty bits 10 and 11)
    FirmwareReverbParams reverb[2]; // the firmware's reverbs on buses A and B (dirty bits 12 and 13)
    uint16_t sync_mode;
    uint16_t unused_be;
    uint32_t dirty2;
};

static_assert(sizeof(MixerParams) == 0xC4);
static_assert(offsetof(MixerParams, master_volume) == 0x04);
static_assert(offsetof(MixerParams, aux_return) == 0x08);
static_assert(offsetof(MixerParams, output_buffers) == 0x10);
static_assert(offsetof(MixerParams, output_mode) == 0x16);
static_assert(offsetof(MixerParams, clipping_mode) == 0x18);
static_assert(offsetof(MixerParams, surround_depth) == 0x1C);
static_assert(offsetof(MixerParams, rear_ratio) == 0x22);
static_assert(offsetof(MixerParams, aux_bypass) == 0x24);
static_assert(offsetof(MixerParams, aux_enable) == 0x28);
static_assert(offsetof(MixerParams, delay) == 0x2C);
static_assert(offsetof(MixerParams, reverb) == 0x54);
static_assert(offsetof(MixerParams, sync_mode) == 0xBC);
static_assert(offsetof(MixerParams, dirty2) == 0xC0);

// DSP-ADPCM coefficients: 16 per source.
struct AdpcmCoefficientTable
{
    int16_t source[kNumSources][16];
};

static_assert(sizeof(AdpcmCoefficientTable) == 768);

} // namespace threesf::nnsnd
