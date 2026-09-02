// RingBuffer: the multi-reader, single-writer path audio takes from an input to
// every output that subscribes to it.

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "ring_buffer.h"

#include "check.h"

namespace {

using namespace pipeeq;

std::vector<float> interleave(const std::vector<std::vector<float>>& channels) {
    const std::size_t frames = channels.empty() ? 0 : channels[0].size();
    std::vector<float> out(frames * channels.size());
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            out[f * channels.size() + ch] = channels[ch][f];
        }
    }
    return out;
}

void testWriteThenReadRoundTrips() {
    RingBuffer buffer(64, 2);
    CHECK_EQ(buffer.numChannels(), 2);

    const std::vector<float> in = interleave({{1.0f, 2.0f, 3.0f}, {-1.0f, -2.0f, -3.0f}});
    buffer.write(in.data(), 3);

    std::size_t cursor = 0;
    std::vector<float> out(3 * 2, 99.0f);
    buffer.readAt(cursor, out.data(), 3);

    CHECK_EQ(out, in);
    CHECK_EQ(cursor, std::size_t{3});
}

// Zero-fill, not stale data: a reader asking for more than has been written
// must get silence for the remainder, or it would replay whatever the ring
// happened to hold there.
void testReadBeyondAvailableZeroFills() {
    RingBuffer buffer(64, 2);
    const std::vector<float> in = interleave({{1.0f, 2.0f}, {1.0f, 2.0f}});
    buffer.write(in.data(), 2);

    std::size_t cursor = 0;
    std::vector<float> out(5 * 2, 99.0f);
    buffer.readAt(cursor, out.data(), 5);

    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(out[i] != 99.0f);
    }
    for (std::size_t i = 4; i < out.size(); ++i) {
        CHECK_NEAR(out[i], 0.0, 1e-12);
    }
    CHECK_EQ(cursor, std::size_t{2});
}

void testReadFromEmptyBufferIsSilence() {
    RingBuffer buffer(32, 2);
    std::size_t cursor = 0;
    std::vector<float> out(4 * 2, 99.0f);
    buffer.readAt(cursor, out.data(), 4);

    for (float sample : out) {
        CHECK_NEAR(sample, 0.0, 1e-12);
    }
    CHECK_EQ(cursor, std::size_t{0});
}

// Every output holds its own cursor into one shared buffer, so two readers at
// different positions must not disturb each other.
void testIndependentCursors() {
    RingBuffer buffer(64, 1);
    std::vector<float> in(8);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = static_cast<float>(i);
    }
    buffer.write(in.data(), in.size());

    std::size_t fast = 0;
    std::size_t slow = 0;
    std::vector<float> out(4);

    buffer.readAt(fast, out.data(), 4);
    CHECK_NEAR(out[0], 0.0, 1e-6);
    buffer.readAt(fast, out.data(), 4);
    CHECK_NEAR(out[0], 4.0, 1e-6);

    // The slow reader still sees the beginning.
    buffer.readAt(slow, out.data(), 4);
    CHECK_NEAR(out[0], 0.0, 1e-6);
}

// A reader that falls further behind than the buffer holds must resync to the
// oldest surviving frame - an audible jump, never a read of garbage.
void testLaggingReaderResyncsToOldestAvailable() {
    const std::size_t capacity = 16;
    RingBuffer buffer(capacity, 1);

    std::vector<float> block(capacity);
    for (int round = 0; round < 4; ++round) {
        for (std::size_t i = 0; i < capacity; ++i) {
            block[i] = static_cast<float>(round * capacity + i);
        }
        buffer.write(block.data(), capacity);
    }

    std::size_t staleCursor = 0; // 64 frames behind
    std::vector<float> out(4);
    buffer.readAt(staleCursor, out.data(), 4);

    // Oldest surviving frame is writeIndex - capacity == 64 - 16 == 48.
    CHECK_EQ(staleCursor, std::size_t{48 + 4});
    CHECK_NEAR(out[0], 48.0, 1e-6);
}

