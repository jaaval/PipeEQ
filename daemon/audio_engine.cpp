#include "audio_engine.h"

#include <algorithm>
#include <cstdio>
#include <string_view>

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/dict.h>

namespace pipeeq {

namespace {

const pw_stream_events kCaptureStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = AudioEngine::onCaptureStateChanged,
    .process = AudioEngine::onCaptureProcess,
};

const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = AudioEngine::onRegistryGlobal,
    .global_remove = AudioEngine::onRegistryGlobalRemove,
};

} // namespace

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    stop();
}

void AudioEngine::start() {
    loop_ = pw_thread_loop_new("pipeeq-loop", nullptr);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);

    pw_thread_loop_start(loop_);

    pw_thread_loop_lock(loop_);

    core_ = pw_context_connect(context_, nullptr, 0);

    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(registry_, &registryListener_, &kRegistryEvents, this);

    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_CLASS,
                           "Audio/Sink", PW_KEY_NODE_NAME, "pipeeq_sink", PW_KEY_NODE_DESCRIPTION,
                           "PipeEQ Virtual Sink", nullptr);

    captureStream_ = pw_stream_new(core_, "pipeeq capture", props);
    pw_stream_add_listener(captureStream_, &captureListener_, &kCaptureStreamEvents, this);

    uint8_t buffer[1024];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = static_cast<uint32_t>(kNumChannels);
    info.rate = kSampleRateHz;

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&podBuilder, SPA_PARAM_EnumFormat, &info);

    pw_stream_connect(
        captureStream_, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                      PW_STREAM_FLAG_RT_PROCESS),
        params, 1);

    pw_thread_loop_unlock(loop_);
}

void AudioEngine::stop() {
    if (!loop_) {
        return;
    }

    pw_thread_loop_lock(loop_);
    routes_.clear();
    if (captureStream_) {
        pw_stream_destroy(captureStream_);
        captureStream_ = nullptr;
    }
    if (core_) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    pw_thread_loop_unlock(loop_);

    pw_thread_loop_stop(loop_);

    if (context_) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
}

std::vector<DeviceInfo> AudioEngine::listDevices() const {
    std::lock_guard<std::mutex> lock(devicesMutex_);
    return devices_;
}

std::string AudioEngine::addRoute(const std::string& deviceName, const std::string& displayName,
                                   double gainDb) {
    uint32_t targetId = PW_ID_ANY;
    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        auto it = std::find_if(devices_.begin(), devices_.end(),
                                [&](const DeviceInfo& d) { return d.nodeName == deviceName; });
        if (it == devices_.end()) {
            return {};
        }
        targetId = it->id;
    }

    const std::string routeId = "route-" + std::to_string(nextRouteIndex_++);
    const std::string label = displayName.empty() ? deviceName : displayName;

    pw_thread_loop_lock(loop_);
    auto route = std::make_unique<OutputRoute>(core_, loop_, routeId, deviceName, label, targetId,
                                                kNumChannels, kSampleRateHz);
    if (gainDb != 0.0) {
        route->setGainDb(gainDb);
    }
    routes_.push_back(std::move(route));
    pw_thread_loop_unlock(loop_);

    return routeId;
}

void AudioEngine::removeRoute(const std::string& routeId) {
    pw_thread_loop_lock(loop_);
    routes_.erase(std::remove_if(routes_.begin(), routes_.end(),
                                  [&](const std::unique_ptr<OutputRoute>& r) { return r->id() == routeId; }),
                  routes_.end());
    pw_thread_loop_unlock(loop_);
}

std::vector<RouteInfo> AudioEngine::listRoutes() const {
    std::vector<RouteInfo> result;
    pw_thread_loop_lock(loop_);
    result.reserve(routes_.size());
    for (const auto& route : routes_) {
        result.push_back(route->info());
    }
    pw_thread_loop_unlock(loop_);
    return result;
}

OutputRoute* AudioEngine::findRoute(const std::string& routeId) {
    auto it = std::find_if(routes_.begin(), routes_.end(),
                            [&](const std::unique_ptr<OutputRoute>& r) { return r->id() == routeId; });
    return it == routes_.end() ? nullptr : it->get();
}

