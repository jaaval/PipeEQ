#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>

#include "channel_pair.h"
#include "input_source.h"
#include "output_route.h"
#include "route_config.h"

namespace pipeeq {

struct DeviceInfo {
    uint32_t id;
    std::string nodeName;
    std::string description;
    // The device's channel layout as SPA short names ("FL", "FR", "RL", ...),
    // and the stereo pairs derived from it. A 4.0 interface offers two, each
    // of which can host its own independent output.
    std::vector<std::string> positions;
    std::vector<ChannelPair> pairs;
};

struct InputInfo {
    std::string id;
    std::string displayName;
};

struct RouteInfo {
    std::string id;
    std::string deviceName;
    std::string displayName;
    double gainDb = 0.0;
    bool muted = false;
    std::size_t bandCount = 0;
    // True while a live stream to the target device exists. False means the
    // route is configured but waiting for its device to show up - it can
    // still be edited, and the edits are kept and applied on connect.
    bool connected = false;
    bool autoConnect = true;
    // Which stereo pair of the device this output drives; empty for
    // "device default". See RouteConfig.
    std::string leftChannel;
    std::string rightChannel;
};

// Owns the PipeWire thread loop, the set of InputSources (each a capture
// "virtual sink" apps can play into), and the set of output routes that mix
// any subset of those inputs and fan the result out to physical devices,
// each with its own gain/mute/EqChain.
//
// A route is a *RouteEntry*: an always-present desired configuration plus
// an OutputRoute that exists only while the target device does. That split
// is what makes "connect whenever the hardware shows up" work, and it means
// a missing device is never a reason to forget a route's settings.
//
// Threading model. Three threads touch this object:
//   - the main thread: start()/stop()/applyConfig()/reconcile();
//   - the D-Bus dispatch thread: every setter/getter (sdbus-c++ serializes
//     incoming method calls onto one thread);
//   - PipeWire's loop thread: the registry callbacks only.
// The first two are serialized against each other by controlMutex_, which
// guards inputs_, routes_ and the id counters, and which every public
// method below takes. The registry callbacks deliberately do NOT take it:
// they run with the PipeWire thread-loop lock already held, and the other
// two threads take controlMutex_ *before* pw_thread_loop_lock() when they
// create or destroy streams, so having the callbacks acquire it in the
// opposite order is exactly the deadlock this avoids. Instead they only
// update devices_ (under its own leaf mutex) and set devicesDirty_; the
// main thread picks that up in reconcile() and does the stream work there.
//
// Nothing on PipeWire's realtime thread iterates any of these containers
// (see OutputRoute's class comment) - an OutputRoute only ever reads from
// InputSource ring buffers it was handed a shared_ptr to, so removing an
// input or a route never invalidates something a concurrently-running
// process() callback is using.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Starts the thread loop, connects to PipeWire, and does not return
    // until the initial registry enumeration has completed - so devices
    // are already known by the time applyConfig() runs. Call pw_init()
    // before constructing/starting any engine. Returns false if PipeWire
    // can't be reached at all, in which case nothing else here is usable.
    bool start();
    void stop();

    // Recreates inputs and routes from a saved configuration, preserving
    // the saved ids. Routes whose device isn't available yet are kept as
    // pending and connected later by reconcile().
    void applyConfig(const eqcore::AppConfig& config);

    // The full current configuration, including routes that are configured
    // but not currently connected. This is what gets persisted - it must
    // never be narrowed to just the live routes.
    eqcore::AppConfig snapshotConfig() const;

    // Connects any pending route whose device has appeared, and tears down
    // any live route whose device has gone away (or came back with a new
    // node id, i.e. a replug). Cheap and a no-op unless the registry
    // reported a change; call it periodically from the main thread.
    void reconcile();

    std::vector<DeviceInfo> listDevices() const;

    // Inputs (capture sources apps/other clients play into).
    std::string addInput(const std::string& displayName);
    void removeInput(const std::string& inputId);
    std::vector<InputInfo> listInputs() const;

    // Adds a route driving one stereo pair of the given PipeWire node.name,
    // connecting it immediately if that device is present and leaving it
    // pending otherwise (so hardware can be configured before it's plugged
    // in). leftChannel/rightChannel are SPA channel short names, empty for
    // the device default. Returns the new route id. Seeds every
    // currently-known input at 0dB, so a new output hears everything by
    // default.
    std::string addRoute(const std::string& deviceName, const std::string& displayName,
                          const std::string& leftChannel = {}, const std::string& rightChannel = {},
                          double gainDb = 0.0);
    void removeRoute(const std::string& routeId);

    std::vector<RouteInfo> listRoutes() const;