void testWrapAroundPreservesChannelInterleaving() {
    const std::size_t capacity = 8;
    RingBuffer buffer(capacity, 2);

    // Write 12 frames through a capacity-8 ring: the last 8 survive.
    std::vector<float> in(12 * 2);
    for (std::size_t f = 0; f < 12; ++f) {
        in[f * 2 + 0] = static_cast<float>(f);
        in[f * 2 + 1] = -static_cast<float>(f);
    }
    buffer.write(in.data(), 12);

    std::size_t cursor = 4; // the oldest surviving frame
    std::vector<float> out(8 * 2);
    buffer.readAt(cursor, out.data(), 8);

    for (std::size_t f = 0; f < 8; ++f) {
        const float expected = static_cast<float>(f + 4);
        CHECK_NEAR(out[f * 2 + 0], expected, 1e-6);
        CHECK_NEAR(out[f * 2 + 1], -expected, 1e-6);
    }
}

void testMultichannelLayouts() {
    for (int channels : {1, 2, 6, 8, 12}) {
        RingBuffer buffer(32, channels);
        std::vector<float> in(4 * static_cast<std::size_t>(channels));
        for (std::size_t i = 0; i < in.size(); ++i) {
            in[i] = static_cast<float>(i) * 0.01f;
        }
        buffer.write(in.data(), 4);

        std::size_t cursor = 0;
        std::vector<float> out(in.size(), 99.0f);
        buffer.readAt(cursor, out.data(), 4);
        CHECK_EQ(out, in);
    }
}

// A concurrent writer and two readers, as the daemon really runs it.
//
// The writer alternates between two bit patterns chosen so that a TORN read is
// detectable: mixing the high half of one with the low half of the other cannot
// produce either value. An earlier version of this test wrote a single constant,
// which made the assertion mathematically incapable of failing - tearing between
// two identical floats yields that same float - so reverting the atomic payload
// still passed it three times over.
//
// Under -DPIPEEQ_SANITIZE=thread this is also the check that the atomic payload
// removed the data race outright.
void testConcurrentWriterAndReaders() {
    RingBuffer buffer(1024, 2);
    std::atomic<bool> stop{false};
    std::atomic<bool> sawImpossibleValue{false};

    // 0x3F000000 and 0x40A00000: every byte of the two differs in the exponent
    // and mantissa, so any half-and-half combination is a third value.
    constexpr float kValueA = 0.5f;
    constexpr float kValueB = 5.0f;

    std::thread writer([&] {
        std::vector<float> blockA(128 * 2, kValueA);
        std::vector<float> blockB(128 * 2, kValueB);
        bool useA = true;
        while (!stop.load(std::memory_order_relaxed)) {
            buffer.write(useA ? blockA.data() : blockB.data(), 128);
            useA = !useA;
        }
    });

    auto readerBody = [&] {
        std::size_t cursor = 0;
        std::vector<float> out(64 * 2);
        while (!stop.load(std::memory_order_relaxed)) {
            buffer.readAt(cursor, out.data(), 64);
            for (float sample : out) {
                // Only the zero-fill and the two written values are possible.
                // Anything else is a torn read.
                if (sample != 0.0f && sample != kValueA && sample != kValueB) {
                    sawImpossibleValue.store(true, std::memory_order_relaxed);
                }
            }
        }
    };
    std::thread readerA(readerBody);
    std::thread readerB(readerBody);

    std::this_thread::sleep_for(std::chrono::milliseconds(pipeeq::test::concurrencyMs(200)));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    readerA.join();
    readerB.join();

    CHECK(!sawImpossibleValue.load());
}

} // namespace

int main() {
    RUN(testWriteThenReadRoundTrips);
    RUN(testReadBeyondAvailableZeroFills);
    RUN(testReadFromEmptyBufferIsSilence);
    RUN(testIndependentCursors);
    RUN(testLaggingReaderResyncsToOldestAvailable);
    RUN(testWrapAroundPreservesChannelInterleaving);
    RUN(testMultichannelLayouts);
    RUN(testConcurrentWriterAndReaders);
    return pipeeq::test::summary("ring_buffer");
}
