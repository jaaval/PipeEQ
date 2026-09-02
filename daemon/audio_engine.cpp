#include "audio_engine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string_view>

#include <spa/param/audio/raw.h>
#include <spa/utils/dict.h>

#include "adopt_layout.h"
#include "channel_layout.h"
#include "mix_plan.h"

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


} // namespace

std::vector<std::string> DeviceInfo::streamPositions() const {
    if (!positions.empty()) {
        return positions;
    }
    // The device advertises no layout. Substitute the conventional one for its
    // channel count, which for anything unusual is AUX0..AUXn - honest about
    // "we don't know what these are" rather than guessing a layout the
    // hardware may not have. Two channels is the safe fallback when even the
    // channel count is missing.
    return layout::defaultPositionsFor(channelCount > 0 ? static_cast<int>(channelCount) : 2);
}

namespace {

const DeviceInfo* findDevice(const std::vector<DeviceInfo>& devices, const std::string& nodeName) {
    auto it = std::find_if(devices.begin(), devices.end(),
                            [&](const DeviceInfo& d) { return d.nodeName == nodeName; });
    return it == devices.end() ? nullptr : &*it;
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
    // Without this, a restored output would open its stream before its device's
    // real channel layout is known.
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
        addInputLocked(inputConfig.id, inputConfig.displayName, inputConfig.positions);
    }

    for (const auto& outputConfig : config.outputs) {
        if (outputConfig.id.empty() || findEntryLocked(outputConfig.id)) {
            std::fprintf(stderr, "pipeeq: skipping saved output with a missing or duplicate id '%s'\n",
                         outputConfig.id.c_str());
            continue;
        }

        RouteEntry entry;
        entry.desired = outputConfig;
        nextRouteIndex_ = std::max(nextRouteIndex_, idSuffix(outputConfig.id, "output-") + 1);

        // Band clamping, dangling-EQ-reference clearing and pruning sends for
        // inputs that don't exist all happen in eqcore::sanitize() during the
        // load, so a hand-edited or migrated config arrives already
        // representable and this loop doesn't need to repair anything. The
        // channel list is reshaped to the device's real layout on connect.

        routes_.push_back(std::move(entry));
        RouteEntry& added = routes_.back();

        if (const DeviceInfo* device = connectableDevice(added, devices)) {
            connectLocked(added, *device);
        } else {
            std::fprintf(stderr,
                         "pipeeq: device '%s' for output '%s' isn't available yet; the output is "
                         "configured and %s\n",
                         added.desired.deviceName.c_str(), added.desired.displayName.c_str(),
                         added.desired.autoConnect ? "will connect when the device appears"
                                                   : "has auto-connect disabled");
        }
    }
}

eqcore::AppConfig AudioEngine::snapshotConfig() const {
    std::lock_guard<std::mutex> lock(controlMutex_);

    eqcore::AppConfig config;
    config.version = eqcore::kConfigVersion;
    config.inputs.reserve(inputs_.size());
    for (const auto& in : inputs_) {
        eqcore::InputConfig input;
        input.id = in->id();
        input.displayName = in->displayName();
        input.positions = in->positions();
        config.inputs.push_back(std::move(input));
    }
    config.outputs.reserve(routes_.size());
    for (const auto& entry : routes_) {
        config.outputs.push_back(entry.desired);
    }
    return config;
}

void AudioEngine::reconcile() {
    const bool devicesChanged = devicesDirty_.exchange(false);

    const std::vector<DeviceInfo> devices = listDevices();

    std::lock_guard<std::mutex> lock(controlMutex_);
    if (devicesChanged) {
        for (auto& entry : routes_) {
            reconcileEntryLocked(entry, devices);
        }
        return;
    }

    // Even with no device news, a live stream may have just finished
    // negotiating its format - that flag is set from PipeWire's loop thread and
    // is unrelated to registry activity, so it has to be picked up every tick.
    for (auto& entry : routes_) {
        if (entry.live && entry.live->consumeFormatChanged()) {
            republishLocked(entry);
        }
    }
}

const DeviceInfo* AudioEngine::connectableDevice(const RouteEntry& entry,
                                                  const std::vector<DeviceInfo>& devices) {
    // A device is connectable as soon as it exists. There is no longer any
    // "does it still offer this output's stereo pair?" question to ask: an
    // output drives whatever channels the device has, and adoptDeviceLayout()
    // reshapes its configuration to match on connect.
    return findDevice(devices, entry.desired.deviceName);
}

void AudioEngine::reconcileEntryLocked(RouteEntry& entry, const std::vector<DeviceInfo>& devices) {
    const DeviceInfo* device = connectableDevice(entry, devices);

    // A device whose LAYOUT changed under us (a profile switch) needs the
    // stream renegotiated: the channel count and positions are part of the
    // negotiated format, and pw_stream can't change them in place. Detected by
    // comparing against the layout the live stream was created with, so a mere
    // property refresh reporting the same layout does NOT tear anything down.
    if (entry.live && device &&
        layout::needsRenegotiation(entry.livePositions, device->streamPositions())) {
        std::fprintf(stderr,
                     "pipeeq: device '%s' changed its channel layout; reconnecting output '%s'\n",
                     entry.desired.deviceName.c_str(), entry.desired.displayName.c_str());
        disconnectLocked(entry);
    }

    // A device that reappeared with a different node id (a replug, or a
    // profile switch) leaves the existing stream bound to a node that no
    // longer exists, so treat that as a disconnect too.
    if (entry.live && (!device || device->id != entry.targetNodeId)) {
        std::fprintf(stderr,
                     "pipeeq: device '%s' for output '%s' is no longer available; disconnecting\n",
                     entry.desired.deviceName.c_str(), entry.desired.displayName.c_str());
        disconnectLocked(entry);
    }

    if (!entry.live && device && entry.desired.autoConnect) {
        std::fprintf(stderr, "pipeeq: device '%s' is available; connecting output '%s'\n",
                     entry.desired.deviceName.c_str(), entry.desired.displayName.c_str());
        connectLocked(entry, *device);
    }

    // Format negotiation completed (or changed) since the last pass: the mix
    // plan and every EQ coefficient must be rebuilt for the layout and sample
    // rate PipeWire actually chose, not the ones we asked for.
    if (entry.live && entry.live->consumeFormatChanged()) {
        republishLocked(entry);
    }
}

