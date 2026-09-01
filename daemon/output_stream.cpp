#include "output_stream.h"

#include <algorithm>
#include <cstdio>

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include "audio_format.h"
#include "channel_layout.h"
#include "rt_limits.h"

namespace pipeeq {

namespace {

const pw_stream_events kOutputStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = OutputStream::onStateChanged,
    .param_changed = OutputStream::onParamChanged,
    .process = OutputStream::onProcess,
};

} // namespace

OutputStream::OutputStream(pw_core* core, std::string id, std::string deviceName,
                            std::string displayName, uint32_t targetNodeId,
                            std::vector<std::string> devicePositions, uint32_t requestedRateHz)
    : id_(std::move(id)),
      deviceName_(std::move(deviceName)),
      displayName_(std::move(displayName)),
      requestedPositions_(std::move(devicePositions)),
      processor_(requestedRateHz) {
    if (requestedPositions_.empty()) {
        requestedPositions_ = layout::defaultPositionsFor(2);
    }
    if (requestedPositions_.size() > kMaxOutputChannels) {
        std::fprintf(stderr,
                     "pipeeq: device '%s' reports %zu channels; PipeEQ drives at most %zu, so the "
                     "rest will not be used\n",
                     deviceName_.c_str(), requestedPositions_.size(), kMaxOutputChannels);
        requestedPositions_.resize(kMaxOutputChannels);
    }

    const std::string nodeName = "pipeeq_route_" + id_;
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_CLASS,
        "Stream/Output/Audio", PW_KEY_NODE_NAME, nodeName.c_str(), PW_KEY_NODE_DESCRIPTION,
        displayName_.c_str(), nullptr);

    // Without this PipeWire's channelmix will happily up/downmix our stream
    // across the device's ports - which is how a single stereo stream ended up
    // audible on all four outputs of a 4.0 interface. Our layout is identical
    // to the device's, so the channelmix would be an identity anyway; this is
    // belt-and-braces, and cheap.
    pw_properties_set(props, PW_KEY_STREAM_DONT_REMIX, "true");

    stream_ = pw_stream_new(core, ("pipeeq output " + displayName_).c_str(), props);
    pw_stream_add_listener(stream_, &listener_, &kOutputStreamEvents, this);

    uint8_t buffer[4096];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = static_cast<uint32_t>(requestedPositions_.size());
    info.rate = requestedRateHz;
    applyChannelPositions(info, requestedPositions_);

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&podBuilder, SPA_PARAM_EnumFormat, &info);

    pw_stream_connect(
        stream_, PW_DIRECTION_OUTPUT, targetNodeId,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                      PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
}

OutputStream::~OutputStream() {
    // Destroying the stream first guarantees no further process() callback, so
    // the processor's destructor can safely free everything it retired.
    if (stream_) {
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
}

std::vector<std::string> OutputStream::negotiatedPositions() const {
    std::lock_guard<std::mutex> lock(negotiatedMutex_);
    return negotiatedPositions_;
}

void OutputStream::onParamChanged(void* userdata, uint32_t id, const spa_pod* param) {
    auto* self = static_cast<OutputStream*>(userdata);
    if (!param || id != SPA_PARAM_Format) {
        return;
    }

    spa_audio_info info{};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) {
        return;
    }
    if (info.media_type != SPA_MEDIA_TYPE_audio || info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }
    if (spa_format_audio_raw_parse(param, &info.info.raw) < 0) {
        return;
    }

    // What we ASKED for is not necessarily what we GOT. The previous engine
    // assumed it was and computed its buffer stride from the requested channel
    // count - which at two channels never bit, but with N channels means a
    // wrong stride: garbled audio or a read past the end of the buffer.
    const uint32_t channels = std::min<uint32_t>(info.info.raw.channels, kMaxOutputChannels);

    std::vector<std::string> positions;
    positions.reserve(channels);
    for (uint32_t i = 0; i < channels; ++i) {
        positions.push_back(layout::positionName(info.info.raw.position[i]));
    }

    {
        std::lock_guard<std::mutex> lock(self->negotiatedMutex_);
        self->negotiatedPositions_ = positions;
    }
    self->negotiatedRateHz_.store(info.info.raw.rate, std::memory_order_release);
    self->negotiatedChannels_.store(channels, std::memory_order_release);
    self->formatChanged_.store(true, std::memory_order_release);

    // Logged unconditionally: when a device negotiates something other than
    // what was requested, this line is the only way to diagnose it from a
    // user's log.
    std::string layoutText;
    for (const std::string& name : positions) {
        layoutText += layoutText.empty() ? name : ("," + name);
    }
    std::fprintf(stderr, "pipeeq: output '%s' negotiated %u ch @ %u Hz [%s]\n",
                 self->displayName_.c_str(), info.info.raw.channels, info.info.raw.rate,
                 layoutText.c_str());
    if (info.info.raw.channels != self->requestedPositions_.size()) {
        std::fprintf(stderr,
                     "pipeeq: (asked for %zu channels; using the negotiated %u)\n",
                     self->requestedPositions_.size(), info.info.raw.channels);
    }
}

void OutputStream::onProcess(void* userdata) {
    auto* self = static_cast<OutputStream*>(userdata);

    pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
    if (!b) {
        return;
    }

    spa_buffer* buf = b->buffer;
    float* dst = static_cast<float*>(buf->datas[0].data);
    const uint32_t numChannels = self->negotiatedChannels_.load(std::memory_order_acquire);
    if (!dst || numChannels == 0) {
        // Not negotiated yet: hand the buffer back untouched rather than
        // writing with a stride we can't know.
        pw_stream_queue_buffer(self->stream_, b);
        return;
    }

    const uint32_t stride = static_cast<uint32_t>(sizeof(float)) * numChannels;
    uint32_t frames = buf->datas[0].maxsize / stride;
    if (b->requested != 0 && b->requested < frames) {
        frames = static_cast<uint32_t>(b->requested);
    }

    self->processor_.process(dst, frames, numChannels);

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = static_cast<int32_t>(stride);
    buf->datas[0].chunk->size = frames * stride;

    pw_stream_queue_buffer(self->stream_, b);
}

void OutputStream::onStateChanged(void* userdata, pw_stream_state /*old*/, pw_stream_state state,
                                   const char* error) {
    auto* self = static_cast<OutputStream*>(userdata);
    if (state == PW_STREAM_STATE_ERROR) {
        std::fprintf(stderr, "pipeeq: output '%s' stream error: %s\n", self->displayName_.c_str(),
                     error ? error : "unknown");
    }
}

} // namespace pipeeq
