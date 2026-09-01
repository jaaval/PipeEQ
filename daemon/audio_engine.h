#pragma once

#include <atomic>
#include <cstdint>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>

#include "input_source.h"
#include "output_stream.h"
#include "rt_limits.h"
#include "app_config.h"

namespace pipeeq {

struct DeviceInfo {
    uint32_t id;
    std::string nodeName;
    std::string description;
    // The device's channel layout as SPA short names ("FL", "FR", "RL", ...).
    // Empty when the device advertises none, in which case streamPositions()
    // substitutes a conventional layout for its channel count.
    std::vector<std::string> positions;
    // From the node's audio.channels property. positions may be empty while
    // this is not, which is exactly the case that needs a substitute layout.
    uint32_t channelCount = 0;

    // The layout an output stream should negotiate for this device: what the
    // device says, or the conventional layout for its channel count.
    std::vector<std::string> streamPositions() const;
};

struct InputInfo {
    std::string id;
    std::string displayName;
    // The virtual sink's declared channel layout, SPA short names.
    std::vector<std::string> positions;
};

// One output: a device, and how much of it is currently live.
struct OutputInfo {
    std::string id;
    std::string deviceName;
    std::string displayName;
    // True while a live stream to the target device exists. False means the
    // output is configured but waiting for its device to show up - it can
    // still be edited, and the edits are kept and applied on connect.
    bool connected = false;
    bool autoConnect = true;
    // Total configured channels, which may exceed liveChannelCount: the extra
    // entries are retired configs kept across a profile switch.
    std::size_t channelCount = 0;
    std::size_t liveChannelCount = 0;
    // The rate actually negotiated, or 0 when not connected. The GUI needs
    // this to draw an EQ curve that matches what is really being applied.
    uint32_t sampleRateHz = 0;
};

// One mono hardware output channel: the mixer strip.
struct ChannelInfo {
    std::size_t index = 0;
    std::string position;
    std::string displayName;
    double gainDb = 0.0;
    bool muted = false;
    std::string eqInstanceId;
    std::string groupId; // empty when ungrouped
    // False for a configured channel the device doesn't currently offer (a
    // retired channel after a profile switch). Still fully editable.
    bool driven = false;
};

struct EqInstanceInfo {
    std::string id;
    std::string displayName;
    std::size_t bandCount = 0;
    bool bypassed = false;
    // How many channels of this output reference it, which the UI shows as
    // "shared by N".
    std::size_t channelCount = 0;
};

struct LinkGroupInfo {
    std::string id;
    std::string displayName;
    std::vector<uint32_t> channelIndices;
};

struct SendInfo {
    std::size_t channelIndex = 0;
    std::string inputId;
    double gainDb = 0.0;
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
    // "device default".
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

    // True once since the last call: the set of devices (or a device's layout)
    // changed, so a client's device list is stale. Separate from the flag
    // reconcile() consumes for its own work, because that one is cleared by
    // reconciliation whether or not anyone has been told.
    bool consumeDevicesChanged() {
        return devicesChangedForClients_.exchange(false, std::memory_order_acq_rel);
    }

    std::vector<DeviceInfo> listDevices() const;

    // ------------------------------------------------------------- inputs --

    // Creates a virtual sink with the given channel layout (SPA short names).
    // An empty layout means stereo. The layout is fixed for the sink's life:
    // changing it would mean recreating the stream and its ring buffer, which
    // every live output holds a reference to.
    //
    // A new input starts SILENT on every existing output - no sends at all -
    // so adding one can never disturb an output that was already tuned.
    std::string addInput(const std::string& displayName,
                          const std::vector<std::string>& positions = {});
    void removeInput(const std::string& inputId);
    bool setInputDisplayName(const std::string& inputId, const std::string& displayName);
    std::vector<InputInfo> listInputs() const;

    // ------------------------------------------------------------ outputs --

    // Adds an output for the given PipeWire node.name, connecting it
    // immediately if that device is present and leaving it pending otherwise
    // (so hardware can be configured before it's plugged in).
    //
    // The channel list is adopted from the device on connect. Every channel
    // seeds a 0 dB send from every currently-known input, so a newly added
    // output hears everything by default - the same behaviour outputs have
    // always had.
    std::string addOutput(const std::string& deviceName, const std::string& displayName);
    void removeOutput(const std::string& outputId);
    std::vector<OutputInfo> listOutputs() const;
    bool setOutputDisplayName(const std::string& outputId, const std::string& displayName);

