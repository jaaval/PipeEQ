#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <pipewire/pipewire.h>

#include "ring_buffer.h"

namespace pipeeq {

// One audio input: a capture stream masquerading as a virtual sink that apps
// can be assigned to ("PipeEQ Music", "PipeEQ Voice Chat", ...), whose captured
// audio is written into a single shared ring buffer.
//
// Any number of outputs can independently read from that same buffer, each with
// its own cursor (see RingBuffer), so this class never needs to know which
// outputs are subscribed to it; AudioEngine wires that up by putting a copy of
// ringBuffer() into an output's snapshot.
class InputSource {
public:
    // `positions` is the sink's channel layout as SPA short names; its size is
    // the channel count. Fixed for the object's life.
    InputSource(pw_core* core, std::string id, std::string displayName,
                std::vector<std::string> positions, uint32_t sampleRateHz);
    ~InputSource();

    InputSource(const InputSource&) = delete;
    InputSource& operator=(const InputSource&) = delete;

    const std::string& id() const { return id_; }
    const std::string& displayName() const { return displayName_; }
    void setDisplayName(std::string displayName) { displayName_ = std::move(displayName); }
    int numChannels() const { return numChannels_; }

    // The channel layout this sink advertises, as SPA short names. Fixed at
    // construction: changing it means recreating the stream and its ring
    // buffer, which every live output holds a shared_ptr to.
    const std::vector<std::string>& positions() const { return positions_; }

    std::shared_ptr<const RingBuffer> ringBuffer() const { return ringBuffer_; }

    static void onProcess(void* userdata);
    static void onStateChanged(void* userdata, pw_stream_state old, pw_stream_state state,
                                const char* error);

private:
    std::string id_;
    std::string displayName_;
    int numChannels_;
    std::vector<std::string> positions_;

    pw_stream* stream_ = nullptr;
    spa_hook listener_{};

    std::shared_ptr<RingBuffer> ringBuffer_;
};

} // namespace pipeeq
