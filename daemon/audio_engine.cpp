#include "audio_engine.h"

#include <algorithm>
#include <cstdio>
#include <string_view>

#include <spa/utils/dict.h>

namespace pipeeq {

namespace {

const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = AudioEngine::onRegistryGlobal,
    .global_remove = AudioEngine::onRegistryGlobalRemove,
};

const pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .done = AudioEngine::onCoreDone,
};

const pw_node_events kNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = AudioEngine::onNodeInfo,
};

// How long start() waits for PipeWire to finish the initial registry
// enumeration before giving up and carrying on with whatever arrived.
constexpr int kRegistrySyncTimeoutSec = 2;

// Parses the numeric suffix of a daemon-assigned id ("route-7" -> 7) so
// restoring a config can keep its saved ids and still hand out fresh ones
// afterwards. Returns 0 for anything that doesn't fit the pattern.
int idSuffix(const std::string& id, std::string_view prefix) {
    if (id.size() <= prefix.size() || !std::string_view(id).starts_with(prefix)) {
        return 0;
    }
    int value = 0;
    for (char c : std::string_view(id).substr(prefix.size())) {
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

const DeviceInfo* findDevice(const std::vector<DeviceInfo>& devices, const std::string& nodeName) {
    auto it = std::find_if(devices.begin(), devices.end(),
                            [&](const DeviceInfo& d) { return d.nodeName == nodeName; });
    return it == devices.end() ? nullptr : &*it;
}

bool deviceOffersPair(const DeviceInfo& device, const std::string& leftChannel,
                       const std::string& rightChannel) {
    if (leftChannel.empty() || rightChannel.empty()) {
        return true; // "device default" is always available
    }
    return std::any_of(device.pairs.begin(), device.pairs.end(), [&](const ChannelPair& pair) {
        return pair.leftName == leftChannel && pair.rightName == rightChannel;
    });
}

std::string pairDescription(const std::string& leftChannel, const std::string& rightChannel) {
    if (leftChannel.empty() || rightChannel.empty()) {
        return "the device default channels";
    }
    return "channels " + leftChannel + "/" + rightChannel;
}

} // namespace

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::start() {
    loop_ = pw_thread_loop_new("pipeeq-loop", nullptr);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);

    pw_thread_loop_start(loop_);

    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect(context_, nullptr, 0);
    if (core_) {
        pw_core_add_listener(core_, &coreListener_, &kCoreEvents, this);
        registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(registry_, &registryListener_, &kRegistryEvents, this);
    }
    pw_thread_loop_unlock(loop_);

    if (!core_) {
        // Everything past this point dereferences the core, so report it
        // here rather than crashing on the first stream we try to create.
        std::fprintf(stderr, "pipeeq: cannot connect to PipeWire; is it running?\n");
        return false;
    }

    // Registry globals arrive asynchronously on the loop thread. Without
    // waiting for them, restoring a saved config immediately after start()
    // would look up devices against an empty list and conclude that none of
    // the user's hardware exists.
    waitForRegistrySync();
    // A second roundtrip: handling those globals issued a bind per sink, and
    // it's the resulting node info that carries each device's channel layout.
    // Without this, a route restored for a specific stereo pair would be
    // judged against a device whose pairs aren't known yet.
    waitForRegistrySync();
    return true;
}

void AudioEngine::waitForRegistrySync() {
    pw_thread_loop_lock(loop_);
    syncDone_ = false;
    syncSeq_ = pw_core_sync(core_, PW_ID_CORE, 0);
    while (!syncDone_) {
        if (pw_thread_loop_timed_wait(loop_, kRegistrySyncTimeoutSec) != 0) {
            std::fprintf(stderr,
                         "pipeeq: timed out waiting for PipeWire to enumerate devices; routes for devices "
                         "that haven't been reported yet will connect once they are\n");
            break;
        }
    }
    pw_thread_loop_unlock(loop_);
}

void AudioEngine::onCoreDone(void* userdata, uint32_t id, int seq) {
    auto* self = static_cast<AudioEngine*>(userdata);
    if (id == PW_ID_CORE && seq == self->syncSeq_) {
        self->syncDone_ = true;
        pw_thread_loop_signal(self->loop_, false);
    }
}

