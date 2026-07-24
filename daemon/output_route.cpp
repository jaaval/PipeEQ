#include "output_route.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

namespace pipeeq {

InterleavedRingBuffer::InterleavedRingBuffer(std::size_t capacityFrames, int numChannels)
    : numChannels_(numChannels),
      capacityFrames_(capacityFrames),
      buffer_(capacityFrames * static_cast<std::size_t>(numChannels), 0.0f) {}

void InterleavedRingBuffer::write(const float* interleaved, std::size_t frames) {
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t slot = (writePos_ + i) % capacityFrames_;
        for (int ch = 0; ch < numChannels_; ++ch) {
            buffer_[slot * numChannels_ + ch] = interleaved[i * numChannels_ + ch];
        }
    }
    writePos_ = (writePos_ + frames) % capacityFrames_;
    available_ = std::min(available_ + frames, capacityFrames_);
}

void InterleavedRingBuffer::read(float* outInterleaved, std::size_t frames) {
    const std::size_t toRead = std::min(frames, available_);
    const std::size_t readPos = (writePos_ + capacityFrames_ - available_) % capacityFrames_;

    for (std::size_t i = 0; i < toRead; ++i) {
        const std::size_t slot = (readPos + i) % capacityFrames_;
        for (int ch = 0; ch < numChannels_; ++ch) {
            outInterleaved[i * numChannels_ + ch] = buffer_[slot * numChannels_ + ch];
        }
    }
    for (std::size_t i = toRead; i < frames; ++i) {
        for (int ch = 0; ch < numChannels_; ++ch) {
            outInterleaved[i * numChannels_ + ch] = 0.0f;
        }
    }
    available_ -= toRead;
}

namespace {

const pw_stream_events kOutputRouteStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = OutputRoute::onStateChanged,
    .process = OutputRoute::onProcess,
};

} // namespace

OutputRoute::OutputRoute(pw_core* core, pw_thread_loop* /*loop*/, std::string id, std::string deviceName,
                          std::string displayName, uint32_t targetNodeId, int numChannels,
                          uint32_t sampleRateHz)
    : id_(std::move(id)),
      deviceName_(std::move(deviceName)),
      displayName_(std::move(displayName)),
      numChannels_(numChannels),
      ring_(sampleRateHz / 2, numChannels),
      chain_(numChannels, static_cast<double>(sampleRateHz)) {
    const std::string nodeName = "pipeeq_route_" + id_;

    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                                               PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_NAME, nodeName.c_str(),
                                               PW_KEY_NODE_DESCRIPTION, displayName_.c_str(), nullptr);

    stream_ = pw_stream_new(core, ("pipeeq route " + displayName_).c_str(), props);
    pw_stream_add_listener(stream_, &listener_, &kOutputRouteStreamEvents, this);

    uint8_t buffer[1024];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = static_cast<uint32_t>(numChannels_);
    info.rate = sampleRateHz;

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&podBuilder, SPA_PARAM_EnumFormat, &info);

    pw_stream_connect(
        stream_, PW_DIRECTION_OUTPUT, targetNodeId,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                      PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
}

OutputRoute::~OutputRoute() {
    if (stream_) {
        pw_stream_destroy(stream_);
    }
}

void OutputRoute::setGainDb(double gainDb) {
    gainDb_ = gainDb;
    gainLinear_.store(static_cast<float>(std::pow(10.0, gainDb / 20.0)), std::memory_order_relaxed);
}

void OutputRoute::setMuted(bool muted) {
    muted_ = muted;
}

void OutputRoute::setBandCount(std::size_t count) {
    chain_.setBandCount(count);
}

void OutputRoute::setBand(std::size_t index, const eqcore::EqBand& band) {
    chain_.setBand(index, band);
}

std::vector<eqcore::EqBand> OutputRoute::bands() const {
    std::vector<eqcore::EqBand> result;
    result.reserve(chain_.bandCount());
    for (std::size_t i = 0; i < chain_.bandCount(); ++i) {
        result.push_back(chain_.band(i));
    }
    return result;
}

RouteInfo OutputRoute::info() const {
    return RouteInfo{id_, deviceName_, displayName_, gainDb_, muted_, chain_.bandCount()};
}

void OutputRoute::pushCaptured(const float* interleaved, std::size_t frames) {
    ring_.write(interleaved, frames);
}

void OutputRoute::onProcess(void* userdata) {
    auto* self = static_cast<OutputRoute*>(userdata);

    pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
    if (!b) {
        return;
    }

    spa_buffer* buf = b->buffer;
    float* dst = static_cast<float*>(buf->datas[0].data);
    if (!dst) {
        pw_stream_queue_buffer(self->stream_, b);
        return;
    }

    const uint32_t stride = static_cast<uint32_t>(sizeof(float)) * static_cast<uint32_t>(self->numChannels_);
    uint32_t frames = buf->datas[0].maxsize / stride;
    if (b->requested != 0 && b->requested < frames) {
        frames = static_cast<uint32_t>(b->requested);
    }

    self->ring_.read(dst, frames);

    const float gain = self->muted_ ? 0.0f : self->gainLinear_.load(std::memory_order_relaxed);
    const int numChannels = self->numChannels_;
    for (uint32_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < numChannels; ++ch) {
            float& sample = dst[i * numChannels + ch];
            sample = self->chain_.processSample(ch, sample) * gain;
        }
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = static_cast<int32_t>(stride);
    buf->datas[0].chunk->size = frames * stride;

    pw_stream_queue_buffer(self->stream_, b);
}

void OutputRoute::onStateChanged(void* /*userdata*/, pw_stream_state /*old*/, pw_stream_state state,
                                  const char* error) {
    if (state == PW_STREAM_STATE_ERROR) {
        std::fprintf(stderr, "pipeeq: output route stream error: %s\n", error ? error : "(unknown)");
    }
}

} // namespace pipeeq
