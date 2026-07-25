#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <pipewire/pipewire.h>

#include "output_route.h"

namespace pipeeq {

// One audio input: a capture stream (today always a new virtual sink apps
// can be assigned to - "PipeEQ Music", "PipeEQ Voice Chat", etc.) whose
// captured audio is written into a single shared ring buffer. Any number
// of OutputRoutes can independently read from that same buffer (each with
// its own cursor - see InterleavedRingBuffer), so this class never needs
// to know which routes are subscribed to it; AudioEngine wires that up by
// handing OutputRoute a copy of ringBuffer() when a route subscribes.
class InputSource {
public:
    InputSource(pw_core* core, std::string id, std::string displayName, int numChannels,
                uint32_t sampleRateHz);
    ~InputSource();

    InputSource(const InputSource&) = delete;
    InputSource& operator=(const InputSource&) = delete;

    const std::string& id() const { return id_; }
    const std::string& displayName() const { return displayName_; }

    std::shared_ptr<const InterleavedRingBuffer> ringBuffer() const { return ringBuffer_; }

    static void onProcess(void* userdata);
    static void onStateChanged(void* userdata, pw_stream_state old, pw_stream_state state,
                                const char* error);

private:
    std::string id_;
    std::string displayName_;

    pw_stream* stream_ = nullptr;
    spa_hook listener_{};

    std::shared_ptr<InterleavedRingBuffer> ringBuffer_;
};

} // namespace pipeeq