void AudioEngine::connectLocked(RouteEntry& entry, const DeviceInfo& device) {
    const std::vector<std::string> streamPositions = device.streamPositions();

    // Reshape the configuration to the layout the device actually has, matching
    // by position name first so a profile flip restores settings rather than
    // discarding them. New channels arrive with no sends, so previously-unused
    // outputs come up silent instead of suddenly playing the mix.
    const AdoptResult adopted = adoptDeviceLayout(entry.desired, streamPositions);
    entry.liveChannelCount = adopted.liveChannelCount;
    entry.livePositions = streamPositions;

    if (adopted.appendedChannels > 0) {
        std::fprintf(stderr,
                     "pipeeq: output '%s' gained %zu new channel(s) from its device's layout; they "
                     "start silent (no sends) so nothing unexpected plays out of them\n",
                     entry.desired.displayName.c_str(), adopted.appendedChannels);
    }
    if (adopted.retiredChannels > 0) {
        std::fprintf(stderr,
                     "pipeeq: output '%s' has %zu channel(s) its device no longer offers; their "
                     "settings are kept in case the profile changes back\n",
                     entry.desired.displayName.c_str(), adopted.retiredChannels);
    }

    pw_thread_loop_lock(loop_);
    auto stream = std::make_unique<OutputStream>(core_, entry.desired.id, entry.desired.deviceName,
                                                  entry.desired.displayName, device.id, streamPositions,
                                                  kSampleRateHz);
    pw_thread_loop_unlock(loop_);

    entry.live = std::move(stream);
    entry.targetNodeId = device.id;

    // Publishing is a plain snapshot swap, not a PipeWire call, so it needs no
    // loop lock. The stream emits silence until its format is negotiated, at
    // which point reconcile() republishes for the real layout and rate.
    republishLocked(entry);
}