    // Turning auto-connect on connects the output right away if its device is
    // already available; turning it off leaves an existing connection alone and
    // only stops it being reconnected in the future.
    bool setOutputAutoConnect(const std::string& outputId, bool autoConnect);

    // ----------------------------------------------------------- channels --

    std::vector<ChannelInfo> getOutputChannels(const std::string& outputId) const;

    // Gain and mute are LINKED: setting either on a channel that belongs to a
    // link group writes every member of that group. The per-channel fields stay
    // the single source of truth, so there is never a group value that can
    // disagree with its members.
    bool setChannelGain(const std::string& outputId, std::size_t channelIndex, double gainDb);
    bool setChannelMuted(const std::string& outputId, std::size_t channelIndex, bool muted);

    // Changing a channel's logical position is control-plane only: the stream
    // always carries the device's own layout, so this is a snapshot rebuild
    // rather than a reconnect. It changes which input channels are matched into
    // this channel and nothing else.
    bool setChannelPosition(const std::string& outputId, std::size_t channelIndex,
                             const std::string& position);
    bool setChannelDisplayName(const std::string& outputId, std::size_t channelIndex,
                                const std::string& displayName);

    // Points a channel at one of its output's EQ instances, or at none when
    // eqInstanceId is empty.
    //
    // Not exposed in the UI: which channels share a curve is decided by LINKING
    // them, not by a separate assignment step. Kept because the daemon needs it
    // internally and because a client that wants finer control can still use it.
    bool setChannelEqInstance(const std::string& outputId, std::size_t channelIndex,
                               const std::string& eqInstanceId);

    // --------------------------------------------------------------- EQ --

    std::vector<EqInstanceInfo> listEqInstances(const std::string& outputId) const;
    // Returns the new instance id, or empty if outputId is unknown.
    std::string addEqInstance(const std::string& outputId, const std::string& displayName);
    bool removeEqInstance(const std::string& outputId, const std::string& eqInstanceId);
    bool setEqInstanceName(const std::string& outputId, const std::string& eqInstanceId,
                            const std::string& displayName);
    bool setEqBypassed(const std::string& outputId, const std::string& eqInstanceId, bool bypassed);
    bool setEqBandCount(const std::string& outputId, const std::string& eqInstanceId,
                         std::size_t count);
    bool setEqBand(const std::string& outputId, const std::string& eqInstanceId, std::size_t index,
                    const eqcore::EqBand& band);
    std::vector<eqcore::EqBand> getEqBands(const std::string& outputId,
                                            const std::string& eqInstanceId) const;

    // Copies an instance (and its bands) onto another output. This is how one
    // curve gets shared across devices - instances are owned per output, so
    // cross-device sharing is a copy rather than a shared reference.
    std::string copyEqInstance(const std::string& sourceOutputId, const std::string& eqInstanceId,
                                const std::string& targetOutputId);

    // Convenience: resolve a channel to its EQ instance. The band-setting forms
    // CREATE an instance on demand and assign it to the channel, which lets a
    // caller edit a channel's EQ without knowing instances exist at all.
    std::vector<eqcore::EqBand> getChannelEqBands(const std::string& outputId,
                                                   std::size_t channelIndex) const;
    bool setChannelEqBandCount(const std::string& outputId, std::size_t channelIndex,
                                std::size_t count);
    bool setChannelEqBand(const std::string& outputId, std::size_t channelIndex, std::size_t index,
                           const eqcore::EqBand& band);

    // ------------------------------------------------------- link groups --

    std::vector<LinkGroupInfo> listLinkGroups(const std::string& outputId) const;
    // Joins channels so they share fader, mute and send levels. The group
    // adopts its LOWEST-INDEX member's values, so linking is never ambiguous
    // about which side wins. Returns the new group id, or empty on failure
    // (fewer than two channels, an out-of-range index, or a channel that is
    // already in another group).
    std::string createLinkGroup(const std::string& outputId,
                                 const std::vector<uint32_t>& channelIndices,
                                 const std::string& displayName);
    bool removeLinkGroup(const std::string& outputId, const std::string& groupId);
    bool setLinkGroupChannels(const std::string& outputId, const std::string& groupId,
                               const std::vector<uint32_t>& channelIndices);

