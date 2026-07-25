#pragma once

#include <memory>
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

private:
    using DeviceRow = sdbus::Struct<uint32_t, std::string, std::string>;
    using RouteRow = sdbus::Struct<std::string, std::string, std::string, double, bool, uint32_t>;
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

    std::string addRoute(const std::string& deviceName, const std::string& displayName);
    void removeRoute(const std::string& routeId);
    bool setRouteGain(const std::string& routeId, double gainDb);
    bool setRouteMute(const std::string& routeId, bool muted);
    bool setRouteBandCount(const std::string& routeId, uint32_t count);
    bool setRouteBand(const std::string& routeId, uint32_t index, const std::string& type, double freqHz,
                       double gainDb, double q);

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
};

} // namespace pipeeq
