#include "input_source.h"

#include <atomic>
#include <cstdio>

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include "audio_format.h"
#include "channel_layout.h"

namespace pipeeq {

namespace {

const pw_stream_events kInputSourceStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = InputSource::onStateChanged,
    .process = InputSource::onProcess,
};

} // namespace

InputSource::InputSource(pw_core* core, std::string id, std::string displayName,
                          std::vector<std::string> positions, uint32_t sampleRateHz)
    : id_(std::move(id)),
      displayName_(std::move(displayName)),
      identity_(nextIdentity()),
      numChannels_(static_cast<int>(positions.size())),
      positions_(std::move(positions)),
      ringBuffer_(std::make_shared<RingBuffer>(sampleRateHz / 2, static_cast<int>(positions_.size()))) {
    const int numChannels = numChannels_;
    const std::string nodeName = "pipeeq_input_" + id_;

    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_CLASS,
                           "Audio/Sink", PW_KEY_NODE_NAME, nodeName.c_str(), PW_KEY_NODE_DESCRIPTION,
                           displayName_.c_str(), nullptr);

    stream_ = pw_stream_new(core, ("pipeeq input " + displayName_).c_str(), props);
    pw_stream_add_listener(stream_, &listener_, &kInputSourceStreamEvents, this);

    uint8_t buffer[1024];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = static_cast<uint32_t>(numChannels);
    info.rate = sampleRateHz;
    // Makes this advertise a real stereo pair, so applications and volume
    // controls see front-left/front-right instead of aux0/aux1.
    applyChannelPositions(info, positions_);

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&podBuilder, SPA_PARAM_EnumFormat, &info);

    pw_stream_connect(
        stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                      PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
}

// Monotonic and never reused, so an id reappearing on a different object still
// looks like a different occupant to the realtime thread.
uint64_t InputSource::nextIdentity() {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

InputSource::~InputSource() {
    if (stream_) {
        pw_stream_destroy(stream_);
    }
}

void InputSource::onProcess(void* userdata) {
    auto* self = static_cast<InputSource*>(userdata);

    pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
    if (!b) {
        return;
    }

    spa_buffer* buf = b->buffer;
    const float* samples = static_cast<const float*>(buf->datas[0].data);
    if (samples && buf->datas[0].chunk) {
        const uint32_t stride =
            static_cast<uint32_t>(sizeof(float)) * static_cast<uint32_t>(self->ringBuffer_->numChannels());
        const uint32_t frames = buf->datas[0].chunk->size / stride;
        self->ringBuffer_->write(samples, frames);
    }

    pw_stream_queue_buffer(self->stream_, b);
}

void InputSource::onStateChanged(void* /*userdata*/, pw_stream_state /*old*/, pw_stream_state state,
                                  const char* error) {
    if (state == PW_STREAM_STATE_ERROR) {
        std::fprintf(stderr, "pipeeq: input source stream error: %s\n", error ? error : "(unknown)");
    }
}

} // namespace pipeeq
