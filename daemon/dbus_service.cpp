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

    // Migration: configs saved before the mixer feature have no inputs at
    // all - synthesize one default so upgrading is a no-op for existing
    // single-input users (every saved route implicitly heard "the" input).
    if (config.inputs.empty()) {
        eqcore::InputConfig defaultInput;
        defaultInput.id = "input-1";
        defaultInput.displayName = "Default";
        config.inputs.push_back(defaultInput);
        for (auto& routeConfig : config.routes) {
            routeConfig.inputGainsDb[defaultInput.id] = 0.0;
        }
    }

    // The engine restores inputs and routes verbatim, keeping the saved ids
    // (so the mix levels keyed by input id stay valid) and keeping routes
    // whose device isn't plugged in right now as pending rather than
    // dropping them.
    engine_.applyConfig(config);

#if SDBUSCPP_MAJOR_VERSION >= 2
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
                .implementedAs([this](std::string deviceName, std::string displayName, std::string leftChannel,
                                       std::string rightChannel) {
                    return addRoute(deviceName, displayName, leftChannel, rightChannel);
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
            sdbus::registerMethod(eqcore::dbus::kMethodListInputs).implementedAs([this] { return listInputs(); }),
            sdbus::registerMethod(eqcore::dbus::kMethodAddInput)
                .implementedAs([this](std::string displayName) { return addInput(displayName); }),
            sdbus::registerMethod(eqcore::dbus::kMethodRemoveInput)
                .implementedAs([this](std::string inputId) { removeInput(inputId); }),
            sdbus::registerMethod(eqcore::dbus::kMethodSetRouteInputGain)
                .implementedAs([this](std::string routeId, std::string inputId, double gainDb) {
                    return setRouteInputGain(routeId, inputId, gainDb);
                }),
            sdbus::registerMethod(eqcore::dbus::kMethodRemoveRouteInput)
                .implementedAs([this](std::string routeId, std::string inputId) {
                    return removeRouteInput(routeId, inputId);
                }),
            sdbus::registerMethod(eqcore::dbus::kMethodGetRouteInputGains)
                .implementedAs([this](std::string routeId) { return getRouteInputGains(routeId); }),
            sdbus::registerMethod(eqcore::dbus::kMethodGetMixMatrix)
                .implementedAs([this] { return getMixMatrix(); }),
            sdbus::registerMethod(eqcore::dbus::kMethodSetRouteAutoConnect)
                .implementedAs([this](std::string routeId, bool autoConnect) {
                    return setRouteAutoConnect(routeId, autoConnect);
                }),
            sdbus::registerMethod(eqcore::dbus::kMethodSetRouteChannels)
                .implementedAs([this](std::string routeId, std::string leftChannel, std::string rightChannel) {
                    return setRouteChannels(routeId, leftChannel, rightChannel);
                }),
            sdbus::registerSignal(eqcore::dbus::kSignalDevicesChanged),
            sdbus::registerSignal(eqcore::dbus::kSignalInputsChanged),
            sdbus::registerSignal(eqcore::dbus::kSignalRouteChanged).withParameters<std::string>())
        .forInterface(sdbus::InterfaceName{eqcore::dbus::kInterfaceName});
#else
    // sdbus-c++ 1.x: no strong ServiceName/ObjectPath/InterfaceName types, and
    // no addVTable() - each method/signal is registered individually, then
    // finalized with finishRegistration().
    connection_ = sdbus::createSessionBusConnection(std::string(eqcore::dbus::kServiceName));
    object_ = sdbus::createObject(*connection_, std::string(eqcore::dbus::kObjectPath));

    object_->registerMethod(eqcore::dbus::kMethodListDevices)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this] { return listDevices(); });
    object_->registerMethod(eqcore::dbus::kMethodListRoutes)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this] { return listRoutes(); });
    object_->registerMethod(eqcore::dbus::kMethodGetState)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this] { return getState(); });
    object_->registerMethod(eqcore::dbus::kMethodGetRouteBands)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this](std::string routeId) { return getRouteBands(routeId); });
    object_->registerMethod(eqcore::dbus::kMethodAddRoute)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this](std::string deviceName, std::string displayName, std::string leftChannel,
                               std::string rightChannel) {
            return addRoute(deviceName, displayName, leftChannel, rightChannel);
        });
    object_->registerMethod(eqcore::dbus::kMethodRemoveRoute)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this](std::string routeId) { removeRoute(routeId); });
    object_->registerMethod(eqcore::dbus::kMethodSetRouteGain)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this](std::string routeId, double gainDb) { return setRouteGain(routeId, gainDb); });
    object_->registerMethod(eqcore::dbus::kMethodSetRouteMute)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this](std::string routeId, bool muted) { return setRouteMute(routeId, muted); });
    object_->registerMethod(eqcore::dbus::kMethodSetRouteBandCount)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs(
            [this](std::string routeId, uint32_t count) { return setRouteBandCount(routeId, count); });
    object_->registerMethod(eqcore::dbus::kMethodSetRouteBand)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this](std::string routeId, uint32_t index, std::string type, double freqHz,
                               double gainDb, double q) {
            return setRouteBand(routeId, index, type, freqHz, gainDb, q);
        });
    object_->registerMethod(eqcore::dbus::kMethodListInputs)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this] { return listInputs(); });
    object_->registerMethod(eqcore::dbus::kMethodAddInput)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this](std::string displayName) { return addInput(displayName); });
    object_->registerMethod(eqcore::dbus::kMethodRemoveInput)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this](std::string inputId) { removeInput(inputId); });
    object_->registerMethod(eqcore::dbus::kMethodSetRouteInputGain)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this](std::string routeId, std::string inputId, double gainDb) {
            return setRouteInputGain(routeId, inputId, gainDb);
        });
    object_->registerMethod(eqcore::dbus::kMethodRemoveRouteInput)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs(
            [this](std::string routeId, std::string inputId) { return removeRouteInput(routeId, inputId); });
    object_->registerMethod(eqcore::dbus::kMethodGetRouteInputGains)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this](std::string routeId) { return getRouteInputGains(routeId); });
    object_->registerMethod(eqcore::dbus::kMethodGetMixMatrix)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this] { return getMixMatrix(); });
    object_->registerMethod(eqcore::dbus::kMethodSetRouteAutoConnect)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs(
            [this](std::string routeId, bool autoConnect) { return setRouteAutoConnect(routeId, autoConnect); });
    object_->registerMethod(eqcore::dbus::kMethodSetRouteChannels)
        .onInterface(eqcore::dbus::kInterfaceName)
        .implementedAs([this](std::string routeId, std::string leftChannel, std::string rightChannel) {
            return setRouteChannels(routeId, leftChannel, rightChannel);
        });
    object_->registerSignal(eqcore::dbus::kSignalDevicesChanged).onInterface(eqcore::dbus::kInterfaceName);
    object_->registerSignal(eqcore::dbus::kSignalInputsChanged).onInterface(eqcore::dbus::kInterfaceName);
    object_->registerSignal(eqcore::dbus::kSignalRouteChanged)
        .onInterface(eqcore::dbus::kInterfaceName)
        .withParameters<std::string>();

    object_->finishRegistration();
