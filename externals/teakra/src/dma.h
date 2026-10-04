#pragma once
#include <algorithm>
#include <array>
#include <functional>
#include <type_traits>
#include <utility>
#include "common_types.h"

namespace Teakra {

struct SharedMemory;
class Ahbm;

class Dma {
public:
    Dma(SharedMemory& shared_memory, Ahbm& ahbm) : shared_memory(shared_memory), ahbm(ahbm) {}

    void Reset();

    // Lets the DMA move one more step (a word, or two in dword mode) for each of the `cycles` the
    // DSP is about to run.
    void AddBudget(u64 cycles) {
        budget = std::min(budget + cycles, std::max(cycles, BudgetCap));
    }

    void EnableChannel(u16 value) {
        enable_channel = value;
    }
    u16 GetChannelEnabled() const {
        return enable_channel;
    }

    void ActivateChannel(u16 value) {
        // Wraps around, so that the firmware can't make the channel registers index past the end
        // of `channels`.
        active_channel = static_cast<u16>(value % channels.size());
    }
    u16 GetActiveChannel() const {
        return active_channel;
    }

    void SetAddrSrcLow(u16 value) {
        channels[active_channel].addr_src_low = value;
    }
    u16 GetAddrSrcLow() const {
        return channels[active_channel].addr_src_low;
    }

    void SetAddrSrcHigh(u16 value) {
        channels[active_channel].addr_src_high = value;
    }
    u16 GetAddrSrcHigh() const {
        return channels[active_channel].addr_src_high;
    }

    void SetAddrDstLow(u16 value) {
        channels[active_channel].addr_dst_low = value;
    }
    u16 GetAddrDstLow() const {
        return channels[active_channel].addr_dst_low;
    }

    void SetAddrDstHigh(u16 value) {
        channels[active_channel].addr_dst_high = value;
    }
    u16 GetAddrDstHigh() const {
        return channels[active_channel].addr_dst_high;
    }

    void SetSize0(u16 value) {
        channels[active_channel].size0 = value;
    }
    u16 GetSize0() const {
        return channels[active_channel].size0;
    }

    void SetSize1(u16 value) {
        channels[active_channel].size1 = value;
    }
    u16 GetSize1() const {
        return channels[active_channel].size1;
    }

    void SetSize2(u16 value) {
        channels[active_channel].size2 = value;
    }
    u16 GetSize2() const {
        return channels[active_channel].size2;
    }

    void SetSrcStep0(u16 value) {
        channels[active_channel].src_step0 = value;
    }
    u16 GetSrcStep0() const {
        return channels[active_channel].src_step0;
    }

    void SetDstStep0(u16 value) {
        channels[active_channel].dst_step0 = value;
    }
    u16 GetDstStep0() const {
        return channels[active_channel].dst_step0;
    }

    void SetSrcStep1(u16 value) {
        channels[active_channel].src_step1 = value;
    }
    u16 GetSrcStep1() const {
        return channels[active_channel].src_step1;
    }

    void SetDstStep1(u16 value) {
        channels[active_channel].dst_step1 = value;
    }
    u16 GetDstStep1() const {
        return channels[active_channel].dst_step1;
    }

    void SetSrcStep2(u16 value) {
        channels[active_channel].src_step2 = value;
    }
    u16 GetSrcStep2() const {
        return channels[active_channel].src_step2;
    }

    void SetDstStep2(u16 value) {
        channels[active_channel].dst_step2 = value;
    }
    u16 GetDstStep2() const {
        return channels[active_channel].dst_step2;
    }

    void SetSrcSpace(u16 value) {
        channels[active_channel].src_space = value;
    }
    u16 GetSrcSpace() const {
        return channels[active_channel].src_space;
    }

    void SetDstSpace(u16 value) {
        channels[active_channel].dst_space = value;
    }
    u16 GetDstSpace() const {
        return channels[active_channel].dst_space;
    }

    void SetDwordMode(u16 value) {
        channels[active_channel].dword_mode = value;
    }
    u16 GetDwordMode() const {
        return channels[active_channel].dword_mode;
    }

    void SetY(u16 value) {
        channels[active_channel].y = value;
    }
    u16 GetY() const {
        return channels[active_channel].y;
    }

    void SetZ(u16 value) {
        channels[active_channel].z = value;

        if (value == 0x40C0) {
            DoDma(active_channel);
        }
    }
    u16 GetZ() const {
        return channels[active_channel].z;
    }

    void DoDma(u16 channel);

    void SetInterruptHandler(std::function<void()> handler) {
        interrupt_handler = std::move(handler);
    }

    template <typename Archive>
    void Serialize(Archive& ar) {
        // Whole channels, so that a field added to them is saved too.
        static_assert(std::is_trivially_copyable_v<Channel>);
        ar(enable_channel, active_channel, budget, channels);
    }

private:
    std::function<void()> interrupt_handler;

    u16 enable_channel = 0;
    u16 active_channel = 0;

    struct Channel {
        u16 addr_src_low = 0, addr_src_high = 0;
        u16 addr_dst_low = 0, addr_dst_high = 0;
        u16 size0 = 0, size1 = 0, size2 = 0;
        u16 src_step0 = 0, dst_step0 = 0;
        u16 src_step1 = 0, dst_step1 = 0;
        u16 src_step2 = 0, dst_step2 = 0;
        u16 src_space = 0, dst_space = 0;
        u16 dword_mode = 0;
        u16 y = 0;
        u16 z = 0;

        u32 current_src = 0, current_dst = 0;
        // Wider than the sizes, so that a dword transfer of 0xFFFF words ends instead of wrapping.
        u32 counter0 = 0, counter1 = 0, counter2 = 0;
        u16 running = 0;
        u16 ahbm_channel = 0;

        void Start();
        void Tick(Dma& parent);
    };

    std::array<Channel, 8> channels;

    // Steps the DMA may still take. A transfer runs to its end within one DSP instruction, and the
    // firmware comes from the file being played, so without a limit a crafted transfer of up to
    // 0xFFFF * 0xFFFF * 0xFFFF steps could run for years. Real firmware stays far below one step
    // per cycle, so the limit leaves real transfers alone.
    static constexpr u64 BudgetCap = 1 << 20;
    u64 budget = BudgetCap;

    SharedMemory& shared_memory;
    Ahbm& ahbm;
};

} // namespace Teakra
