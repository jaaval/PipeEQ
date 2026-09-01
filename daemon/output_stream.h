#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pipewire/pipewire.h>

#include "output_processor.h"

namespace pipeeq {

// One physical output: a PipeWire playback stream pinned to a target device,
// carrying ALL of that device's channels, wrapped around the OutputProcessor
// that does the actual work.
//
// This class is deliberately nothing but PipeWire glue - connect, format
// negotiation, dequeue/queue - so that every line of DSP lives in
// OutputProcessor and can be tested without a graph.
//
// The stream is always negotiated with the DEVICE'S OWN advertised layout,
// never with the user's logical channel-position assignment. If the user
// relabels physical channel 3 as "FC" for routing purposes, that is
// control-plane metadata: it changes which input channels the mix planner
// matches into that channel and nothing else. Declaring "FC" on the stream
// instead would break the 1:1 port link, because the device has no FC port.
// The consequence is worth stating plainly: changing a channel's position is a
// snapshot rebuild, not a reconnect. Only a change to the DEVICE's layout - a
// profile switch - forces renegotiation.
//
// This object exists only while its target device does; the output's
// user-visible configuration is owned by AudioEngine and outlives it, so a
// device disappearing tears down the stream without losing any settings.
class OutputStream {
public:
    // `devicePositions` is the layout to negotiate: the device's own
    // audio.position names, or a conventional layout for its channel count if
    // it advertises none.
    OutputStream(pw_core* core, std::string id, std::string deviceName, std::string displayName,
                  uint32_t targetNodeId, std::vector<std::string> devicePositions,
                  uint32_t requestedRateHz);
    ~OutputStream();

    OutputStream(const OutputStream&) = delete;
    OutputStream& operator=(const OutputStream&) = delete;

    const std::string& id() const { return id_; }
    const std::string& deviceName() const { return deviceName_; }
    const std::string& displayName() const { return displayName_; }

    // The layout this stream ASKED for. Compared against the device's current
    // layout to detect a profile switch.
    const std::vector<std::string>& requestedPositions() const { return requestedPositions_; }

    // What was actually negotiated. Zero channels means negotiation hasn't
    // completed yet, in which case process() emits silence.
    uint32_t negotiatedChannels() const { return negotiatedChannels_.load(std::memory_order_acquire); }
    uint32_t negotiatedRateHz() const { return negotiatedRateHz_.load(std::memory_order_acquire); }
    std::vector<std::string> negotiatedPositions() const;

    // True once since the last call: the negotiated format changed, so the
    // caller must rebuild the mix plan and the EQ coefficients for the real
    // layout and sample rate.
    //
    // A flag consumed by the control thread rather than a callback, on purpose.
    // param_changed runs on PipeWire's loop thread with the thread-loop lock
    // held, and AudioEngine's documented lock order is controlMutex_ before
    // pw_thread_loop_lock() - so calling back into the engine from here would
    // invert it and deadlock.
    bool consumeFormatChanged() { return formatChanged_.exchange(false, std::memory_order_acq_rel); }

    void publish(std::unique_ptr<const OutputSnapshot> snapshot) {
        processor_.publish(std::move(snapshot));
    }
    float takeChannelPeak(std::size_t channel) { return processor_.takeChannelPeak(channel); }
    OutputProcessor& processor() { return processor_; }

    static void onProcess(void* userdata);
    static void onParamChanged(void* userdata, uint32_t id, const spa_pod* param);
    static void onStateChanged(void* userdata, pw_stream_state old, pw_stream_state state,
                                const char* error);

private:
    std::string id_;
    std::string deviceName_;
    std::string displayName_;
    std::vector<std::string> requestedPositions_;

    pw_stream* stream_ = nullptr;
    spa_hook listener_{};

    // Written on PipeWire's loop thread in param_changed, read on the control
    // thread. Atomics for the scalars; the position list needs a mutex, and is
    // only ever read by the control thread outside the audio path.
    std::atomic<uint32_t> negotiatedChannels_{0};
    std::atomic<uint32_t> negotiatedRateHz_{0};
    std::atomic<bool> formatChanged_{false};
    mutable std::mutex negotiatedMutex_;
    std::vector<std::string> negotiatedPositions_;

    OutputProcessor processor_;
};

} // namespace pipeeq
