#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "audio_engine.h"

namespace pipeeq {

// Exposes org.pipeeq.Daemon1 on the session bus, backed by an AudioEngine.
//
// Also owns config persistence: start() recreates inputs and outputs from the
// saved AppConfig before serving requests, and every mutating call marks the
// config dirty for a debounced save.
//
// Threading: this class owns ONE service thread that both dispatches incoming
// D-Bus messages and drives everything periodic - device reconciliation, the
// debounced config save, and signal emission. That unification is the point.
// Previously reconciliation ran on the main thread while sdbus dispatched on
// another, which meant the reconciler could not emit a signal at all (writing
// the connection from two threads isn't safe), and DevicesChanged was
// registered but never actually emitted. With one thread owning the connection,
// it can.
class DbusService {
public:
    explicit DbusService(AudioEngine& engine);
    ~DbusService();

    DbusService(const DbusService&) = delete;
    DbusService& operator=(const DbusService&) = delete;

    // Loads the config, applies it to the engine, exports the interface, and
    // starts the service thread. Returns false if the bus name can't be taken.
    bool start();
    void stop();

private:
    // nodeId, nodeName, description, [(channelIndex, positionName)], inUse
    using DeviceChannelRow = sdbus::Struct<uint32_t, std::string>;
    using DeviceRow = sdbus::Struct<uint32_t, std::string, std::string, std::vector<DeviceChannelRow>, bool>;
    // id, deviceName, displayName, connected, autoConnect, channelCount,
    // liveChannelCount, sampleRateHz
    using OutputRow =
        sdbus::Struct<std::string, std::string, std::string, bool, bool, uint32_t, uint32_t, uint32_t>;
    // index, position, displayName, gainDb, muted, eqInstanceId, groupId, driven
    using ChannelRow =
        sdbus::Struct<uint32_t, std::string, std::string, double, bool, std::string, std::string, bool>;
    // id, displayName, bandCount, bypassed, channelCount
    using EqInstanceRow = sdbus::Struct<std::string, std::string, uint32_t, bool, uint32_t>;
    using BandRow = sdbus::Struct<std::string, double, double, double>;
    // id, displayName, channelIndices
    using LinkGroupRow = sdbus::Struct<std::string, std::string, std::vector<uint32_t>>;
    // channelIndex, inputId, gainDb
    using SendRow = sdbus::Struct<uint32_t, std::string, double>;
    // id, displayName, channel positions
    using InputRow = sdbus::Struct<std::string, std::string, std::vector<std::string>>;
    using StateRow =
        sdbus::Struct<std::vector<DeviceRow>, std::vector<OutputRow>, std::vector<InputRow>>;
    // outputId (or inputId), per-channel peak in dB
    using MeterRow = sdbus::Struct<std::string, std::vector<double>>;

    void registerInterface();
    void serviceLoop();

    // ------------------------------------------------------------ readers --
    std::vector<DeviceRow> listDevices();
    std::vector<OutputRow> listOutputs();
    std::vector<ChannelRow> getOutputChannels(const std::string& outputId);
    std::vector<EqInstanceRow> listEqInstances(const std::string& outputId);
    std::vector<BandRow> getEqBands(const std::string& outputId, const std::string& eqInstanceId);
    std::vector<BandRow> getChannelEqBands(const std::string& outputId, uint32_t channelIndex);
    std::vector<LinkGroupRow> listLinkGroups(const std::string& outputId);
    std::vector<SendRow> getSends(const std::string& outputId);
    std::vector<InputRow> listInputs();
    StateRow getState();

    // ---------------------------------------------------------- mutators --
    std::string addOutput(const std::string& deviceName, const std::string& displayName);
    void removeOutput(const std::string& outputId);
    bool setOutputDisplayName(const std::string& outputId, const std::string& displayName);
    bool setOutputAutoConnect(const std::string& outputId, bool autoConnect);

    bool setChannelGain(const std::string& outputId, uint32_t channelIndex, double gainDb);
    bool setChannelMuted(const std::string& outputId, uint32_t channelIndex, bool muted);
    bool setChannelPosition(const std::string& outputId, uint32_t channelIndex,
                             const std::string& position);
    bool setChannelDisplayName(const std::string& outputId, uint32_t channelIndex,
                                const std::string& displayName);
    bool setChannelEqInstance(const std::string& outputId, uint32_t channelIndex,
                               const std::string& eqInstanceId);