void AudioEngine::stop() {
    if (!loop_) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        pw_thread_loop_lock(loop_);
        routes_.clear();
        inputs_.clear();
        // Bound node proxies belong to the core, so they have to go first.
        for (auto& bound : boundNodes_) {
            spa_hook_remove(&bound->listener);
            pw_proxy_destroy(bound->proxy);
        }
        boundNodes_.clear();
        if (core_) {
            pw_core_disconnect(core_);
            core_ = nullptr;
        }
        pw_thread_loop_unlock(loop_);
    }

    pw_thread_loop_stop(loop_);

    if (context_) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
}

void AudioEngine::applyConfig(const eqcore::AppConfig& config) {
    const std::vector<DeviceInfo> devices = listDevices();

    std::lock_guard<std::mutex> lock(controlMutex_);

    for (const auto& inputConfig : config.inputs) {
        if (inputConfig.id.empty() || findInputLocked(inputConfig.id)) {
            std::fprintf(stderr, "pipeeq: skipping saved input with a missing or duplicate id '%s'\n",
                         inputConfig.id.c_str());
            continue;
        }
        addInputLocked(inputConfig.id, inputConfig.displayName);
    }

    for (const auto& routeConfig : config.routes) {
        if (routeConfig.id.empty() || findEntryLocked(routeConfig.id)) {
            std::fprintf(stderr, "pipeeq: skipping saved route with a missing or duplicate id '%s'\n",
                         routeConfig.id.c_str());
            continue;
        }

        RouteEntry entry;
        entry.desired = routeConfig;
        nextRouteIndex_ = std::max(nextRouteIndex_, idSuffix(routeConfig.id, "route-") + 1);

        // Clamp/prune what the rest of the daemon can't represent, so a
        // hand-edited or future-version config can't wedge anything.
        if (entry.desired.bands.size() > OutputRoute::kMaxBands) {
            std::fprintf(stderr, "pipeeq: route '%s' has %zu bands; keeping the first %zu\n",
                         entry.desired.id.c_str(), entry.desired.bands.size(), OutputRoute::kMaxBands);
            entry.desired.bands.resize(OutputRoute::kMaxBands);
        }
        for (auto it = entry.desired.inputGainsDb.begin(); it != entry.desired.inputGainsDb.end();) {
            it = findInputLocked(it->first) ? std::next(it) : entry.desired.inputGainsDb.erase(it);
        }

        routes_.push_back(std::move(entry));
        RouteEntry& added = routes_.back();

        if (const DeviceInfo* device = connectableDevice(added, devices)) {
            connectLocked(added, device->id);
        } else {
            std::fprintf(stderr,
                         "pipeeq: device '%s' (%s) for output '%s' isn't available yet; the output is "
                         "configured and %s\n",
                         added.desired.deviceName.c_str(),
                         pairDescription(added.desired.leftChannel, added.desired.rightChannel).c_str(),
                         added.desired.displayName.c_str(),
                         added.desired.autoConnect ? "will connect when the device appears"
                                                   : "has auto-connect disabled");
        }
    }
}

eqcore::AppConfig AudioEngine::snapshotConfig() const {
    std::lock_guard<std::mutex> lock(controlMutex_);

    eqcore::AppConfig config;
    config.inputs.reserve(inputs_.size());
    for (const auto& in : inputs_) {
        config.inputs.push_back(eqcore::InputConfig{in->id(), in->displayName()});
    }
    config.routes.reserve(routes_.size());
    for (const auto& entry : routes_) {
        config.routes.push_back(entry.desired);
    }
    return config;
}

void AudioEngine::reconcile() {
    if (!devicesDirty_.exchange(false)) {
        return;
    }

    const std::vector<DeviceInfo> devices = listDevices();

    std::lock_guard<std::mutex> lock(controlMutex_);
    for (auto& entry : routes_) {
        reconcileEntryLocked(entry, devices);
    }
}

