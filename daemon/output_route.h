#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <pipewire/pipewire.h>

#include "eq_band.h"
#include "eq_chain.h"

namespace pipeeq {

// Simple ring buffer of interleaved float frames. The producer (capture
// stream's process callback) and consumer (this route's own playback process
// callback) both run on the same pw_thread_loop, so calls are serialized by
// PipeWire rather than truly concurrent - a plain circular buffer is enough,
// no atomics required for the indices themselves.
class InterleavedRingBuffer {
public:
    InterleavedRingBuffer(std::size_t capacityFrames, int numChannels);

    void write(const float* interleaved, std::size_t frames);
    // Reads up to `frames`; any shortfall (underrun) is filled with silence.
    void read(float* outInterleaved, std::size_t frames);

private:
    int numChannels_;
    std::size_t capacityFrames_;
    std::vector<float> buffer_;
    std::size_t writePos_ = 0;
    std::size_t available_ = 0;
};

// Snapshot of a route's state, for callers that just want to list/report it
// (e.g. the future D-Bus ListRoutes/GetState handlers).
struct RouteInfo {
    std::string id;
    std::string deviceName;
    std::string displayName;
    double gainDb = 0.0;
    bool muted = false;
    std::size_t bandCount = 0;
};

// One physical output route: a playback stream pinned to a target device,
// with its own gain/mute and parametric EqChain.
//
// Mutators (setGainDb/setMuted/setBandCount/setBand) are only ever called
// by AudioEngine while holding pw_thread_loop_lock(), and onProcess() only
// ever runs on the loop's own thread during dispatch - pw_thread_loop
// guarantees those two never overlap, so the plain (non-atomic) fields
// below are safe without extra synchronization. gainLinear_ stays atomic
// mainly as a documentation cue for the one field actually read from the
// RT callback.
class OutputRoute {
public:
    OutputRoute(pw_core* core, pw_thread_loop* loop, std::string id, std::string deviceName,
                std::string displayName, uint32_t targetNodeId, int numChannels, uint32_t sampleRateHz);
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
    std::vector<eqcore::EqBand> bands() const;

    RouteInfo info() const;

    // Called from the capture stream's process callback (same thread as this
    // route's own process callback, so no locking needed).
    void pushCaptured(const float* interleaved, std::size_t frames);

    // Public because they're referenced from a file-scope pw_stream_events
    // struct in output_route.cpp; not part of the intended public API otherwise.
    static void onProcess(void* userdata);
    static void onStateChanged(void* userdata, pw_stream_state old, pw_stream_state state,
                                const char* error);

private:
    std::string id_;
    std::string deviceName_;
    std::string displayName_;
    int numChannels_;

    pw_stream* stream_ = nullptr;
    spa_hook listener_{};

    InterleavedRingBuffer ring_;
    eqcore::EqChain chain_;
    std::atomic<float> gainLinear_{1.0f};
    double gainDb_ = 0.0;
    bool muted_ = false;
};

} // namespace pipeeq