void AudioEngine::republishLocked(RouteEntry& entry) {
    if (!entry.live) {
        return;
    }

    // Negotiation may not have completed yet, in which case fall back to the
    // layout we asked for: the stream is silent until it does, and reconcile()
    // republishes as soon as param_changed reports the real one.
    std::vector<std::string> positions = entry.live->negotiatedPositions();
    if (positions.empty()) {
        positions = entry.live->requestedPositions();
    }

    const eqcore::OutputConfig& desired = entry.desired;

    // Routing uses the USER'S assigned position per channel, falling back to
    // what the device advertises. This is the whole point of the position being
    // control-plane metadata: the stream keeps the device's own layout (so the
    // 1:1 port link holds) while the mix planner matches input channels against
    // whatever the user says each physical channel actually drives. Building
    // the plan from the stream layout instead made SetChannelPosition a no-op
    // that returned true.
    std::vector<std::string> routingPositions = positions;
    for (std::size_t ch = 0; ch < routingPositions.size(); ++ch) {
        if (ch < desired.channels.size() && !desired.channels[ch].position.empty()) {
            routingPositions[ch] = desired.channels[ch].position;
        }
    }
    const uint32_t negotiatedRate = entry.live->negotiatedRateHz();
    const double sampleRateHz =
        negotiatedRate != 0 ? static_cast<double>(negotiatedRate) : static_cast<double>(kSampleRateHz);

    const std::size_t channelCount = std::min(positions.size(), kMaxOutputChannels);

    auto snapshot = std::make_unique<OutputSnapshot>();
    snapshot->numChannels = static_cast<uint8_t>(channelCount);

    // One coefficient block per EQ instance, shared BY POINTER with every
    // channel referencing it - so FL and FR on one instance genuinely share one
    // block, and a gain-only change recomputes no coefficients at all.
    std::map<std::string, std::shared_ptr<const EqCoeffBlock>> coeffBlocks;
    for (const eqcore::EqInstanceConfig& instance : desired.eqInstances) {
        if (instance.bypassed || instance.bands.empty()) {
            continue;
        }
        auto block = std::make_shared<EqCoeffBlock>();
        block->bandCount = std::min(instance.bands.size(), kMaxBands);
        for (std::size_t i = 0; i < block->bandCount; ++i) {
            block->coeffs[i] = instance.bands[i].toCoeffs(sampleRateHz);
        }
        coeffBlocks.emplace(instance.id, std::move(block));
    }

    for (std::size_t ch = 0; ch < channelCount; ++ch) {
        ChannelSnapshot& channelSnapshot = snapshot->channels[ch];
        if (ch >= desired.channels.size()) {
            // A device channel with no configuration yet: silent, so it can
            // never play something the user didn't ask for.
            channelSnapshot.gainLinear = 0.0f;
            continue;
        }
        const eqcore::OutputChannelConfig& channel = desired.channels[ch];
        // Mute folded into the gain here, so the RT path has no mute branch and
        // a mute is just another fader target to slew towards.
        channelSnapshot.gainLinear =
            channel.muted ? 0.0f : static_cast<float>(std::pow(10.0, channel.gainDb / 20.0));
        if (const auto it = coeffBlocks.find(channel.eqInstanceId); it != coeffBlocks.end()) {
            channelSnapshot.eq = it->second;
        }
    }

    // Which inputs does any channel of this output actually send from? An input
    // absent from every channel's sends map gets no slot at all, so the RT
    // thread never reads its ring buffer.
    const std::vector<uint32_t> outputPositionValues = mix::positionValues(routingPositions);
    std::size_t slotIndex = 0;
    for (const auto& input : inputs_) {
        if (slotIndex >= kMaxInputs) {
            std::fprintf(stderr,
                         "pipeeq: output '%s' can mix at most %zu inputs; '%s' is configured but "
                         "won't be heard\n",
                         desired.displayName.c_str(), kMaxInputs, input->id().c_str());
            break;
        }

        std::vector<mix::SendSpec> sends(channelCount);
        bool anySend = false;
        for (std::size_t ch = 0; ch < channelCount; ++ch) {
            if (ch >= desired.channels.size()) {
                sends[ch].enabled = false;
                continue;
            }
            const auto& sendsDb = desired.channels[ch].sendsDb;
            const auto it = sendsDb.find(input->id());
            if (it == sendsDb.end()) {
                // Absent is not a 0 dB send: it means this input isn't routed
                // to this channel at all.
                sends[ch].enabled = false;
                continue;
            }
            sends[ch].gainDb = it->second;
            sends[ch].enabled = true;
            anySend = true;
        }
        if (!anySend) {
            continue;
        }

        InputMixSlot& slot = snapshot->inputs[slotIndex++];
        slot.active = true;
        slot.inputId = input->id();
        slot.identity = input->identity();
        slot.buffer = input->ringBuffer();
        slot.inputChannels = static_cast<uint16_t>(input->numChannels());
        mix::buildOutputTaps(outputPositionValues, mix::positionValues(input->positions()), sends,
                              slot.perChannel, slot.anyTaps);
    }

    entry.live->publish(std::move(snapshot));
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

AudioEngine::RouteEntry* AudioEngine::findEntryLocked(const std::string& outputId) {
    auto it = std::find_if(routes_.begin(), routes_.end(),
                            [&](const RouteEntry& e) { return e.desired.id == outputId; });
    return it == routes_.end() ? nullptr : &*it;
}

const AudioEngine::RouteEntry* AudioEngine::findEntryLocked(const std::string& outputId) const {
    auto it = std::find_if(routes_.begin(), routes_.end(),
                            [&](const RouteEntry& e) { return e.desired.id == outputId; });
    return it == routes_.end() ? nullptr : &*it;
}

InputSource* AudioEngine::findInputLocked(const std::string& inputId) const {
    auto it = std::find_if(inputs_.begin(), inputs_.end(),
                            [&](const std::unique_ptr<InputSource>& in) { return in->id() == inputId; });
    return it == inputs_.end() ? nullptr : it->get();
}

std::string AudioEngine::addInput(const std::string& displayName,
                                  const std::vector<std::string>& positions) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return addInputLocked("input-" + std::to_string(nextInputIndex_), displayName, positions);
}

std::string AudioEngine::addInputLocked(std::string inputId, const std::string& displayName,
                                         const std::vector<std::string>& positions) {
    std::vector<std::string> layoutNames = positions;
    if (layoutNames.empty()) {
        layoutNames = layout::defaultPositionsFor(kDefaultInputChannels);
    }
    if (layoutNames.size() > kMaxInputChannels) {
        std::fprintf(stderr,
                     "pipeeq: input '%s' asked for %zu channels; PipeEQ mixes at most %zu, so the "
                     "layout is truncated\n",
                     displayName.c_str(), layoutNames.size(), kMaxInputChannels);
        layoutNames.resize(kMaxInputChannels);
    }

    nextInputIndex_ = std::max(nextInputIndex_, idSuffix(inputId, "input-") + 1);

    pw_thread_loop_lock(loop_);
    inputs_.push_back(std::make_unique<InputSource>(core_, inputId, displayName, layoutNames,
                                                     kSampleRateHz));
    pw_thread_loop_unlock(loop_);

    return inputId;
}

void AudioEngine::removeInput(const std::string& inputId) {
    std::lock_guard<std::mutex> lock(controlMutex_);

    // Drop this input from every channel of every output, live and pending
    // alike, so nothing keeps a stale reference and the persisted config stops
    // mentioning it.
    for (auto& entry : routes_) {
        for (auto& channel : entry.desired.channels) {
            channel.sendsDb.erase(inputId);
        }
        republishLocked(entry);
    }

    pw_thread_loop_lock(loop_);
    inputs_.erase(std::remove_if(inputs_.begin(), inputs_.end(),
                                  [&](const std::unique_ptr<InputSource>& in) {
                                      return in->id() == inputId;
                                  }),
                   inputs_.end());
    pw_thread_loop_unlock(loop_);
}

bool AudioEngine::setInputDisplayName(const std::string& inputId, const std::string& displayName) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    InputSource* input = findInputLocked(inputId);
    if (!input) {
        return false;
    }
    input->setDisplayName(displayName);
    return true;
}