const DeviceInfo* AudioEngine::connectableDevice(const RouteEntry& entry,
                                                  const std::vector<DeviceInfo>& devices) {
    const DeviceInfo* device = findDevice(devices, entry.desired.deviceName);
    if (!device) {
        return nullptr;
    }
    if (!deviceOffersPair(*device, entry.desired.leftChannel, entry.desired.rightChannel)) {
        return nullptr;
    }
    return device;
}

void AudioEngine::reconcileEntryLocked(RouteEntry& entry, const std::vector<DeviceInfo>& devices) {
    const DeviceInfo* device = connectableDevice(entry, devices);

    // A device that reappeared with a different node id (a replug, or a
    // profile switch) leaves the existing stream bound to a node that no
    // longer exists, so treat that as a disconnect too.
    if (entry.live && (!device || device->id != entry.targetNodeId)) {
        std::fprintf(stderr, "pipeeq: device '%s' (%s) for output '%s' is no longer available; disconnecting\n",
                     entry.desired.deviceName.c_str(),
                     pairDescription(entry.desired.leftChannel, entry.desired.rightChannel).c_str(),
                     entry.desired.displayName.c_str());
        disconnectLocked(entry);
    }

    if (!entry.live && device && entry.desired.autoConnect) {
        std::fprintf(stderr, "pipeeq: device '%s' (%s) is available; connecting output '%s'\n",
                     entry.desired.deviceName.c_str(),
                     pairDescription(entry.desired.leftChannel, entry.desired.rightChannel).c_str(),
                     entry.desired.displayName.c_str());
        connectLocked(entry, device->id);
    }
}

void AudioEngine::connectLocked(RouteEntry& entry, uint32_t targetNodeId) {
    pw_thread_loop_lock(loop_);
    auto route = std::make_unique<OutputRoute>(
        core_, loop_, entry.desired.id, entry.desired.deviceName, entry.desired.displayName, targetNodeId,
        kNumChannels, kSampleRateHz, channelPositionFromName(entry.desired.leftChannel),
        channelPositionFromName(entry.desired.rightChannel));
    pw_thread_loop_unlock(loop_);

    // Push the desired configuration into the fresh stream. These are plain
    // snapshot swaps, not PipeWire calls, so they need no loop lock.
    route->setGainDb(entry.desired.gainDb);
    route->setMuted(entry.desired.muted);
    route->setBandCount(entry.desired.bands.size());
    for (std::size_t i = 0; i < entry.desired.bands.size(); ++i) {
        route->setBand(i, entry.desired.bands[i]);
    }
    for (const auto& [inputId, gainDb] : entry.desired.inputGainsDb) {
        InputSource* input = findInputLocked(inputId);
        if (!input) {
            continue;
        }
        if (!route->setInputGainDb(inputId, input->ringBuffer(), gainDb)) {
            std::fprintf(stderr,
                         "pipeeq: output '%s' can mix at most %zu inputs; input '%s' is configured but "
                         "won't be heard\n",
                         entry.desired.displayName.c_str(), OutputRoute::kMaxInputs, inputId.c_str());
        }
    }

    entry.live = std::move(route);
    entry.targetNodeId = targetNodeId;
}

void AudioEngine::disconnectLocked(RouteEntry& entry) {
    if (!entry.live) {
        return;
    }
    pw_thread_loop_lock(loop_);
    entry.live.reset();
    pw_thread_loop_unlock(loop_);
    entry.targetNodeId = SPA_ID_INVALID;
}

std::vector<DeviceInfo> AudioEngine::listDevices() const {
    std::lock_guard<std::mutex> lock(devicesMutex_);
    return devices_;
}

std::string AudioEngine::addInput(const std::string& displayName) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return addInputLocked("input-" + std::to_string(nextInputIndex_), displayName);
}

std::string AudioEngine::addInputLocked(std::string inputId, const std::string& displayName) {
    nextInputIndex_ = std::max(nextInputIndex_, idSuffix(inputId, "input-") + 1);

    pw_thread_loop_lock(loop_);
    inputs_.push_back(
        std::make_unique<InputSource>(core_, inputId, displayName, kNumChannels, kSampleRateHz));
    pw_thread_loop_unlock(loop_);

    return inputId;
}

