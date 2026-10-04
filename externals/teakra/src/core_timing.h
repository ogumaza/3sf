#pragma once

#include <algorithm>
#include <functional>
#include <limits>
#include <vector>
#include "common_types.h"

namespace Teakra {

class CoreTiming {
public:
    class Callbacks {
    public:
        virtual ~Callbacks() = default;
        virtual void Tick() = 0;
        virtual u64 GetMaxSkip() const = 0;
        virtual void Skip(u64) = 0;
        static constexpr u64 Infinity = std::numeric_limits<u64>::max();
    };

    // One cycle. Ticks that can't produce an event (as GetMaxSkip() reports) are only counted,
    // and applied in one batch with Skip() when component state is needed: before a tick that
    // may produce an event, before the firmware reads or writes a timer or BTDMP register
    // (MMIORegion's AffectsTiming), on reset and at the end of a run. This gives the same results
    // as ticking every component on every cycle.
    TEAKRA_ALWAYS_INLINE void Tick() {
        if (pending < budget) {
            ++pending;
            return;
        }
        TickComponents();
    }

    // Number of ticks before the next possible event. The JIT limits its blocks to this budget, so
    // components need no updates during a block.
    u64 BudgetLeft() const {
        return budget - pending;
    }

    // Counts `ticks` ticks at once, no more than BudgetLeft().
    void AddPending(u64 ticks) {
        pending += ticks;
    }

    // Applies the counted ticks, so that component state is current.
    void Sync() {
        if (pending != 0) {
            for (const auto& callbacks : registered_callbacks) {
                callbacks->Skip(pending);
            }
            if (budget != Callbacks::Infinity) {
                budget -= pending;
            }
            pending = 0;
        }
    }

    // Component state is about to be read or changed from outside (MMIO, reset): applies the
    // counted ticks and re-evaluates the next event on the next tick.
    void Invalidate() {
        Sync();
        budget = 0;
    }

    u64 Skip(u64 maximum) {
        Invalidate();
        u64 ticks = maximum;
        for (const auto& callbacks : registered_callbacks) {
            ticks = std::min(ticks, callbacks->GetMaxSkip());
        }
        for (const auto& callbacks : registered_callbacks) {
            callbacks->Skip(ticks);
        }
        return ticks;
    }

    void RegisterCallbacks(Callbacks* callbacks) {
        registered_callbacks.push_back(std::move(callbacks));
    }

    // Saves or loads the counted ticks and the budget (see state.h). The components save their
    // own state.
    template <typename Archive>
    void Serialize(Archive& ar) {
        ar(pending, budget);
    }

private:
    // The tick that may produce an event: ticks every component.
    TEAKRA_NOINLINE void TickComponents() {
        Sync();
        for (const auto& callbacks : registered_callbacks) {
            callbacks->Tick();
        }
        budget = Callbacks::Infinity;
        for (const auto& callbacks : registered_callbacks) {
            budget = std::min(budget, callbacks->GetMaxSkip());
        }
    }

    std::vector<Callbacks*> registered_callbacks;
    u64 pending = 0; // ticks counted but not yet applied
    u64 budget = 0;  // ticks that can be counted before one may produce an event
};
} // namespace Teakra
