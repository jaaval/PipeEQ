#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pipeeq {

// Per-channel peak hold, written by the realtime thread and drained by the
// control thread, with no lock and no allocation.
//
// The trick: for non-negative floats the IEEE-754 bit pattern is monotone in
// the value, so a maximum over the BIT PATTERNS is a maximum over the
// magnitudes. That means a plain integer compare-exchange gives an exact float
// maximum without needing any float-atomic max support.
template <std::size_t N>
class PeakMeterBank {
public:
    // Realtime side: call ONCE PER BLOCK PER CHANNEL with that block's local
    // maximum magnitude - never once per sample. A per-sample lock cmpxchg
    // across 32 channels at 48 kHz would burn a few percent of a core to
    // measure something a meter redraws 30 times a second.
    //
    // `magnitude` must be non-negative; a negative value would compare wrong
    // under the bit-pattern trick, so callers pass fabs().
    void accumulate(std::size_t channel, float magnitude) {
        if (channel >= N) {
            return;
        }
        uint32_t bits;
        std::memcpy(&bits, &magnitude, sizeof(bits));
        uint32_t previous = cells_[channel].load(std::memory_order_relaxed);
        while (bits > previous &&
               !cells_[channel].compare_exchange_weak(previous, bits, std::memory_order_relaxed)) {
            // compare_exchange_weak refreshed `previous`; retry only while we
            // are still the larger value.
        }
    }

    // Control side: the peak since the previous call, then reset to zero.
    //
    // Racing the writer can at worst lose one block's peak, which at a 30 Hz
    // refresh and ~1-20 ms blocks is invisible. Documented rather than fixed:
    // making it exact would need the RT side to synchronize with the reader.
    float takeAndReset(std::size_t channel) {
        if (channel >= N) {
            return 0.0f;
        }
        const uint32_t bits = cells_[channel].exchange(0, std::memory_order_relaxed);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // Non-destructive read, for tests and diagnostics.
    float peek(std::size_t channel) const {
        if (channel >= N) {
            return 0.0f;
        }
        const uint32_t bits = cells_[channel].load(std::memory_order_relaxed);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    void reset() {
        for (auto& cell : cells_) {
            cell.store(0, std::memory_order_relaxed);
        }
    }

    static constexpr std::size_t capacity() { return N; }

private:
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "PeakMeterBank needs lock-free atomic<uint32_t> to be usable from the RT thread");
    std::array<std::atomic<uint32_t>, N> cells_{};
};

} // namespace pipeeq