std::vector<InputInfo> AudioEngine::listInputs() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    std::vector<InputInfo> result;
    result.reserve(inputs_.size());
    for (const auto& in : inputs_) {
        result.push_back(InputInfo{in->id(), in->displayName(), in->positions()});
    }
    return result;
}

// ------------------------------------------------------------------- outputs --

std::string AudioEngine::addOutput(const std::string& deviceName, const std::string& displayName) {
    const std::vector<DeviceInfo> devices = listDevices();

    std::lock_guard<std::mutex> lock(controlMutex_);

    RouteEntry entry;
    entry.desired.id = "output-" + std::to_string(nextRouteIndex_++);
    entry.desired.deviceName = deviceName;
    entry.desired.displayName = displayName.empty() ? deviceName : displayName;

    // Seed the channel list from the device if we can see it, so a brand-new
    // output already has its strips before it connects. Otherwise leave it
    // empty; connectLocked() adopts the real layout when the device shows up.
    if (const DeviceInfo* device = findDevice(devices, deviceName)) {
        adoptDeviceLayout(entry.desired, device->streamPositions());
    }

    // New outputs hear everything at unity, which is the behaviour outputs have
    // always had. (New INPUTS are the opposite - silent on every existing
    // output - so adding an input can't disturb a tuned one.)
    for (auto& channel : entry.desired.channels) {
        for (const auto& in : inputs_) {
            channel.sendsDb[in->id()] = 0.0;
        }
    }

    routes_.push_back(std::move(entry));
    RouteEntry& added = routes_.back();

    // A device that isn't here yet is not an error: the output is configured
    // now and connects when the hardware appears.
    if (const DeviceInfo* device = connectableDevice(added, devices)) {
        connectLocked(added, *device);
    }

    return added.desired.id;
}

void AudioEngine::removeOutput(const std::string& outputId) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    auto it = std::find_if(routes_.begin(), routes_.end(),
                            [&](const RouteEntry& e) { return e.desired.id == outputId; });
    if (it == routes_.end()) {
        return;
    }
    disconnectLocked(*it);
    routes_.erase(it);
}

std::vector<OutputInfo> AudioEngine::listOutputs() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    std::vector<OutputInfo> result;
    result.reserve(routes_.size());
    for (const auto& entry : routes_) {
        OutputInfo info;
        info.id = entry.desired.id;
        info.deviceName = entry.desired.deviceName;
        info.displayName = entry.desired.displayName;
        info.connected = entry.live != nullptr;
        info.autoConnect = entry.desired.autoConnect;
        info.channelCount = entry.desired.channels.size();
        info.liveChannelCount = entry.live ? entry.liveChannelCount : 0;
        info.sampleRateHz = entry.live ? entry.live->negotiatedRateHz() : 0;
        result.push_back(std::move(info));
    }
    return result;
}

bool AudioEngine::setOutputDisplayName(const std::string& outputId, const std::string& displayName) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    entry->desired.displayName = displayName.empty() ? entry->desired.deviceName : displayName;
    return true;
}

bool AudioEngine::setOutputAutoConnect(const std::string& outputId, bool autoConnect) {
    const std::vector<DeviceInfo> devices = listDevices();

    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    entry->desired.autoConnect = autoConnect;

    // Turning it on takes effect immediately if the device is already there;
    // turning it off deliberately leaves a live connection running.
    if (autoConnect && !entry->live) {
        if (const DeviceInfo* device = connectableDevice(*entry, devices)) {
            connectLocked(*entry, *device);
        }
    }
    return true;
}

// ------------------------------------------------------------------ channels --

std::vector<ChannelInfo> AudioEngine::getOutputChannels(const std::string& outputId) const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    const RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return {};
    }

    std::vector<ChannelInfo> result;
    result.reserve(entry->desired.channels.size());
    for (std::size_t i = 0; i < entry->desired.channels.size(); ++i) {
        const eqcore::OutputChannelConfig& channel = entry->desired.channels[i];
        ChannelInfo info;
        info.index = i;
        info.position = channel.position;
        info.displayName = channel.displayName;
        info.gainDb = channel.gainDb;
        info.muted = channel.muted;
        info.eqInstanceId = channel.eqInstanceId;
        if (const eqcore::LinkGroupConfig* group = entry->desired.groupOfChannel(i)) {
            info.groupId = group->id;
        }
        info.driven = entry->live != nullptr && i < entry->liveChannelCount;
        result.push_back(std::move(info));
    }
    return result;
}

// Resolves (outputId, channelIndex) and returns every channel index a set must
// write: the whole link group, or just this one when ungrouped. Empty on an
// unknown output or an out-of-range index.
std::vector<std::size_t> AudioEngine::linkedChannelsLocked(RouteEntry& entry,
                                                            std::size_t channelIndex) const {
    if (channelIndex >= entry.desired.channels.size()) {
        return {};
    }
    return entry.desired.linkedChannels(channelIndex);
}

bool AudioEngine::setChannelGain(const std::string& outputId, std::size_t channelIndex, double gainDb) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    const std::vector<std::size_t> targets = linkedChannelsLocked(*entry, channelIndex);
    if (targets.empty()) {
        return false;
    }
    for (std::size_t index : targets) {
        entry->desired.channels[index].gainDb = gainDb;
    }
    republishLocked(*entry);
    return true;
}

