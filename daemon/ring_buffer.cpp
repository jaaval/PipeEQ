#include "ring_buffer.h"

#include <algorithm>

namespace pipeeq {

namespace {
// A relaxed atomic float load/store is a plain movss on x86-64 and a plain
// ldr/str on aarch64: no fence, no lock, no cost over a bare float.
static_assert(std::atomic<float>::is_always_lock_free,
              "RingBuffer needs lock-free atomic<float>; a locking fallback would put a mutex on "
              "the realtime audio path");
} // namespace

RingBuffer::RingBuffer(std::size_t capacityFrames, int numChannels)
    : numChannels_(numChannels),
      capacityFrames_(capacityFrames),
      buffer_(std::make_unique<std::atomic<float>[]>(capacityFrames *
                                                      static_cast<std::size_t>(numChannels))) {
    const std::size_t total = capacityFrames * static_cast<std::size_t>(numChannels);
    for (std::size_t i = 0; i < total; ++i) {
        buffer_[i].store(0.0f, std::memory_order_relaxed);
    }
}

void RingBuffer::write(const float* interleaved, std::size_t frames) {
    const std::size_t channels = static_cast<std::size_t>(numChannels_);
    std::size_t pos = writePos_;
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t slot = (pos + i) % capacityFrames_;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            buffer_[slot * channels + ch].store(interleaved[i * channels + ch],
                                                 std::memory_order_relaxed);
        }
    }
    pos += frames;
    writePos_ = pos;
    // Release: everything stored above is visible to a consumer that acquires
    // this index. That ordering is what makes the relaxed payload accesses safe
    // for the frames the index says are available.
    writeIndex_.store(pos, std::memory_order_release);
}

void RingBuffer::readAt(std::size_t& cursor, float* outInterleaved, std::size_t frames) const {
    const std::size_t channels = static_cast<std::size_t>(numChannels_);
    const std::size_t writtenTotal = writeIndex_.load(std::memory_order_acquire);

    // If this reader has fallen behind by more than the buffer holds, jump
    // forward to the oldest frame the writer hasn't already overwritten.
    if (writtenTotal > cursor + capacityFrames_) {
        cursor = writtenTotal - capacityFrames_;
    }

    const std::size_t availableFrames = (writtenTotal > cursor) ? (writtenTotal - cursor) : 0;
    const std::size_t toRead = std::min(frames, availableFrames);

    for (std::size_t i = 0; i < toRead; ++i) {
        const std::size_t slot = (cursor + i) % capacityFrames_;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            outInterleaved[i * channels + ch] = buffer_[slot * channels + ch].load(std::memory_order_relaxed);
        }
    }
    std::fill(outInterleaved + toRead * channels, outInterleaved + frames * channels, 0.0f);

    cursor += toRead;
}

} // namespace pipeeq
