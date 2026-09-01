#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

namespace pipeeq {

// Lock-free ring buffer of interleaved float frames: exactly one producer (an
// InputSource's own process() callback) calls write(); any number of
// independent consumers may call readAt() concurrently with the writer and with
// each other, each maintaining its own cursor.
//
// This shape exists because PW_STREAM_FLAG_RT_PROCESS means process() runs on
// PipeWire's own realtime thread rather than a thread we control -
// pw_thread_loop_lock() does NOT synchronize against it (confirmed against
// PipeWire's own docs: "An exception [to callbacks running under the thread
// lock] is for the data processing callbacks... when PW_STREAM_FLAG_RT_PROCESS
// is set").
//
// No backpressure: if a reader falls behind by more than the buffer's capacity,
// its cursor jumps forward to the oldest frame still available - an audible
// jump on underrun/overrun, never undefined behaviour.
//
// The payload is std::atomic<float> with relaxed access, not plain float.
// Relaxed atomics compile to exactly the same load/store instruction on every
// architecture PipeWire runs on, so this costs nothing at runtime - but plain
// floats raced between the producer and a consumer were a genuine data race,
// and a torn read can produce a NaN. A NaN entering a biquad's IIR state stays
// there for the lifetime of the process, so "an audible jump" would have been
// "that channel is silent until you restart the daemon". (OutputProcessor keeps
// a per-block isfinite guard as well; this removes the cause, that one bounds
// the damage from any other source.)
class RingBuffer {
public:
    RingBuffer(std::size_t capacityFrames, int numChannels);

    // Producer-only: must never be called from more than one thread.
    void write(const float* interleaved, std::size_t frames);

    // Any number of readers may call this concurrently (with the writer and
    // with each other), each owning its own `cursor`. Writes exactly
    // frames * numChannels() floats, zero-filling any it has no data for.
    void readAt(std::size_t& cursor, float* outInterleaved, std::size_t frames) const;

    std::size_t currentWriteIndex() const { return writeIndex_.load(std::memory_order_acquire); }
    int numChannels() const { return numChannels_; }
    std::size_t capacityFrames() const { return capacityFrames_; }

private:
    int numChannels_;
    std::size_t capacityFrames_;
    // unique_ptr<atomic<float>[]> rather than vector<atomic<float>> because
    // std::atomic is neither copyable nor movable, so it can't go in a vector.
    std::unique_ptr<std::atomic<float>[]> buffer_;

    std::size_t writePos_ = 0; // producer-local shadow, never read by consumers
    std::atomic<std::size_t> writeIndex_{0};
};

} // namespace pipeeq
