// PeakMeterBank: the bit-pattern trick that gives an exact lock-free float max,
// the take-and-reset semantics the control thread relies on, and a two-thread
// smoke test that must be clean under -DPIPEEQ_SANITIZE=thread.

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "peak_meter.h"

#include "check.h"

namespace {

using namespace pipeeq;

void testAccumulateKeepsTheMaximum() {
    PeakMeterBank<4> meters;

    meters.accumulate(0, 0.25f);
    meters.accumulate(0, 0.75f);
    meters.accumulate(0, 0.5f); // must not lower the held peak
    CHECK_NEAR(meters.peek(0), 0.75, 1e-6);

    // Channels are independent.
    CHECK_NEAR(meters.peek(1), 0.0, 1e-12);
}

// The whole design rests on IEEE-754 bit patterns being monotone for
// non-negative floats, so check it across a wide range rather than one value.
void testBitPatternMaxIsATrueFloatMax() {
    PeakMeterBank<1> meters;
    const std::vector<float> values = {0.0f,     1e-30f, 1e-8f,   0.001f, 0.1f,
                                        0.499f,  0.5f,   0.99999f, 1.0f,   4.0f,
                                        1000.0f};

    // Feed them in an order that would break a naive integer compare if the
    // monotonicity assumption were wrong.
    float expected = 0.0f;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float value = values[(i * 7) % values.size()];
        meters.accumulate(0, value);
        expected = std::max(expected, value);
        CHECK_NEAR(meters.peek(0), expected, 1e-9);
    }
}

void testTakeAndResetReturnsThenClears() {
    PeakMeterBank<2> meters;
    meters.accumulate(1, 0.6f);

    CHECK_NEAR(meters.takeAndReset(1), 0.6, 1e-6);
    // Reset means the next window starts from silence, so a stopped signal
    // decays instead of holding forever.
    CHECK_NEAR(meters.takeAndReset(1), 0.0, 1e-12);
    CHECK_NEAR(meters.peek(1), 0.0, 1e-12);
}

// Out-of-range channels must be inert, not memory corruption: the RT thread
// passes a channel index derived from a negotiated format it doesn't control.
void testOutOfRangeChannelsAreIgnored() {
    PeakMeterBank<2> meters;
    meters.accumulate(2, 1.0f);
    meters.accumulate(99, 1.0f);
    CHECK_NEAR(meters.takeAndReset(2), 0.0, 1e-12);
    CHECK_NEAR(meters.peek(99), 0.0, 1e-12);
    CHECK_NEAR(meters.peek(0), 0.0, 1e-12);
    CHECK_EQ(PeakMeterBank<2>::capacity(), std::size_t{2});
}

void testResetClearsEverything() {
    PeakMeterBank<3> meters;
    for (std::size_t ch = 0; ch < 3; ++ch) {
        meters.accumulate(ch, 0.9f);
    }
    meters.reset();
    for (std::size_t ch = 0; ch < 3; ++ch) {
        CHECK_NEAR(meters.peek(ch), 0.0, 1e-12);
    }
}

// One writer accumulating while a reader drains, as the daemon really uses it.
// The reader may lose at most the block in flight, so the sum of everything it
// observes must never exceed the true peak, and the true peak must be seen at
// least once given it is fed repeatedly.
void testConcurrentWriterAndReader() {
    PeakMeterBank<2> meters;
    std::atomic<bool> stop{false};
    std::atomic<bool> sawPeak{false};
    std::atomic<bool> sawTooLarge{false};

    std::thread writer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 64; ++i) {
                meters.accumulate(0, 0.25f);
            }
            meters.accumulate(0, 0.8f);
        }
    });

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const float value = meters.takeAndReset(0);
            if (value > 0.8f + 1e-6f) {
                sawTooLarge.store(true, std::memory_order_relaxed);
            }
            if (std::fabs(value - 0.8f) < 1e-6f) {
                sawPeak.store(true, std::memory_order_relaxed);
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    reader.join();

    CHECK(!sawTooLarge.load());
    CHECK(sawPeak.load());
}

} // namespace

int main() {
    RUN(testAccumulateKeepsTheMaximum);
    RUN(testBitPatternMaxIsATrueFloatMax);
    RUN(testTakeAndResetReturnsThenClears);
    RUN(testOutOfRangeChannelsAreIgnored);
    RUN(testResetClearsEverything);
    RUN(testConcurrentWriterAndReader);
    return pipeeq::test::summary("peak_meter");
}