bool AudioEngine::setChannelMuted(const std::string& outputId, std::size_t channelIndex, bool muted) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    const std::vector<std::size_t> targets = linkedChannelsLocked(*entry, channelIndex);
    if (targets.empty()) {
        return false;
    }
    for (std::size_t index : targets) {
        entry->desired.channels[index].muted = muted;
    }
    republishLocked(*entry);
    return true;
}

bool AudioEngine::setChannelPosition(const std::string& outputId, std::size_t channelIndex,
                                      const std::string& position) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry || channelIndex >= entry->desired.channels.size()) {
        return false;
    }
    // NOT linked, and NOT a reconnect: the stream carries the device's own
    // layout regardless, so this only changes which input channels the mix
    // planner matches into this one channel.
    entry->desired.channels[channelIndex].position = position;
    republishLocked(*entry);
    return true;
}

bool AudioEngine::setChannelDisplayName(const std::string& outputId, std::size_t channelIndex,
                                         const std::string& displayName) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry || channelIndex >= entry->desired.channels.size()) {
        return false;
    }
    entry->desired.channels[channelIndex].displayName = displayName;
    return true;
}

bool AudioEngine::setChannelEqInstance(const std::string& outputId, std::size_t channelIndex,
                                        const std::string& eqInstanceId) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry || channelIndex >= entry->desired.channels.size()) {
        return false;
    }
    if (!eqInstanceId.empty() && !entry->desired.findEqInstance(eqInstanceId)) {
        return false;
    }
    // Deliberately NOT linked across the group: giving FL and FR different EQs
    // is exactly what per-channel EQ is for.
    entry->desired.channels[channelIndex].eqInstanceId = eqInstanceId;
    republishLocked(*entry);
    return true;
}

// ------------------------------------------------------------------------ EQ --

std::vector<EqInstanceInfo> AudioEngine::listEqInstances(const std::string& outputId) const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    const RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return {};
    }

    std::vector<EqInstanceInfo> result;
    result.reserve(entry->desired.eqInstances.size());
    for (const eqcore::EqInstanceConfig& instance : entry->desired.eqInstances) {
        EqInstanceInfo info;
        info.id = instance.id;
        info.displayName = instance.displayName;
        info.bandCount = instance.bands.size();
        info.bypassed = instance.bypassed;
        for (const eqcore::OutputChannelConfig& channel : entry->desired.channels) {
            if (channel.eqInstanceId == instance.id) {
                ++info.channelCount;
            }
        }
        result.push_back(std::move(info));
    }
    return result;
}

std::string AudioEngine::addEqInstance(const std::string& outputId, const std::string& displayName) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return {};
    }
    eqcore::EqInstanceConfig instance;
    instance.id = entry->desired.nextEqInstanceId();
    instance.displayName = displayName.empty() ? instance.id : displayName;
    entry->desired.eqInstances.push_back(std::move(instance));
    return entry->desired.eqInstances.back().id;
}

bool AudioEngine::removeEqInstance(const std::string& outputId, const std::string& eqInstanceId) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    auto& instances = entry->desired.eqInstances;
    auto it = std::find_if(instances.begin(), instances.end(),
                            [&](const eqcore::EqInstanceConfig& i) { return i.id == eqInstanceId; });
    if (it == instances.end()) {
        return false;
    }
    instances.erase(it);

    // Any channel that referenced it is left with no EQ rather than a dangling
    // reference - flat, which is the only safe interpretation.
    for (auto& channel : entry->desired.channels) {
        if (channel.eqInstanceId == eqInstanceId) {
            channel.eqInstanceId.clear();
        }
    }
    republishLocked(*entry);
    return true;
}

bool AudioEngine::setEqInstanceName(const std::string& outputId, const std::string& eqInstanceId,
                                     const std::string& displayName) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    eqcore::EqInstanceConfig* instance = entry->desired.findEqInstance(eqInstanceId);
    if (!instance) {
        return false;
    }
    instance->displayName = displayName.empty() ? instance->id : displayName;
    return true;
}

bool AudioEngine::setEqBypassed(const std::string& outputId, const std::string& eqInstanceId,
                                 bool bypassed) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    eqcore::EqInstanceConfig* instance = entry->desired.findEqInstance(eqInstanceId);
    if (!instance) {
        return false;
    }
    instance->bypassed = bypassed;
    republishLocked(*entry);
    return true;
}

bool AudioEngine::setEqBandCount(const std::string& outputId, const std::string& eqInstanceId,
                                  std::size_t count) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    eqcore::EqInstanceConfig* instance = entry->desired.findEqInstance(eqInstanceId);
    if (!instance) {
        return false;
    }
    instance->bands.resize(std::min(count, eqcore::kMaxBands));
    republishLocked(*entry);
    return true;
}

bool AudioEngine::setEqBand(const std::string& outputId, const std::string& eqInstanceId,
                             std::size_t index, const eqcore::EqBand& band) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    eqcore::EqInstanceConfig* instance = entry->desired.findEqInstance(eqInstanceId);
    if (!instance || index >= instance->bands.size()) {
        return false;
    }
    instance->bands[index] = band;
    republishLocked(*entry);
    return true;
}

std::vector<eqcore::EqBand> AudioEngine::getEqBands(const std::string& outputId,
                                                     const std::string& eqInstanceId) const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    const RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return {};
    }
    const eqcore::EqInstanceConfig* instance = entry->desired.findEqInstance(eqInstanceId);
    return instance ? instance->bands : std::vector<eqcore::EqBand>{};
}