void AudioEngine::removeInput(const std::string& inputId) {
    std::lock_guard<std::mutex> lock(controlMutex_);

    // Drop this input from every route, live and pending alike, so nothing
    // keeps a stale reference and the persisted config stops mentioning it.
    for (auto& entry : routes_) {
        entry.desired.inputGainsDb.erase(inputId);
        if (entry.live) {
            entry.live->removeInputSlot(inputId);
        }
    }

    pw_thread_loop_lock(loop_);
    inputs_.erase(std::remove_if(inputs_.begin(), inputs_.end(),
                                  [&](const std::unique_ptr<InputSource>& in) { return in->id() == inputId; }),
                  inputs_.end());
    pw_thread_loop_unlock(loop_);
}

std::vector<InputInfo> AudioEngine::listInputs() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    std::vector<InputInfo> result;
    result.reserve(inputs_.size());
    for (const auto& in : inputs_) {
        result.push_back(InputInfo{in->id(), in->displayName()});
    }
    return result;
}

InputSource* AudioEngine::findInputLocked(const std::string& inputId) const {
    auto it = std::find_if(inputs_.begin(), inputs_.end(),
                            [&](const std::unique_ptr<InputSource>& in) { return in->id() == inputId; });
    return it == inputs_.end() ? nullptr : it->get();
}

std::string AudioEngine::addRoute(const std::string& deviceName, const std::string& displayName,
                                   const std::string& leftChannel, const std::string& rightChannel,
                                   double gainDb) {
    const std::vector<DeviceInfo> devices = listDevices();

    std::lock_guard<std::mutex> lock(controlMutex_);

    RouteEntry entry;
    entry.desired.id = "route-" + std::to_string(nextRouteIndex_++);
    entry.desired.deviceName = deviceName;
    entry.desired.displayName = displayName.empty() ? deviceName : displayName;
    entry.desired.leftChannel = leftChannel;
    entry.desired.rightChannel = rightChannel;
    entry.desired.gainDb = gainDb;

    // New outputs hear everything by default (matches pre-mixer behavior).
    for (const auto& in : inputs_) {
        entry.desired.inputGainsDb[in->id()] = 0.0;
    }

    routes_.push_back(std::move(entry));
    RouteEntry& added = routes_.back();

    // A device that isn't here yet is not an error: the route is configured
    // now and connects when the hardware appears.
    if (const DeviceInfo* device = connectableDevice(added, devices)) {
        connectLocked(added, device->id);
    }

    return added.desired.id;
}

void AudioEngine::removeRoute(const std::string& routeId) {
    std::lock_guard<std::mutex> lock(controlMutex_);

    auto it = std::find_if(routes_.begin(), routes_.end(),
                            [&](const RouteEntry& e) { return e.desired.id == routeId; });
    if (it == routes_.end()) {
        return;
    }
    disconnectLocked(*it);
    routes_.erase(it);
}

std::vector<RouteInfo> AudioEngine::listRoutes() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    std::vector<RouteInfo> result;
    result.reserve(routes_.size());
    for (const auto& entry : routes_) {
        result.push_back(RouteInfo{entry.desired.id, entry.desired.deviceName, entry.desired.displayName,
                                    entry.desired.gainDb, entry.desired.muted, entry.desired.bands.size(),
                                    entry.live != nullptr, entry.desired.autoConnect,
                                    entry.desired.leftChannel, entry.desired.rightChannel});
    }
    return result;
}

AudioEngine::RouteEntry* AudioEngine::findEntryLocked(const std::string& routeId) {
    auto it = std::find_if(routes_.begin(), routes_.end(),
                            [&](const RouteEntry& e) { return e.desired.id == routeId; });
    return it == routes_.end() ? nullptr : &*it;
}

const AudioEngine::RouteEntry* AudioEngine::findEntryLocked(const std::string& routeId) const {
    auto it = std::find_if(routes_.begin(), routes_.end(),
                            [&](const RouteEntry& e) { return e.desired.id == routeId; });
    return it == routes_.end() ? nullptr : &*it;
}

bool AudioEngine::setRouteGain(const std::string& routeId, double gainDb) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(routeId);
    if (!entry) {
        return false;
    }
    entry->desired.gainDb = gainDb;
    if (entry->live) {
        entry->live->setGainDb(gainDb);
    }
    return true;
}

