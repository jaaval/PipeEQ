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
    std::size_t pos = writePos_;
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t slot = (pos + i) % capacityFrames_;
        for (int ch = 0; ch < numChannels_; ++ch) {
            buffer_[slot * numChannels_ + ch] = interleaved[i * numChannels_ + ch];
        }
    }
    pos += frames;
    writePos_ = pos;
    writeIndex_.store(pos, std::memory_order_release);
}

void InterleavedRingBuffer::readAt(std::size_t& cursor, float* outInterleaved, std::size_t frames) const {
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
        for (int ch = 0; ch < numChannels_; ++ch) {
            outInterleaved[i * numChannels_ + ch] = buffer_[slot * numChannels_ + ch];
        }
    }
    for (std::size_t i = toRead; i < frames; ++i) {
        for (int ch = 0; ch < numChannels_; ++ch) {
            outInterleaved[i * numChannels_ + ch] = 0.0f;
        }
    }

    cursor += toRead;
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
      sampleRateHz_(sampleRateHz),
      bandState_(static_cast<std::size_t>(numChannels)),
      mixScratch_(kScratchCapacityFrames * static_cast<std::size_t>(numChannels), 0.0f) {
    snapshot_.store(std::make_shared<const RouteSnapshot>());

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
    rebuildSnapshot();
}

void OutputRoute::setMuted(bool muted) {
    muted_ = muted;
    rebuildSnapshot();
}

void OutputRoute::setBandCount(std::size_t count) {
    count = std::min(count, kMaxBands);
    bands_.resize(count);
    rebuildSnapshot();
}

void OutputRoute::setBand(std::size_t index, const eqcore::EqBand& band) {
    bands_.at(index) = band;
    rebuildSnapshot();
}

std::vector<eqcore::EqBand> OutputRoute::bands() const {
    return bands_;
}

int OutputRoute::findSlot(const std::string& inputId) const {
    for (std::size_t i = 0; i < inputSlotsMirror_.size(); ++i) {
        if (inputSlotsMirror_[i].active && inputSlotsMirror_[i].inputId == inputId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool OutputRoute::setInputGainDb(const std::string& inputId, std::shared_ptr<const InterleavedRingBuffer> buffer,
                                  double gainDb) {
    int slot = findSlot(inputId);
    if (slot < 0) {
        for (std::size_t i = 0; i < inputSlotsMirror_.size(); ++i) {
            if (!inputSlotsMirror_[i].active) {
                slot = static_cast<int>(i);
                inputSlotsMirror_[i].active = true;
                inputSlotsMirror_[i].inputId = inputId;
                inputSlotsMirror_[i].buffer = buffer;
                // Start "live" (no backlog) rather than replaying history
                // or immediately triggering the overrun-jump path.
                readCursors_[i] = buffer ? buffer->currentWriteIndex() : 0;
                break;
            }
        }
        if (slot < 0) {
            return false;
        }
    }

    inputSlotsMirror_[static_cast<std::size_t>(slot)].gainLinear =
        static_cast<float>(std::pow(10.0, gainDb / 20.0));
    rebuildSnapshot();
    return true;
}

void OutputRoute::removeInputSlot(const std::string& inputId) {
    const int slot = findSlot(inputId);
    if (slot < 0) {
        return;
    }
    inputSlotsMirror_[static_cast<std::size_t>(slot)] = InputMixSlot{};
    rebuildSnapshot();
}

std::vector<std::pair<std::string, double>> OutputRoute::inputGainsDb() const {
    std::vector<std::pair<std::string, double>> result;
    for (const auto& slot : inputSlotsMirror_) {
        if (slot.active) {
            result.emplace_back(slot.inputId, 20.0 * std::log10(static_cast<double>(slot.gainLinear)));
        }
    }
    return result;
}

RouteInfo OutputRoute::info() const {
    return RouteInfo{id_, deviceName_, displayName_, gainDb_, muted_, bands_.size()};
}

void OutputRoute::rebuildSnapshot() {
    auto next = std::make_shared<RouteSnapshot>();

    next->coeffs.reserve(bands_.size());
    for (const auto& band : bands_) {
        next->coeffs.push_back(band.toCoeffs(static_cast<double>(sampleRateHz_)));
    }

    next->masterGainLinear = static_cast<float>(std::pow(10.0, gainDb_ / 20.0));
    next->muted = muted_;
    next->inputs = inputSlotsMirror_;

    snapshot_.store(std::move(next), std::memory_order_release);
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

    const int numChannels = self->numChannels_;
    const uint32_t stride = static_cast<uint32_t>(sizeof(float)) * static_cast<uint32_t>(numChannels);
    uint32_t frames = buf->datas[0].maxsize / stride;
    if (b->requested != 0 && b->requested < frames) {
        frames = static_cast<uint32_t>(b->requested);
    }
    frames = std::min(frames, static_cast<uint32_t>(OutputRoute::kScratchCapacityFrames));

    const std::shared_ptr<const RouteSnapshot> snap = self->snapshot_.load(std::memory_order_acquire);
    const uint32_t sampleCount = frames * static_cast<uint32_t>(numChannels);

    std::fill(dst, dst + sampleCount, 0.0f);

    for (std::size_t i = 0; i < snap->inputs.size(); ++i) {
        const InputMixSlot& slot = snap->inputs[i];
        if (!slot.active || !slot.buffer || slot.gainLinear == 0.0f) {
            continue;
        }
        slot.buffer->readAt(self->readCursors_[i], self->mixScratch_.data(), frames);
        const float gain = slot.gainLinear;
        for (uint32_t s = 0; s < sampleCount; ++s) {
            dst[s] += self->mixScratch_[s] * gain;
        }
    }

    // Safety clamp against multi-input summing overrun.
    for (uint32_t s = 0; s < sampleCount; ++s) {
        dst[s] = std::clamp(dst[s], -1.0f, 1.0f);
    }

    const float masterGain = snap->muted ? 0.0f : snap->masterGainLinear;
    for (uint32_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < numChannels; ++ch) {
            float sample = dst[i * numChannels + ch];
            auto& channelState = self->bandState_[static_cast<std::size_t>(ch)];
            for (std::size_t band = 0; band < snap->coeffs.size(); ++band) {
                sample = channelState[band].process(snap->coeffs[band], sample);
            }
            dst[i * numChannels + ch] = sample * masterGain;
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