std::string AudioEngine::copyEqInstance(const std::string& sourceOutputId,
                                         const std::string& eqInstanceId,
                                         const std::string& targetOutputId) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    const RouteEntry* source = findEntryLocked(sourceOutputId);
    RouteEntry* target = findEntryLocked(targetOutputId);
    if (!source || !target) {
        return {};
    }
    const eqcore::EqInstanceConfig* instance = source->desired.findEqInstance(eqInstanceId);
    if (!instance) {
        return {};
    }

    eqcore::EqInstanceConfig copy = *instance;
    copy.id = target->desired.nextEqInstanceId();
    target->desired.eqInstances.push_back(std::move(copy));
    republishLocked(*target);
    return target->desired.eqInstances.back().id;
}

// The channel-scoped convenience forms. These exist so a caller can edit a
// channel's EQ without knowing that instances exist - the band-setting forms
// create one on demand and assign it.
eqcore::EqInstanceConfig& AudioEngine::channelEqInstanceLocked(RouteEntry& entry,
                                                                std::size_t channelIndex) {
    eqcore::OutputChannelConfig& channel = entry.desired.channels[channelIndex];
    if (eqcore::EqInstanceConfig* existing = entry.desired.findEqInstance(channel.eqInstanceId)) {
        return *existing;
    }

    eqcore::EqInstanceConfig instance;
    instance.id = entry.desired.nextEqInstanceId();
    instance.displayName = channel.position.empty() ? instance.id : channel.position;
    entry.desired.eqInstances.push_back(std::move(instance));
    eqcore::EqInstanceConfig& added = entry.desired.eqInstances.back();

    // Assign it to the whole link group, not just this channel. Linked channels
    // share one curve, so creating an EQ by editing one member of a pair has to
    // give the pair one instance - otherwise the first edit would silently
    // unshare them.
    for (std::size_t index : entry.desired.linkedChannels(channelIndex)) {
        entry.desired.channels[index].eqInstanceId = added.id;
    }
    return added;
}

// Drops instances no channel references any more.
//
// Assignment is not user-controllable, so an unreferenced instance is
// unreachable rather than a saved preset - it is garbage, and left alone it
// accumulates every time a group is formed. Only called from the link
// operations that can orphan one; deliberately NOT done on config load, because
// loading a file should not delete things from it.
void AudioEngine::pruneUnreferencedEqLocked(RouteEntry& entry) {
    auto& instances = entry.desired.eqInstances;
    instances.erase(std::remove_if(instances.begin(), instances.end(),
                                    [&](const eqcore::EqInstanceConfig& instance) {
                                        return std::none_of(
                                            entry.desired.channels.begin(),
                                            entry.desired.channels.end(),
                                            [&](const eqcore::OutputChannelConfig& channel) {
                                                return channel.eqInstanceId == instance.id;
                                            });
                                    }),
                     instances.end());
}

// Linked channels share ONE instance: the lowest-index member's.
//
// Consistent with gain, mute and sends, which the group also adopts from that
// member - so linking is never ambiguous about which side wins. A member's own
// previous curve is discarded, which is why the UI says so before linking.
void AudioEngine::shareGroupEqLocked(RouteEntry& entry, const std::vector<uint32_t>& members) {
    if (members.empty()) {
        return;
    }
    const std::size_t leader = *std::min_element(members.begin(), members.end());
    if (leader >= entry.desired.channels.size()) {
        return;
    }
    const std::string leaderEqId = entry.desired.channels[leader].eqInstanceId;
    for (uint32_t index : members) {
        if (index < entry.desired.channels.size()) {
            entry.desired.channels[index].eqInstanceId = leaderEqId;
        }
    }
    pruneUnreferencedEqLocked(entry);
}

// Gives each named channel its OWN copy of whatever curve it currently shares.
//
// Without this, "unlink" would separate the faders but leave the channels still
// sharing one EQ, so editing one would still change the other - which is not
// what unlinking means.
//
// Every named channel gets a copy, including the lowest-indexed one. Skipping
// one as a notional "leader" was wrong for partial regrouping: if the channel
// being removed from a group happened to be the lowest-indexed member, it was
// the one skipped, so it kept sharing the group's instance and editing it still
// changed the channels it had just been separated from.
void AudioEngine::splitGroupEqLocked(RouteEntry& entry, const std::vector<uint32_t>& members) {
    if (members.empty()) {
        return;
    }
    for (uint32_t index : members) {
        if (index >= entry.desired.channels.size()) {
            continue;
        }
        eqcore::OutputChannelConfig& channel = entry.desired.channels[index];
        const eqcore::EqInstanceConfig* shared = entry.desired.findEqInstance(channel.eqInstanceId);
        if (!shared) {
            continue;
        }
        eqcore::EqInstanceConfig copy = *shared;
        copy.id = entry.desired.nextEqInstanceId();
        if (!channel.position.empty()) {
            copy.displayName = channel.position;
        }
        entry.desired.eqInstances.push_back(std::move(copy));
        channel.eqInstanceId = entry.desired.eqInstances.back().id;
    }
}

std::vector<eqcore::EqBand> AudioEngine::getChannelEqBands(const std::string& outputId,
                                                            std::size_t channelIndex) const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    const RouteEntry* entry = findEntryLocked(outputId);
    if (!entry || channelIndex >= entry->desired.channels.size()) {
        return {};
    }
    const eqcore::EqInstanceConfig* instance =
        entry->desired.findEqInstance(entry->desired.channels[channelIndex].eqInstanceId);
    return instance ? instance->bands : std::vector<eqcore::EqBand>{};
}