bool AudioEngine::setRouteMuted(const std::string& routeId, bool muted) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(routeId);
    if (!entry) {
        return false;
    }
    entry->desired.muted = muted;
    if (entry->live) {
        entry->live->setMuted(muted);
    }
    return true;
}

bool AudioEngine::setRouteBandCount(const std::string& routeId, std::size_t count) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(routeId);
    if (!entry) {
        return false;
    }
    entry->desired.bands.resize(std::min(count, OutputRoute::kMaxBands));
    if (entry->live) {
        entry->live->setBandCount(entry->desired.bands.size());
    }
    return true;
}

bool AudioEngine::setRouteBand(const std::string& routeId, std::size_t index, const eqcore::EqBand& band) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(routeId);
    if (!entry || index >= entry->desired.bands.size()) {
        return false;
    }
    entry->desired.bands[index] = band;
    if (entry->live) {
        entry->live->setBand(index, band);
    }
    return true;
}

std::vector<eqcore::EqBand> AudioEngine::getRouteBands(const std::string& routeId) const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    const RouteEntry* entry = findEntryLocked(routeId);
    return entry ? entry->desired.bands : std::vector<eqcore::EqBand>{};
}

bool AudioEngine::setRouteAutoConnect(const std::string& routeId, bool autoConnect) {
    const std::vector<DeviceInfo> devices = listDevices();

    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(routeId);
    if (!entry) {
        return false;
    }
    entry->desired.autoConnect = autoConnect;

    // Turning it on takes effect immediately if the device is already there;
    // turning it off deliberately leaves a live connection running.
    if (autoConnect && !entry->live) {
        if (const DeviceInfo* device = connectableDevice(*entry, devices)) {
            connectLocked(*entry, device->id);
        }
    }
    return true;
}

bool AudioEngine::setRouteChannels(const std::string& routeId, const std::string& leftChannel,
                                    const std::string& rightChannel) {
    const std::vector<DeviceInfo> devices = listDevices();

    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(routeId);
    if (!entry) {
        return false;
    }
    if (entry->desired.leftChannel == leftChannel && entry->desired.rightChannel == rightChannel) {
        return true;
    }

    entry->desired.leftChannel = leftChannel;
    entry->desired.rightChannel = rightChannel;

    // The pair is part of the stream's negotiated format, so it can only
    // change by reconnecting. Everything else about the route lives in
    // `desired` and is reapplied by connectLocked().
    const bool wasLive = entry->live != nullptr;
    disconnectLocked(*entry);
    if (wasLive || entry->desired.autoConnect) {
        if (const DeviceInfo* device = connectableDevice(*entry, devices)) {
            connectLocked(*entry, device->id);
        }
    }
    return true;
}

bool AudioEngine::setRouteInputGain(const std::string& routeId, const std::string& inputId, double gainDb) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(routeId);
    InputSource* input = findInputLocked(inputId);
    if (!entry || !input) {
        return false;
    }

    // Don't record a level the route can't actually honor, so the persisted
    // config never promises a mix that won't come back after a restart.
    if (entry->live && !entry->live->setInputGainDb(inputId, input->ringBuffer(), gainDb)) {
        return false;
    }
    if (!entry->live && !entry->desired.inputGainsDb.count(inputId) &&
        entry->desired.inputGainsDb.size() >= OutputRoute::kMaxInputs) {
        return false;
    }

    entry->desired.inputGainsDb[inputId] = gainDb;
    return true;
}

bool AudioEngine::removeRouteInput(const std::string& routeId, const std::string& inputId) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(routeId);
    if (!entry) {
        return false;
    }
    entry->desired.inputGainsDb.erase(inputId);
    if (entry->live) {
        entry->live->removeInputSlot(inputId);
    }
    return true;
}