    // ------------------------------------------------------------- sends --

    // Every routed (channel, input) pair of this output. A pair that is absent
    // is not routed at all, which is deliberately distinct from a 0 dB send.
    std::vector<SendInfo> getSends(const std::string& outputId) const;
    // Sets one input's level into one channel, linked across the channel's
    // group. False if either id is unknown, or if the output's send pool
    // (kMaxInputs) is full and this input has no slot yet.
    bool setSend(const std::string& outputId, std::size_t channelIndex, const std::string& inputId,
                  double gainDb);
    bool removeSend(const std::string& outputId, std::size_t channelIndex, const std::string& inputId);

    // ----------------------------------------------------------- metering --

    // Peak magnitude per channel since the previous call, then resets. Empty
    // for an output that isn't connected.
    std::vector<float> takeOutputPeaks(const std::string& outputId);

    static constexpr int kDefaultInputChannels = 2;
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
    //
    // `desired` is the v2 per-channel OutputConfig. While this engine's public
    // API is still stereo-pair shaped, the pair is channels[0]/channels[1]
    // kept in one link group - see the pair-view helpers at the top of
    // audio_engine.cpp, which are the only place that mapping lives.
    struct RouteEntry {
        eqcore::OutputConfig desired;
        std::unique_ptr<OutputStream> live;
        uint32_t targetNodeId = SPA_ID_INVALID; // node id `live` was connected to

        // The layout `live`'s stream was CREATED with, so reconcile() can tell
        // a profile switch (which needs renegotiation) from a mere property
        // refresh (which must not tear the stream down).
        std::vector<std::string> livePositions;
        // How many of desired.channels the device actually drives. Entries
        // beyond this are retired configs; see OutputConfig::channels.
        std::size_t liveChannelCount = 0;
    };

    // All of these assume controlMutex_ is held.
    RouteEntry* findEntryLocked(const std::string& routeId);
    const RouteEntry* findEntryLocked(const std::string& routeId) const;
    InputSource* findInputLocked(const std::string& inputId) const;
    std::string addInputLocked(std::string inputId, const std::string& displayName,
                                const std::vector<std::string>& positions);
    // Every channel index a set on `channelIndex` must write: its whole link
    // group, or just itself when ungrouped. Empty for an out-of-range index.
    std::vector<std::size_t> linkedChannelsLocked(RouteEntry& entry, std::size_t channelIndex) const;
    // The channel's EQ instance, created and assigned on demand.
    eqcore::EqInstanceConfig& channelEqInstanceLocked(RouteEntry& entry, std::size_t channelIndex);
    // Copies the lowest-index member's gain/mute/sends onto the rest.
    void adoptGroupLeaderLocked(RouteEntry& entry, const std::vector<uint32_t>& channelIndices);
    // Points every member at the lowest-index member's EQ instance, so a linked
    // group shares one curve rather than copies that can drift apart.
    void shareGroupEqLocked(RouteEntry& entry, const std::vector<uint32_t>& channelIndices);
    // The inverse: gives each member its own copy, so unlinking really does
    // separate them.
    void splitGroupEqLocked(RouteEntry& entry, const std::vector<uint32_t>& channelIndices);
    // Drops instances no channel references. Assignment isn't user-controllable,
    // so an unreferenced instance is unreachable rather than a saved preset.
    void pruneUnreferencedEqLocked(RouteEntry& entry);
    // The distinct inputs any channel of this output sends from, which is what
    // the per-output send pool (kMaxInputs) actually bounds.
    std::set<std::string> routedInputsLocked(const RouteEntry& entry) const;
    void connectLocked(RouteEntry& entry, const DeviceInfo& device);
    // Rebuilds and publishes the RT snapshot for a live output from its desired
    // configuration. The single funnel every mutation goes through, so there is
    // exactly one place that knows how config maps onto realtime state.
    void republishLocked(RouteEntry& entry);
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
    std::atomic<bool> devicesChangedForClients_{false};

    std::vector<std::unique_ptr<BoundNode>> boundNodes_; // loop thread only

    mutable std::mutex controlMutex_;
    std::vector<std::unique_ptr<InputSource>> inputs_;
    std::vector<RouteEntry> routes_;
    int nextRouteIndex_ = 1;
    int nextInputIndex_ = 1;
};

} // namespace pipeeq