bool AudioEngine::setChannelEqBandCount(const std::string& outputId, std::size_t channelIndex,
                                         std::size_t count) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry || channelIndex >= entry->desired.channels.size()) {
        return false;
    }
    eqcore::EqInstanceConfig& instance = channelEqInstanceLocked(*entry, channelIndex);
    instance.bands.resize(std::min(count, eqcore::kMaxBands));
    republishLocked(*entry);
    return true;
}

bool AudioEngine::setChannelEqBand(const std::string& outputId, std::size_t channelIndex,
                                    std::size_t index, const eqcore::EqBand& band) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry || channelIndex >= entry->desired.channels.size()) {
        return false;
    }
    eqcore::EqInstanceConfig& instance = channelEqInstanceLocked(*entry, channelIndex);
    if (index >= instance.bands.size()) {
        return false;
    }
    instance.bands[index] = band;
    republishLocked(*entry);
    return true;
}

// -------------------------------------------------------------- link groups --

std::vector<LinkGroupInfo> AudioEngine::listLinkGroups(const std::string& outputId) const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    const RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return {};
    }
    std::vector<LinkGroupInfo> result;
    result.reserve(entry->desired.linkGroups.size());
    for (const eqcore::LinkGroupConfig& group : entry->desired.linkGroups) {
        result.push_back(LinkGroupInfo{group.id, group.displayName, group.channelIndices});
    }
    return result;
}

// Applies the lowest-index member's gain/mute/sends to every other member, so
// a freshly linked group is never ambiguous about which side won.
void AudioEngine::adoptGroupLeaderLocked(RouteEntry& entry,
                                          const std::vector<uint32_t>& channelIndices) {
    if (channelIndices.empty()) {
        return;
    }
    const std::size_t leader = *std::min_element(channelIndices.begin(), channelIndices.end());
    if (leader >= entry.desired.channels.size()) {
        return;
    }
    const eqcore::OutputChannelConfig source = entry.desired.channels[leader];
    for (uint32_t index : channelIndices) {
        if (index >= entry.desired.channels.size() || index == leader) {
            continue;
        }
        eqcore::OutputChannelConfig& target = entry.desired.channels[index];
        target.gainDb = source.gainDb;
        target.muted = source.muted;
        target.sendsDb = source.sendsDb;
    }
}

std::string AudioEngine::createLinkGroup(const std::string& outputId,
                                          const std::vector<uint32_t>& channelIndices,
                                          const std::string& displayName) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return {};
    }

    std::vector<uint32_t> members = channelIndices;
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    if (members.size() < 2) {
        return {};
    }
    for (uint32_t index : members) {
        if (index >= entry->desired.channels.size()) {
            return {};
        }
        // A channel in two groups has no coherent meaning: a set on it would
        // have to write two different member sets.
        if (entry->desired.groupOfChannel(index)) {
            return {};
        }
    }

    eqcore::LinkGroupConfig group;
    group.id = entry->desired.nextLinkGroupId();
    group.channelIndices = members;
    if (displayName.empty()) {
        for (uint32_t index : members) {
            const std::string& position = entry->desired.channels[index].position;
            group.displayName += group.displayName.empty() ? position : ("/" + position);
        }
    } else {
        group.displayName = displayName;
    }
    entry->desired.linkGroups.push_back(std::move(group));

    adoptGroupLeaderLocked(*entry, members);
    shareGroupEqLocked(*entry, members);
    republishLocked(*entry);
    return entry->desired.linkGroups.back().id;
}

bool AudioEngine::removeLinkGroup(const std::string& outputId, const std::string& groupId) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    auto& groups = entry->desired.linkGroups;
    auto it = std::find_if(groups.begin(), groups.end(),
                            [&](const eqcore::LinkGroupConfig& g) { return g.id == groupId; });
    if (it == groups.end()) {
        return false;
    }
    // Unlinking keeps every channel's current gain, mute and sends; they simply
    // stop moving together from now on. The shared EQ has to be split into
    // per-channel copies, though, or the channels would keep sharing one curve
    // and editing either would still change both.
    std::vector<uint32_t> members = it->channelIndices;
    groups.erase(it);
    // One member can keep the original instance; the rest get copies. Which one
    // keeps it is arbitrary, so use the lowest index for determinism.
    if (!members.empty()) {
        members.erase(std::min_element(members.begin(), members.end()));
    }
    splitGroupEqLocked(*entry, members);
    republishLocked(*entry);
    return true;
}

bool AudioEngine::setLinkGroupChannels(const std::string& outputId, const std::string& groupId,
                                        const std::vector<uint32_t>& channelIndices) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    eqcore::LinkGroupConfig* group = entry->desired.findLinkGroup(groupId);
    if (!group) {
        return false;
    }

    std::vector<uint32_t> members = channelIndices;
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    if (members.size() < 2) {
        return false;
    }
    for (uint32_t index : members) {
        if (index >= entry->desired.channels.size()) {
            return false;
        }
        const eqcore::LinkGroupConfig* owner = entry->desired.groupOfChannel(index);
        if (owner && owner->id != groupId) {
            return false;
        }
    }

    const std::vector<uint32_t> previousMembers = group->channelIndices;
    group->channelIndices = members;

    // Every channel dropped from the group gets its own copy of the curve, so it
    // is genuinely separated. Done before the remaining members re-share, since
    // that reassigns the instance the copies are taken from.
    std::vector<uint32_t> removed;
    for (uint32_t index : previousMembers) {
        if (std::find(members.begin(), members.end(), index) == members.end()) {
            removed.push_back(index);
        }
    }
    splitGroupEqLocked(*entry, removed);
    adoptGroupLeaderLocked(*entry, members);
    shareGroupEqLocked(*entry, members);
    republishLocked(*entry);
    return true;
}

