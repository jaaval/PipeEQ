#include "dbus_service.h"

#include <cstdio>

#include "dbus_interface.h"
#include "route_config.h"

namespace pipeeq {

namespace {

eqcore::FilterType filterTypeFromString(const std::string& s) {
    return nlohmann::json(s).get<eqcore::FilterType>();
}

std::string filterTypeToString(eqcore::FilterType t) {
    return nlohmann::json(t).get<std::string>();
}

} // namespace

DbusService::DbusService(AudioEngine& engine) : engine_(engine) {}

DbusService::~DbusService() {
    stop();
}

void DbusService::start() {
    eqcore::AppConfig config = eqcore::loadConfig();
    for (const auto& routeConfig : config.routes) {
        const std::string routeId = engine_.addRoute(routeConfig.deviceName, routeConfig.displayName,
                                                       routeConfig.gainDb);
        if (routeId.empty()) {
            std::fprintf(stderr,
                         "pipeeq: device '%s' from saved config isn't currently available; skipping route "
                         "'%s'\n",
                         routeConfig.deviceName.c_str(), routeConfig.displayName.c_str());
            continue;
        }
        engine_.setRouteMuted(routeId, routeConfig.muted);
        engine_.setRouteBandCount(routeId, routeConfig.bands.size());
        for (std::size_t i = 0; i < routeConfig.bands.size(); ++i) {
            engine_.setRouteBand(routeId, i, routeConfig.bands[i]);
        }
    }

    connection_ = sdbus::createSessionBusConnection(sdbus::ServiceName{eqcore::dbus::kServiceName});
    object_ = sdbus::createObject(*connection_, sdbus::ObjectPath{eqcore::dbus::kObjectPath});

    object_
        ->addVTable(
            sdbus::registerMethod(eqcore::dbus::kMethodListDevices).implementedAs([this] { return listDevices(); }),
            sdbus::registerMethod(eqcore::dbus::kMethodListRoutes).implementedAs([this] { return listRoutes(); }),
            sdbus::registerMethod(eqcore::dbus::kMethodGetState).implementedAs([this] { return getState(); }),
            sdbus::registerMethod(eqcore::dbus::kMethodGetRouteBands)
                .implementedAs([this](std::string routeId) { return getRouteBands(routeId); }),
            sdbus::registerMethod(eqcore::dbus::kMethodAddRoute)
                .implementedAs([this](std::string deviceName, std::string displayName) {
                    return addRoute(deviceName, displayName);
                }),
            sdbus::registerMethod(eqcore::dbus::kMethodRemoveRoute)
                .implementedAs([this](std::string routeId) { removeRoute(routeId); }),
            sdbus::registerMethod(eqcore::dbus::kMethodSetRouteGain)
                .implementedAs([this](std::string routeId, double gainDb) { return setRouteGain(routeId, gainDb); }),
            sdbus::registerMethod(eqcore::dbus::kMethodSetRouteMute)
                .implementedAs([this](std::string routeId, bool muted) { return setRouteMute(routeId, muted); }),
            sdbus::registerMethod(eqcore::dbus::kMethodSetRouteBandCount)
                .implementedAs(
                    [this](std::string routeId, uint32_t count) { return setRouteBandCount(routeId, count); }),
            sdbus::registerMethod(eqcore::dbus::kMethodSetRouteBand)
                .implementedAs([this](std::string routeId, uint32_t index, std::string type, double freqHz,
                                       double gainDb, double q) {
                    return setRouteBand(routeId, index, type, freqHz, gainDb, q);
                }),
            sdbus::registerSignal(eqcore::dbus::kSignalDevicesChanged),
            sdbus::registerSignal(eqcore::dbus::kSignalRouteChanged).withParameters<std::string>())
        .forInterface(sdbus::InterfaceName{eqcore::dbus::kInterfaceName});

    connection_->enterEventLoopAsync();

    std::printf("pipeeq-daemon: D-Bus service running as %s\n", eqcore::dbus::kServiceName);
}

void DbusService::stop() {
    if (connection_) {
        connection_->leaveEventLoop();
    }
    object_.reset();
    connection_.reset();
}

std::vector<DbusService::DeviceRow> DbusService::listDevices() {
    std::vector<DeviceRow> rows;
    for (const auto& d : engine_.listDevices()) {
        rows.push_back(DeviceRow{d.id, d.nodeName, d.description});
    }
    return rows;
}

std::vector<DbusService::RouteRow> DbusService::listRoutes() {
    std::vector<RouteRow> rows;
    for (const auto& r : engine_.listRoutes()) {
        rows.push_back(RouteRow{r.id, r.deviceName, r.displayName, r.gainDb, r.muted,
                                  static_cast<uint32_t>(r.bandCount)});
    }
    return rows;
}

std::vector<DbusService::BandRow> DbusService::getRouteBands(const std::string& routeId) {
    std::vector<BandRow> rows;
    for (const auto& band : engine_.getRouteBands(routeId)) {
        rows.push_back(BandRow{filterTypeToString(band.type), band.freqHz, band.gainDb, band.q});
    }
    return rows;
}

DbusService::StateRow DbusService::getState() {
    return StateRow{listDevices(), listRoutes()};
}

std::string DbusService::addRoute(const std::string& deviceName, const std::string& displayName) {
    const std::string routeId = engine_.addRoute(deviceName, displayName);
    if (!routeId.empty()) {
        persistConfig();
        emitRouteChanged(routeId);
    }
    return routeId;
}

void DbusService::removeRoute(const std::string& routeId) {
    engine_.removeRoute(routeId);
    persistConfig();
    emitRouteChanged(routeId);
}

bool DbusService::setRouteGain(const std::string& routeId, double gainDb) {
    const bool ok = engine_.setRouteGain(routeId, gainDb);
    if (ok) {
        persistConfig();
        emitRouteChanged(routeId);
    }
    return ok;
}

bool DbusService::setRouteMute(const std::string& routeId, bool muted) {
    const bool ok = engine_.setRouteMuted(routeId, muted);
    if (ok) {
        persistConfig();
        emitRouteChanged(routeId);
    }
    return ok;
}

bool DbusService::setRouteBandCount(const std::string& routeId, uint32_t count) {
    const bool ok = engine_.setRouteBandCount(routeId, count);
    if (ok) {
        persistConfig();
        emitRouteChanged(routeId);
    }
    return ok;
}

bool DbusService::setRouteBand(const std::string& routeId, uint32_t index, const std::string& type,
                                double freqHz, double gainDb, double q) {
    eqcore::EqBand band;
    band.type = filterTypeFromString(type);
    band.freqHz = freqHz;
    band.gainDb = gainDb;
    band.q = q;

    const bool ok = engine_.setRouteBand(routeId, index, band);
    if (ok) {
        persistConfig();
        emitRouteChanged(routeId);
    }
    return ok;
}

void DbusService::emitRouteChanged(const std::string& routeId) {
    object_->emitSignal(eqcore::dbus::kSignalRouteChanged)
        .onInterface(eqcore::dbus::kInterfaceName)
        .withArguments(routeId);
}

void DbusService::persistConfig() {
    eqcore::AppConfig config;
    for (const auto& info : engine_.listRoutes()) {
        eqcore::RouteConfig routeConfig;
        routeConfig.id = info.id;
        routeConfig.deviceName = info.deviceName;
        routeConfig.displayName = info.displayName;
        routeConfig.gainDb = info.gainDb;
        routeConfig.muted = info.muted;
        routeConfig.bands = engine_.getRouteBands(info.id);
        config.routes.push_back(std::move(routeConfig));
    }
    eqcore::saveConfig(config);
}

} // namespace pipeeq