std::vector<std::pair<std::string, double>> AudioEngine::getRouteInputGains(const std::string& routeId) const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    const RouteEntry* entry = findEntryLocked(routeId);
    if (!entry) {
        return {};
    }
    return std::vector<std::pair<std::string, double>>(entry->desired.inputGainsDb.begin(),
                                                        entry->desired.inputGainsDb.end());
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

    // Skip nodes this daemon itself created (its own input virtual sinks
    // and output routes), so they never show up as candidate route targets.
    const std::string_view nodeNameView(nodeName);
    if (nodeNameView.starts_with("pipeeq_input_") || nodeNameView.starts_with("pipeeq_route_")) {
        return;
    }

    const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

    // The registry's properties for a node are only a summary - node.name,
    // description, media.class and friends - and notably do NOT include
    // audio.position. The channel layout lives in the node's own info, so the
    // device starts out with no known layout (one "device default" pair) and
    // is filled in by onNodeInfo() once the bind below reports back.
    DeviceInfo device{id, nodeName, description ? description : nodeName, {}, {}};
    device.pairs = stereoPairsFor(device.positions);

    {
        std::lock_guard<std::mutex> lock(self->devicesMutex_);
        self->devices_.push_back(std::move(device));
    }

    self->bindNodeOnLoop(id);
    // Deliberately no route work here: this runs on the PipeWire loop thread
    // and taking controlMutex_ would invert the lock order the control
    // threads use. reconcile() picks this up from the main thread instead.
    self->devicesDirty_.store(true, std::memory_order_release);
}

void AudioEngine::bindNodeOnLoop(uint32_t id) {
    auto bound = std::make_unique<BoundNode>();
    bound->engine = this;
    bound->id = id;
    bound->proxy = static_cast<pw_proxy*>(
        pw_registry_bind(registry_, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (!bound->proxy) {
        return;
    }

    pw_node_add_listener(reinterpret_cast<pw_node*>(bound->proxy), &bound->listener, &kNodeEvents,
                          bound.get());
    boundNodes_.push_back(std::move(bound));
}

void AudioEngine::unbindNodeOnLoop(uint32_t id) {
    auto it = std::find_if(boundNodes_.begin(), boundNodes_.end(),
                            [id](const std::unique_ptr<BoundNode>& b) { return b->id == id; });
    if (it == boundNodes_.end()) {
        return;
    }
    spa_hook_remove(&(*it)->listener);
    pw_proxy_destroy((*it)->proxy);
    boundNodes_.erase(it);
}

void AudioEngine::onNodeInfo(void* userdata, const pw_node_info* info) {
    auto* bound = static_cast<BoundNode*>(userdata);
    // A node emits info repeatedly - on state and param changes as well as
    // the initial description - and only includes props when the PROPS bit is
    // set. Acting on the others would read a missing audio.position as "this
    // device has no channel layout" and wipe the pairs we already know about,
    // which is exactly what happened before this check existed.
    if (!info || !(info->change_mask & PW_NODE_CHANGE_MASK_PROPS) || !info->props) {
        return;
    }

    // e.g. "[ FL, FR, RL, RR ]" for a 4.0 interface. This also fires again
    // when a device's profile changes, so switching a card to a different
    // layout updates the offered pairs - and reconcile() then disconnects any
    // output whose pair no longer exists rather than leaving it playing into
    // channels the device stopped having.
    const std::vector<std::string> positions =
        parseChannelPositions(spa_dict_lookup(info->props, SPA_KEY_AUDIO_POSITION));

    AudioEngine* self = bound->engine;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(self->devicesMutex_);
        for (auto& device : self->devices_) {
            if (device.id != bound->id || device.positions == positions) {
                continue;
            }
            device.positions = positions;
            device.pairs = stereoPairsFor(positions);
            changed = true;
        }
    }
    if (changed) {
        self->devicesDirty_.store(true, std::memory_order_release);
    }
}

void AudioEngine::onRegistryGlobalRemove(void* userdata, uint32_t id) {
    auto* self = static_cast<AudioEngine*>(userdata);
    self->unbindNodeOnLoop(id);
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(self->devicesMutex_);
        const std::size_t before = self->devices_.size();
        self->devices_.erase(std::remove_if(self->devices_.begin(), self->devices_.end(),
                                             [id](const DeviceInfo& d) { return d.id == id; }),
                             self->devices_.end());
        removed = self->devices_.size() != before;
    }
    if (removed) {
        self->devicesDirty_.store(true, std::memory_order_release);
    }
}

} // namespace pipeeq