bool AudioEngine::setRouteGain(const std::string& routeId, double gainDb) {
    pw_thread_loop_lock(loop_);
    OutputRoute* route = findRoute(routeId);
    if (route) {
        route->setGainDb(gainDb);
    }
    pw_thread_loop_unlock(loop_);
    return route != nullptr;
}

bool AudioEngine::setRouteMuted(const std::string& routeId, bool muted) {
    pw_thread_loop_lock(loop_);
    OutputRoute* route = findRoute(routeId);
    if (route) {
        route->setMuted(muted);
    }
    pw_thread_loop_unlock(loop_);
    return route != nullptr;
}

bool AudioEngine::setRouteBandCount(const std::string& routeId, std::size_t count) {
    pw_thread_loop_lock(loop_);
    OutputRoute* route = findRoute(routeId);
    if (route) {
        route->setBandCount(count);
    }
    pw_thread_loop_unlock(loop_);
    return route != nullptr;
}

bool AudioEngine::setRouteBand(const std::string& routeId, std::size_t index, const eqcore::EqBand& band) {
    pw_thread_loop_lock(loop_);
    OutputRoute* route = findRoute(routeId);
    if (route) {
        route->setBand(index, band);
    }
    pw_thread_loop_unlock(loop_);
    return route != nullptr;
}

std::vector<eqcore::EqBand> AudioEngine::getRouteBands(const std::string& routeId) const {
    std::vector<eqcore::EqBand> result;
    pw_thread_loop_lock(loop_);
    for (const auto& route : routes_) {
        if (route->id() == routeId) {
            result = route->bands();
            break;
        }
    }
    pw_thread_loop_unlock(loop_);
    return result;
}

void AudioEngine::distributeToRoutes(const float* interleaved, std::size_t frames) {
    for (auto& route : routes_) {
        route->pushCaptured(interleaved, frames);
    }
}

void AudioEngine::onRegistryGlobal(void* userdata, uint32_t id, uint32_t /*permissions*/, const char* type,
                                    uint32_t /*version*/, const spa_dict* props) {
    auto* self = static_cast<AudioEngine*>(userdata);
    if (!props || std::string_view(type) != PW_TYPE_INTERFACE_Node) {
        return;
    }

    const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!mediaClass || std::string_view(mediaClass) != "Audio/Sink") {
        return;
    }

    const char* nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!nodeName) {
        return;
    }

    // Skip nodes this daemon itself created.
    const std::string_view nodeNameView(nodeName);
    if (nodeNameView == "pipeeq_sink" || nodeNameView.starts_with("pipeeq_route")) {
        return;
    }

    const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

    std::lock_guard<std::mutex> lock(self->devicesMutex_);
    self->devices_.push_back(DeviceInfo{id, nodeName, description ? description : nodeName});
}

void AudioEngine::onRegistryGlobalRemove(void* userdata, uint32_t id) {
    auto* self = static_cast<AudioEngine*>(userdata);
    std::lock_guard<std::mutex> lock(self->devicesMutex_);
    self->devices_.erase(
        std::remove_if(self->devices_.begin(), self->devices_.end(), [id](const DeviceInfo& d) { return d.id == id; }),
        self->devices_.end());
}

void AudioEngine::onCaptureProcess(void* userdata) {
    auto* self = static_cast<AudioEngine*>(userdata);

    pw_buffer* b = pw_stream_dequeue_buffer(self->captureStream_);
    if (!b) {
        return;
    }

    spa_buffer* buf = b->buffer;
    float* samples = static_cast<float*>(buf->datas[0].data);
    if (samples && buf->datas[0].chunk) {
        const uint32_t stride = static_cast<uint32_t>(sizeof(float)) * static_cast<uint32_t>(kNumChannels);
        const uint32_t frames = buf->datas[0].chunk->size / stride;
        self->distributeToRoutes(samples, frames);
    }

    pw_stream_queue_buffer(self->captureStream_, b);
}

void AudioEngine::onCaptureStateChanged(void* /*userdata*/, pw_stream_state /*old*/, pw_stream_state state,
                                         const char* error) {
    if (state == PW_STREAM_STATE_ERROR) {
        std::fprintf(stderr, "pipeeq: capture stream error: %s\n", error ? error : "(unknown)");
    }
}

} // namespace pipeeq