// -------------------------------------------------------------------- sends --

std::vector<SendInfo> AudioEngine::getSends(const std::string& outputId) const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    const RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return {};
    }
    std::vector<SendInfo> result;
    for (std::size_t i = 0; i < entry->desired.channels.size(); ++i) {
        for (const auto& [inputId, gainDb] : entry->desired.channels[i].sendsDb) {
            result.push_back(SendInfo{i, inputId, gainDb});
        }
    }
    return result;
}

bool AudioEngine::setSend(const std::string& outputId, std::size_t channelIndex,
                           const std::string& inputId, double gainDb) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry || !findInputLocked(inputId)) {
        return false;
    }
    const std::vector<std::size_t> targets = linkedChannelsLocked(*entry, channelIndex);
    if (targets.empty()) {
        return false;
    }

    // The send pool is per output, so count the distinct inputs any channel
    // sends from. Refusing here keeps the persisted config from promising a mix
    // that won't come back after a restart.
    if (!routedInputsLocked(*entry).count(inputId) &&
        routedInputsLocked(*entry).size() >= kMaxInputs) {
        return false;
    }

    for (std::size_t index : targets) {
        entry->desired.channels[index].sendsDb[inputId] = gainDb;
    }
    republishLocked(*entry);
    return true;
}

bool AudioEngine::removeSend(const std::string& outputId, std::size_t channelIndex,
                              const std::string& inputId) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry) {
        return false;
    }
    const std::vector<std::size_t> targets = linkedChannelsLocked(*entry, channelIndex);
    if (targets.empty()) {
        return false;
    }
    for (std::size_t index : targets) {
        entry->desired.channels[index].sendsDb.erase(inputId);
    }
    republishLocked(*entry);
    return true;
}

std::set<std::string> AudioEngine::routedInputsLocked(const RouteEntry& entry) const {
    std::set<std::string> ids;
    for (const auto& channel : entry.desired.channels) {
        for (const auto& [inputId, gainDb] : channel.sendsDb) {
            ids.insert(inputId);
        }
    }
    return ids;
}

// ----------------------------------------------------------------- metering --

std::vector<float> AudioEngine::takeOutputPeaks(const std::string& outputId) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    RouteEntry* entry = findEntryLocked(outputId);
    if (!entry || !entry->live) {
        return {};
    }
    const std::size_t count = std::min(entry->liveChannelCount, kMaxOutputChannels);
    std::vector<float> peaks(count, 0.0f);
    for (std::size_t i = 0; i < count; ++i) {
        peaks[i] = entry->live->takeChannelPeak(i);
    }
    return peaks;
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

    {
        std::lock_guard<std::mutex> lock(self->devicesMutex_);
        self->devices_.push_back(std::move(device));
    }

    self->bindNodeOnLoop(id);
    // Deliberately no route work here: this runs on the PipeWire loop thread
    // and taking controlMutex_ would invert the lock order the control
    // threads use. reconcile() picks this up from the main thread instead.
    self->devicesDirty_.store(true, std::memory_order_release);
    self->devicesChangedForClients_.store(true, std::memory_order_release);
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
    // device has no channel layout" and wipe the layout we already know about,
    // which is exactly what happened before this check existed.
    if (!info || !(info->change_mask & PW_NODE_CHANGE_MASK_PROPS) || !info->props) {
        return;
    }

    // e.g. "[ FL, FR, RL, RR ]" for a 4.0 interface. This also fires again when
    // a device's profile changes, so switching a card to a different layout
    // updates what we know - and reconcile() then renegotiates the stream for
    // the new layout rather than leaving it playing into channels the device
    // stopped having.
    const std::vector<std::string> positions =
        layout::parseChannelPositions(spa_dict_lookup(info->props, SPA_KEY_AUDIO_POSITION));

    // The channel COUNT matters independently of the positions: a device can
    // report how many channels it has while advertising no layout for them
    // (a Focusrite in its "Pro Audio" profile, for instance), and that is
    // exactly the case that needs a substituted AUX layout. The previous
    // engine never read this at all - it inferred the count from the position
    // list, so a layout-less device looked like it had no channels.
    uint32_t channelCount = 0;
    if (const char* channelsText = spa_dict_lookup(info->props, SPA_KEY_AUDIO_CHANNELS)) {
        channelCount = static_cast<uint32_t>(std::strtoul(channelsText, nullptr, 10));
    }
    if (channelCount == 0) {
        channelCount = static_cast<uint32_t>(positions.size());
    }

    AudioEngine* self = bound->engine;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(self->devicesMutex_);
        for (auto& device : self->devices_) {
            if (device.id != bound->id ||
                (device.positions == positions && device.channelCount == channelCount)) {
                continue;
            }
            device.positions = positions;
            device.channelCount = channelCount;
            changed = true;
        }
    }
    if (changed) {
        self->devicesDirty_.store(true, std::memory_order_release);
    self->devicesChangedForClients_.store(true, std::memory_order_release);
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
    self->devicesChangedForClients_.store(true, std::memory_order_release);
    }
}

} // namespace pipeeq