#endif

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

void DbusService::tick() {
    // Connecting or disconnecting a route doesn't change its desired
    // configuration, so there's nothing to persist and nothing to announce -
    // the engine logs what it did, and the GUI picks up the new connected
    // state on its periodic refresh.
    engine_.reconcile();
}

std::vector<DbusService::DeviceRow> DbusService::listDevices() {
    std::vector<DeviceRow> rows;
    for (const auto& d : engine_.listDevices()) {
        // One row per stereo pair: a 4.0 interface is two selectable outputs.
        for (const auto& pair : d.pairs) {
            rows.push_back(DeviceRow{d.id, d.nodeName, d.description, pair.label, pair.leftName,
                                      pair.rightName});
        }
    }
    return rows;
}

std::vector<DbusService::RouteRow> DbusService::listRoutes() {
    std::vector<RouteRow> rows;
    for (const auto& r : engine_.listRoutes()) {
        rows.push_back(RouteRow{r.id, r.deviceName, r.displayName, r.gainDb, r.muted,
                                  static_cast<uint32_t>(r.bandCount), r.connected, r.autoConnect,
                                  r.leftChannel, r.rightChannel});
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

std::vector<DbusService::InputRow> DbusService::listInputs() {
    std::vector<InputRow> rows;
    for (const auto& in : engine_.listInputs()) {
        rows.push_back(InputRow{in.id, in.displayName});
    }
    return rows;
}

std::vector<DbusService::InputGainRow> DbusService::getRouteInputGains(const std::string& routeId) {
    std::vector<InputGainRow> rows;
    for (const auto& [inputId, gainDb] : engine_.getRouteInputGains(routeId)) {
        rows.push_back(InputGainRow{inputId, gainDb});
    }
    return rows;
}

std::vector<DbusService::MixMatrixRow> DbusService::getMixMatrix() {
    std::vector<MixMatrixRow> rows;
    for (const auto& route : engine_.listRoutes()) {
        for (const auto& [inputId, gainDb] : engine_.getRouteInputGains(route.id)) {
            rows.push_back(MixMatrixRow{route.id, inputId, gainDb});
        }
    }
    return rows;
}

DbusService::StateRow DbusService::getState() {
    return StateRow{listDevices(), listRoutes(), listInputs()};
}

std::string DbusService::addRoute(const std::string& deviceName, const std::string& displayName,
                                   const std::string& leftChannel, const std::string& rightChannel) {
    const std::string routeId = engine_.addRoute(deviceName, displayName, leftChannel, rightChannel);
    persistConfig();
    emitRouteChanged(routeId);
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

bool DbusService::setRouteAutoConnect(const std::string& routeId, bool autoConnect) {
    const bool ok = engine_.setRouteAutoConnect(routeId, autoConnect);
    if (ok) {
        persistConfig();
        emitRouteChanged(routeId);
    }
    return ok;
}

bool DbusService::setRouteChannels(const std::string& routeId, const std::string& leftChannel,
                                    const std::string& rightChannel) {
    const bool ok = engine_.setRouteChannels(routeId, leftChannel, rightChannel);
    if (ok) {
        persistConfig();
        emitRouteChanged(routeId);
    }
    return ok;
}

std::string DbusService::addInput(const std::string& displayName) {
    const std::string inputId = engine_.addInput(displayName);
    persistConfig();
    emitInputsChanged();
    return inputId;
}

void DbusService::removeInput(const std::string& inputId) {
    engine_.removeInput(inputId);
    persistConfig();
    emitInputsChanged();
}

bool DbusService::setRouteInputGain(const std::string& routeId, const std::string& inputId, double gainDb) {
    const bool ok = engine_.setRouteInputGain(routeId, inputId, gainDb);
    if (ok) {
        persistConfig();
        emitRouteChanged(routeId);
    }
    return ok;
}

bool DbusService::removeRouteInput(const std::string& routeId, const std::string& inputId) {
    const bool ok = engine_.removeRouteInput(routeId, inputId);
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

void DbusService::emitInputsChanged() {
    object_->emitSignal(eqcore::dbus::kSignalInputsChanged).onInterface(eqcore::dbus::kInterfaceName);
}

void DbusService::persistConfig() {
    // snapshotConfig() reports every configured route, including ones whose
    // device isn't currently plugged in. Rebuilding this from only the live
    // routes instead is what used to erase a powered-off device's EQ from
    // the config on the next slider move.
    std::lock_guard<std::mutex> lock(persistMutex_);
    eqcore::saveConfig(engine_.snapshotConfig());
}

} // namespace pipeeq