    std::string addEqInstance(const std::string& outputId, const std::string& displayName);
    bool removeEqInstance(const std::string& outputId, const std::string& eqInstanceId);
    bool setEqInstanceName(const std::string& outputId, const std::string& eqInstanceId,
                            const std::string& displayName);
    bool setEqBypassed(const std::string& outputId, const std::string& eqInstanceId, bool bypassed);
    bool setEqBandCount(const std::string& outputId, const std::string& eqInstanceId, uint32_t count);
    bool setEqBand(const std::string& outputId, const std::string& eqInstanceId, uint32_t index,
                    const std::string& type, double freqHz, double gainDb, double q);
    std::string copyEqInstance(const std::string& sourceOutputId, const std::string& eqInstanceId,
                                const std::string& targetOutputId);
    bool setChannelEqBandCount(const std::string& outputId, uint32_t channelIndex, uint32_t count);
    bool setChannelEqBand(const std::string& outputId, uint32_t channelIndex, uint32_t index,
                           const std::string& type, double freqHz, double gainDb, double q);

    std::string createLinkGroup(const std::string& outputId,
                                 const std::vector<uint32_t>& channelIndices,
                                 const std::string& displayName);
    bool removeLinkGroup(const std::string& outputId, const std::string& groupId);
    bool setLinkGroupChannels(const std::string& outputId, const std::string& groupId,
                               const std::vector<uint32_t>& channelIndices);

    bool setSend(const std::string& outputId, uint32_t channelIndex, const std::string& inputId,
                  double gainDb);
    bool removeSend(const std::string& outputId, uint32_t channelIndex, const std::string& inputId);

    std::string addInput(const std::string& displayName, const std::vector<std::string>& positions);
    void removeInput(const std::string& inputId);
    bool setInputDisplayName(const std::string& inputId, const std::string& displayName);

    void setMeteringEnabled(bool enabled);

    // ---------------------------------------------------- signals, saving --
    // All emitted from the service thread, which is the only thread that writes
    // the connection.
    void emitOutputChanged(const std::string& outputId);
    void emitOutputsChanged();
    void emitInputsChanged();
    void emitDevicesChanged();
    void emitMeters();

    // Queued from the dispatch side, drained on the service loop. A method
    // handler runs ON the service thread (sdbus dispatches there), so these
    // could be emitted inline - but queueing keeps "who emits" to one place and
    // coalesces a burst of mutations on the same output into one signal.
    void queueOutputChanged(const std::string& outputId);
    void queueInputsChanged();

    void markConfigDirty();
    void flushConfigIfDue(bool force);

    // Long enough that a fader drag is one write rather than dozens, short
    // enough that a kill -9 right after an edit rarely loses it.
    static constexpr std::chrono::milliseconds kSaveDebounce{500};
    // Worst-case delay before an output whose hardware just appeared starts
    // playing. Device arrivals are reported on PipeWire's loop thread, which
    // can't do the connect work itself without inverting the engine's lock
    // order, so it hands off to this loop.
    static constexpr std::chrono::milliseconds kReconcileInterval{200};
    static constexpr std::chrono::milliseconds kMeterInterval{33}; // ~30 Hz
    // How long one SetMeteringEnabled(true) keeps metering armed. Clients
    // re-arm about once a second while they're watching; a client that dies
    // stops metering on its own, with no bus-name tracking here.
    static constexpr std::chrono::milliseconds kMeterLease{3000};

    AudioEngine& engine_;
    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IObject> object_;

    std::thread serviceThread_;
    std::atomic<bool> running_{false};

    // Guards the persistence state and serializes gather-then-write so two
    // concurrent saves can't interleave into the config file.
    std::mutex persistMutex_;
    bool configDirty_ = false;
    std::chrono::steady_clock::time_point dirtySince_{};
    // Set for the whole session when the config on disk couldn't be read. The
    // daemon then serves normally but never writes, so an unparseable file is
    // never destroyed by the next mutation.
    bool persistDisabled_ = false;

    std::mutex pendingMutex_;
    std::vector<std::string> pendingOutputChanges_;
    bool pendingInputsChanged_ = false;

    std::atomic<std::chrono::steady_clock::duration> meterLeaseUntil_{
        std::chrono::steady_clock::duration::zero()};
};

} // namespace pipeeq
