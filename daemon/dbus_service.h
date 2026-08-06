#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "audio_engine.h"

namespace pipeeq {

// Exposes org.pipeeq.Daemon1 on the session bus, backed by an AudioEngine.
// Also owns config persistence: start() recreates inputs/routes from the
// saved AppConfig before serving requests (synthesizing one default input
// if the config predates the mixer feature, so upgrading is a no-op for
// existing single-input users), and every mutating call re-saves it.
class DbusService {
public:
    explicit DbusService(AudioEngine& engine);
    ~DbusService();

    DbusService(const DbusService&) = delete;
    DbusService& operator=(const DbusService&) = delete;

    void start();
    void stop();

    // Drives the engine's device reconciliation (connecting outputs whose
    // hardware has appeared). Called from the main thread's loop, not from
    // the D-Bus dispatch thread - so it deliberately doesn't emit signals,
    // since the sd-bus connection is being dispatched on that other thread
    // and isn't safe to write from two threads at once. The GUI notices the
    // resulting connected/disconnected state via its periodic refresh.
    void tick();

private:
    // One row per selectable stereo pair, so a 4.0 interface appears twice:
    // nodeId, nodeName, description, pairLabel, leftChannel, rightChannel.
    // leftChannel/rightChannel are empty for a device whose layout PipeEQ
    // doesn't recognize, meaning "let the device decide".
    using DeviceRow =
        sdbus::Struct<uint32_t, std::string, std::string, std::string, std::string, std::string>;
    // id, deviceName, displayName, gainDb, muted, bandCount, connected,
    // autoConnect, leftChannel, rightChannel
    using RouteRow = sdbus::Struct<std::string, std::string, std::string, double, bool, uint32_t, bool, bool,
                                    std::string, std::string>;
    using BandRow = sdbus::Struct<std::string, double, double, double>;
    using InputRow = sdbus::Struct<std::string, std::string>;
    using InputGainRow = sdbus::Struct<std::string, double>;
    using MixMatrixRow = sdbus::Struct<std::string, std::string, double>; // routeId, inputId, gainDb
    using StateRow = sdbus::Struct<std::vector<DeviceRow>, std::vector<RouteRow>, std::vector<InputRow>>;

    std::vector<DeviceRow> listDevices();
    std::vector<RouteRow> listRoutes();
    std::vector<BandRow> getRouteBands(const std::string& routeId);
    std::vector<InputRow> listInputs();
    std::vector<InputGainRow> getRouteInputGains(const std::string& routeId);
    std::vector<MixMatrixRow> getMixMatrix();
    StateRow getState();

    std::string addRoute(const std::string& deviceName, const std::string& displayName,
                          const std::string& leftChannel, const std::string& rightChannel);
    void removeRoute(const std::string& routeId);
    bool setRouteGain(const std::string& routeId, double gainDb);
    bool setRouteMute(const std::string& routeId, bool muted);
    bool setRouteBandCount(const std::string& routeId, uint32_t count);
    bool setRouteBand(const std::string& routeId, uint32_t index, const std::string& type, double freqHz,
                       double gainDb, double q);
    bool setRouteAutoConnect(const std::string& routeId, bool autoConnect);
    bool setRouteChannels(const std::string& routeId, const std::string& leftChannel,
                           const std::string& rightChannel);

    std::string addInput(const std::string& displayName);
    void removeInput(const std::string& inputId);
    bool setRouteInputGain(const std::string& routeId, const std::string& inputId, double gainDb);
    bool removeRouteInput(const std::string& routeId, const std::string& inputId);

    void emitRouteChanged(const std::string& routeId);
    void emitInputsChanged();
    void persistConfig();

    AudioEngine& engine_;
    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IObject> object_;
    // Serializes gather-then-write so two concurrent saves can't interleave
    // into the config file.
    std::mutex persistMutex_;
};

} // namespace pipeeq
