#pragma once

#include <memory>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "audio_engine.h"

namespace pipeeq {

// Exposes org.pipeeq.Daemon1 on the session bus, backed by an AudioEngine.
// Also owns config persistence: start() recreates routes from the saved
// AppConfig before serving requests, and every mutating call re-saves it.
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
    using StateRow = sdbus::Struct<std::vector<DeviceRow>, std::vector<RouteRow>>;

    std::vector<DeviceRow> listDevices();
    std::vector<RouteRow> listRoutes();
    std::vector<BandRow> getRouteBands(const std::string& routeId);
    StateRow getState();
    std::string addRoute(const std::string& deviceName, const std::string& displayName);
    void removeRoute(const std::string& routeId);
    bool setRouteGain(const std::string& routeId, double gainDb);
    bool setRouteMute(const std::string& routeId, bool muted);
    bool setRouteBandCount(const std::string& routeId, uint32_t count);
    bool setRouteBand(const std::string& routeId, uint32_t index, const std::string& type, double freqHz,
                       double gainDb, double q);

    void emitRouteChanged(const std::string& routeId);
    void persistConfig();

    AudioEngine& engine_;
    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IObject> object_;
};

} // namespace pipeeq