    // Each returns false if routeId doesn't name a known route. All of
    // these work on a route that isn't currently connected: the change is
    // recorded and applied when it connects.
    bool setRouteGain(const std::string& routeId, double gainDb);
    bool setRouteMuted(const std::string& routeId, bool muted);
    bool setRouteBandCount(const std::string& routeId, std::size_t count);
    bool setRouteBand(const std::string& routeId, std::size_t index, const eqcore::EqBand& band);
    std::vector<eqcore::EqBand> getRouteBands(const std::string& routeId) const;

    // Turning auto-connect on connects the route right away if its device
    // is already available; turning it off leaves an existing connection
    // alone and only stops it being reconnected in the future.
    bool setRouteAutoConnect(const std::string& routeId, bool autoConnect);

    // Re-points an output at a different stereo pair of its device (e.g. from
    // a Scarlett's outputs 1/2 to 3/4), keeping its gain/mute/EQ/mix. The
    // stream carries the pair in its negotiated format, so a live route is
    // reconnected to apply this.
    bool setRouteChannels(const std::string& routeId, const std::string& leftChannel,
                           const std::string& rightChannel);

    // Activates/updates or clears one input's mix level into one route.
    // False if either id is unknown, or if the route's input-slot pool is
    // full (OutputRoute::kMaxInputs).
    bool setRouteInputGain(const std::string& routeId, const std::string& inputId, double gainDb);
    bool removeRouteInput(const std::string& routeId, const std::string& inputId);
    std::vector<std::pair<std::string, double>> getRouteInputGains(const std::string& routeId) const;

    static constexpr int kNumChannels = 2;
    static constexpr uint32_t kSampleRateHz = 48000;

    // Public because they're referenced from file-scope pw_*_events structs
    // in audio_engine.cpp; not part of the intended public API otherwise.
    static void onRegistryGlobal(void* userdata, uint32_t id, uint32_t permissions, const char* type,
                                  uint32_t version, const spa_dict* props);
    static void onRegistryGlobalRemove(void* userdata, uint32_t id);
    static void onCoreDone(void* userdata, uint32_t id, int seq);
    static void onNodeInfo(void* userdata, const pw_node_info* info);

private:
    // A route's desired configuration, plus the live stream when its device
    // is present. `desired` is the single source of truth for everything the
    // control plane reports or persists; `live` is derived from it.
    struct RouteEntry {
        eqcore::RouteConfig desired;
        std::unique_ptr<OutputRoute> live;
        uint32_t targetNodeId = SPA_ID_INVALID; // node id `live` was connected to
    };

    // All of these assume controlMutex_ is held.
    RouteEntry* findEntryLocked(const std::string& routeId);
    const RouteEntry* findEntryLocked(const std::string& routeId) const;
    InputSource* findInputLocked(const std::string& inputId) const;
    std::string addInputLocked(std::string inputId, const std::string& displayName);
    void connectLocked(RouteEntry& entry, uint32_t targetNodeId);
    void disconnectLocked(RouteEntry& entry);
    // Connects entry if it should be and isn't, or disconnects it if its
    // device went away or moved to a new node id.
    void reconcileEntryLocked(RouteEntry& entry, const std::vector<DeviceInfo>& devices);
    // The device an entry can actually connect to right now: null unless the
    // device is present *and* still offers the entry's stereo pair. A profile
    // switch can take a pair away while leaving the device in place, and
    // connecting to a pair the device doesn't have would link to nothing and
    // play silently into the void.
    static const DeviceInfo* connectableDevice(const RouteEntry& entry,
                                                const std::vector<DeviceInfo>& devices);

    // Blocks until PipeWire has replied to a sync, by which point every
    // registry global that already existed has been delivered.
    void waitForRegistrySync();

    // A sink node we've bound in order to watch its info, which is the only
    // place its channel layout is reported. Heap-allocated and never moved:
    // `listener` is an intrusive hook and `this` is the callback's userdata.
    // Only ever touched on PipeWire's loop thread.
    struct BoundNode {
        AudioEngine* engine = nullptr;
        uint32_t id = SPA_ID_INVALID;
        pw_proxy* proxy = nullptr;
        spa_hook listener{};
    };

    // Both must run on the loop thread (the registry callbacks already do).
    void bindNodeOnLoop(uint32_t id);
    void unbindNodeOnLoop(uint32_t id);

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    spa_hook registryListener_{};
    spa_hook coreListener_{};

    // Both only ever touched under the PipeWire thread-loop lock.
    int syncSeq_ = 0;
    bool syncDone_ = false;

    mutable std::mutex devicesMutex_; // leaf: never taken while holding another
    std::vector<DeviceInfo> devices_;
    std::atomic<bool> devicesDirty_{false};

    std::vector<std::unique_ptr<BoundNode>> boundNodes_; // loop thread only

    mutable std::mutex controlMutex_;
    std::vector<std::unique_ptr<InputSource>> inputs_;
    std::vector<RouteEntry> routes_;
    int nextRouteIndex_ = 1;
    int nextInputIndex_ = 1;
};

} // namespace pipeeq
