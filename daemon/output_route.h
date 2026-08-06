#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>

#include "biquad.h"
#include "eq_band.h"

namespace pipeeq {

// Lock-free ring buffer of interleaved float frames: exactly one producer
// (an InputSource's own process() callback) calls write(); any number of
// independent consumers may call readAt() concurrently with the writer and
// with each other, each maintaining its own cursor. This shape exists
// because PW_STREAM_FLAG_RT_PROCESS means process() runs on PipeWire's own
// realtime thread rather than a thread we control - pw_thread_loop_lock()
// does NOT synchronize against it (confirmed against PipeWire's own docs:
// "An exception [to callbacks running under the thread lock] is for the
// data processing callbacks... when PW_STREAM_FLAG_RT_PROCESS is set").
// No backpressure: if a reader falls behind by more than the buffer's
// capacity, its cursor jumps forward to the oldest frame still available
// (an audible jump on underrun/overrun, never undefined behavior).
class InterleavedRingBuffer {
public:
    InterleavedRingBuffer(std::size_t capacityFrames, int numChannels);

    // Producer-only: must never be called from more than one thread.
    void write(const float* interleaved, std::size_t frames);

    // Any number of readers may call this concurrently (with the writer
    // and with each other), each owning its own `cursor`.
    void readAt(std::size_t& cursor, float* outInterleaved, std::size_t frames) const;

    std::size_t currentWriteIndex() const { return writeIndex_.load(std::memory_order_acquire); }
    int numChannels() const { return numChannels_; }

private:
    int numChannels_;
    std::size_t capacityFrames_;
    std::vector<float> buffer_;

    std::size_t writePos_ = 0; // producer-local shadow, never read by consumers
    std::atomic<std::size_t> writeIndex_{0};
};

// One route's mix contribution from a single input: which input, the
// (shared, possibly also referenced by other routes) buffer it reads from,
// and this route's send level for it. Lives inside RouteSnapshot, so it's
// only ever constructed/destroyed on the control thread and only ever read
// (never mutated in place) by the RT thread.
struct InputMixSlot {
    bool active = false;
    std::string inputId;
    std::shared_ptr<const InterleavedRingBuffer> buffer;
    float gainLinear = 1.0f;
};

// Immutable, atomically-swapped view of everything OutputRoute::onProcess()
// needs. Rebuilt wholesale (copy-on-write) by the control thread on every
// mutation, and read via a single snapshot_.load() at the top of each RT
// process() call - never mutated in place, so no torn reads and no lock
// needed on the RT side. Filter history and per-input read cursors are
// deliberately NOT here (see OutputRoute) since they must persist across
// snapshot swaps rather than reset with them.
struct RouteSnapshot {
    std::vector<eqcore::BiquadCoeffs> coeffs; // one per active EQ band
    float masterGainLinear = 1.0f;
    bool muted = false;
    std::array<InputMixSlot, 8> inputs{};
};

// One physical output route: a playback stream pinned to a target device,
// mixing any number of subscribed inputs, then running the mix through a
// parametric EqChain-equivalent and a master gain/mute.
//
// This object exists only while its target device does; the route's
// user-visible configuration (gain, mute, EQ, mix levels) is owned by
// AudioEngine::RouteEntry and outlives it, so a device disappearing tears
// down the stream without losing any settings. Nothing here is a source of
// truth for the control plane - it is write-mostly, driven from the entry.
//
// Threading model: all mutators (setGainDb/setMuted/setBandCount/setBand/
// setInputGainDb/removeInputSlot) are called only under
// AudioEngine::controlMutex_, so they never race each other. They DO race
// onProcess(), which
// runs on PipeWire's realtime thread when PW_STREAM_FLAG_RT_PROCESS is set
// (true here) - pw_thread_loop_lock() does not protect against this (see
// InterleavedRingBuffer's comment). RT-visible state is therefore split
// into: (a) an atomically-swapped immutable RouteSnapshot for anything
// that changes via user action, and (b) fixed-capacity, RT-thread-owned
// arrays for anything that must continuously persist across process()
// calls (filter history, per-input read cursors) - resizing either on the
// RT path is exactly what this design avoids.
class OutputRoute {
public:
    static constexpr std::size_t kMaxBands = 16;
    static constexpr std::size_t kMaxInputs = 8;

    // leftPosition/rightPosition are the SPA channel positions this output
    // drives on the target device. Pass SPA_AUDIO_CHANNEL_UNKNOWN for both to
    // let the device decide, which is what devices with no layout PipeEQ
    // recognizes get; otherwise the stream is pinned to exactly that pair, so
    // several outputs can share one multi-channel interface without bleeding
    // into each other's channels.
    OutputRoute(pw_core* core, pw_thread_loop* loop, std::string id, std::string deviceName,
                std::string displayName, uint32_t targetNodeId, int numChannels, uint32_t sampleRateHz,
                uint32_t leftPosition, uint32_t rightPosition);
    ~OutputRoute();

    OutputRoute(const OutputRoute&) = delete;
    OutputRoute& operator=(const OutputRoute&) = delete;

    const std::string& id() const { return id_; }
    const std::string& deviceName() const { return deviceName_; }
    const std::string& displayName() const { return displayName_; }

    void setGainDb(double gainDb);
    void setMuted(bool muted);

    void setBandCount(std::size_t count);
    void setBand(std::size_t index, const eqcore::EqBand& band);

    // Activates (if not already active) or updates this route's mix level
    // for the given input. `buffer` is only used the first time this input
    // is activated on this route. Returns false if this route's input-slot
    // pool (kMaxInputs) is already full and this input has no existing slot.
    bool setInputGainDb(const std::string& inputId, std::shared_ptr<const InterleavedRingBuffer> buffer,
                        double gainDb);
    // Deactivates this route's mix slot for the given input, freeing it for
    // reuse. No-op if not currently active.
    void removeInputSlot(const std::string& inputId);

    static void onProcess(void* userdata);
    static void onStateChanged(void* userdata, pw_stream_state old, pw_stream_state state,
                                const char* error);

private:
    void rebuildSnapshot();
    int findSlot(const std::string& inputId) const;

    std::string id_;
    std::string deviceName_;
    std::string displayName_;
    int numChannels_;
    uint32_t sampleRateHz_;

    pw_stream* stream_ = nullptr;
    spa_hook listener_{};

    // Control-thread-only "source of truth" - see the class comment for why
    // these need no synchronization among themselves.
    double gainDb_ = 0.0;
    bool muted_ = false;
    std::vector<eqcore::EqBand> bands_;
    std::array<InputMixSlot, kMaxInputs> inputSlotsMirror_{};

    // RT-thread-owned, never touched by the control thread: continuous
    // filter history (fixed capacity so a live band-count change never
    // resizes anything on the RT path) and per-input read cursors (must
    // persist across snapshot swaps, so they can't live inside the
    // snapshot itself).
    std::vector<std::array<eqcore::BiquadState, kMaxBands>> bandState_;
    std::array<std::size_t, kMaxInputs> readCursors_{};

    // Generously preallocated once at construction so onProcess() never
    // needs to grow it; frames-per-callback is clamped to this as a safety
    // net rather than ever resizing on the RT path.
    static constexpr std::size_t kScratchCapacityFrames = 8192;
    std::vector<float> mixScratch_;

    std::atomic<std::shared_ptr<const RouteSnapshot>> snapshot_;
};

} // namespace pipeeq
