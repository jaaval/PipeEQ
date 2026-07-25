#include "dbus_client.h"

#include <QDebug>

#include "dbus_interface.h"
#include "route_config.h"

namespace pipeeq {

namespace {
using DeviceStructRow = sdbus::Struct<uint32_t, std::string, std::string>;
using RouteStructRow = sdbus::Struct<std::string, std::string, std::string, double, bool, uint32_t>;
using BandStructRow = sdbus::Struct<std::string, double, double, double>;
using InputStructRow = sdbus::Struct<std::string, std::string>;
using InputGainStructRow = sdbus::Struct<std::string, double>;
} // namespace

DbusClient::DbusClient(QObject* parent) : QObject(parent) {
    // Explicit session-bus connection rather than sdbus-c++'s ambiguous
    // "default bus" resolution, which (like plain `busctl` with no --user
    // flag) can resolve to the system bus in some environments - the daemon
    // only ever registers on the session bus.
#if SDBUSCPP_MAJOR_VERSION >= 2
    proxy_ = sdbus::createProxy(sdbus::createSessionBusConnection(), sdbus::ServiceName{eqcore::dbus::kServiceName},
                                 sdbus::ObjectPath{eqcore::dbus::kObjectPath});
#else
    proxy_ = sdbus::createProxy(sdbus::createSessionBusConnection(), std::string(eqcore::dbus::kServiceName),
                                 std::string(eqcore::dbus::kObjectPath));
#endif

    proxy_->uponSignal(eqcore::dbus::kSignalRouteChanged)
        .onInterface(eqcore::dbus::kInterfaceName)
        .call([this](const std::string& routeId) { emit routeChanged(QString::fromStdString(routeId)); });

    proxy_->uponSignal(eqcore::dbus::kSignalInputsChanged)
        .onInterface(eqcore::dbus::kInterfaceName)
        .call([this] { emit inputsChanged(); });
}

DbusClient::~DbusClient() = default;

std::vector<DeviceRow> DbusClient::listDevices() {
    std::vector<DeviceRow> result;
    try {
        std::vector<DeviceStructRow> rows;
        proxy_->callMethod(eqcore::dbus::kMethodListDevices)
            .onInterface(eqcore::dbus::kInterfaceName)
            .storeResultsTo(rows);
        for (const auto& r : rows) {
            result.push_back(DeviceRow{std::get<0>(r), QString::fromStdString(std::get<1>(r)),
                                        QString::fromStdString(std::get<2>(r))});
        }
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: ListDevices failed: %s", e.what());
    }
    return result;
}

std::vector<RouteRow> DbusClient::listRoutes() {
    std::vector<RouteRow> result;
    try {
        std::vector<RouteStructRow> rows;
        proxy_->callMethod(eqcore::dbus::kMethodListRoutes)
            .onInterface(eqcore::dbus::kInterfaceName)
            .storeResultsTo(rows);
        for (const auto& r : rows) {
            RouteRow row;
            row.id = QString::fromStdString(std::get<0>(r));
            row.deviceName = QString::fromStdString(std::get<1>(r));
            row.displayName = QString::fromStdString(std::get<2>(r));
            row.gainDb = std::get<3>(r);
            row.muted = std::get<4>(r);
            row.bandCount = std::get<5>(r);
            result.push_back(std::move(row));
        }
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: ListRoutes failed: %s", e.what());
    }
    return result;
}

std::vector<eqcore::EqBand> DbusClient::getRouteBands(const QString& routeId) {
    std::vector<eqcore::EqBand> result;
    try {
        std::vector<BandStructRow> rows;
        proxy_->callMethod(eqcore::dbus::kMethodGetRouteBands)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(routeId.toStdString())
            .storeResultsTo(rows);
        for (const auto& r : rows) {
            eqcore::EqBand band;
            band.type = nlohmann::json(std::get<0>(r)).get<eqcore::FilterType>();
            band.freqHz = std::get<1>(r);
            band.gainDb = std::get<2>(r);
            band.q = std::get<3>(r);
            result.push_back(band);
        }
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: GetRouteBands failed: %s", e.what());
    }
    return result;
}

QString DbusClient::addRoute(const QString& deviceName, const QString& displayName) {
    std::string routeId;
    try {
        proxy_->callMethod(eqcore::dbus::kMethodAddRoute)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(deviceName.toStdString(), displayName.toStdString())
            .storeResultsTo(routeId);
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: AddRoute failed: %s", e.what());
    }
    return QString::fromStdString(routeId);
}

void DbusClient::removeRoute(const QString& routeId) {
    try {
        proxy_->callMethod(eqcore::dbus::kMethodRemoveRoute)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(routeId.toStdString());
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: RemoveRoute failed: %s", e.what());
    }
}

bool DbusClient::setRouteGain(const QString& routeId, double gainDb) {
    bool ok = false;
    try {
        proxy_->callMethod(eqcore::dbus::kMethodSetRouteGain)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(routeId.toStdString(), gainDb)
            .storeResultsTo(ok);
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetRouteGain failed: %s", e.what());
    }
    return ok;
}

bool DbusClient::setRouteMute(const QString& routeId, bool muted) {
    bool ok = false;
    try {
        proxy_->callMethod(eqcore::dbus::kMethodSetRouteMute)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(routeId.toStdString(), muted)
            .storeResultsTo(ok);
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetRouteMute failed: %s", e.what());
    }
    return ok;
}

bool DbusClient::setRouteBandCount(const QString& routeId, uint32_t count) {
    bool ok = false;
    try {
        proxy_->callMethod(eqcore::dbus::kMethodSetRouteBandCount)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(routeId.toStdString(), count)
            .storeResultsTo(ok);
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetRouteBandCount failed: %s", e.what());
    }
    return ok;
}

bool DbusClient::setRouteBand(const QString& routeId, uint32_t index, const QString& type, double freqHz,
                               double gainDb, double q) {
    bool ok = false;
    try {
        proxy_->callMethod(eqcore::dbus::kMethodSetRouteBand)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(routeId.toStdString(), index, type.toStdString(), freqHz, gainDb, q)
            .storeResultsTo(ok);
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetRouteBand failed: %s", e.what());
    }
    return ok;
}

std::vector<InputRow> DbusClient::listInputs() {
    std::vector<InputRow> result;
    try {
        std::vector<InputStructRow> rows;
        proxy_->callMethod(eqcore::dbus::kMethodListInputs)
            .onInterface(eqcore::dbus::kInterfaceName)
            .storeResultsTo(rows);
        for (const auto& r : rows) {
            result.push_back(InputRow{QString::fromStdString(std::get<0>(r)), QString::fromStdString(std::get<1>(r))});
        }
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: ListInputs failed: %s", e.what());
    }
    return result;
}

QString DbusClient::addInput(const QString& displayName) {
    std::string inputId;
    try {
        proxy_->callMethod(eqcore::dbus::kMethodAddInput)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(displayName.toStdString())
            .storeResultsTo(inputId);
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: AddInput failed: %s", e.what());
    }
    return QString::fromStdString(inputId);
}

void DbusClient::removeInput(const QString& inputId) {
    try {
        proxy_->callMethod(eqcore::dbus::kMethodRemoveInput)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(inputId.toStdString());
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: RemoveInput failed: %s", e.what());
    }
}

bool DbusClient::setRouteInputGain(const QString& routeId, const QString& inputId, double gainDb) {
    bool ok = false;
    try {
        proxy_->callMethod(eqcore::dbus::kMethodSetRouteInputGain)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(routeId.toStdString(), inputId.toStdString(), gainDb)
            .storeResultsTo(ok);
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetRouteInputGain failed: %s", e.what());
    }
    return ok;
}

bool DbusClient::removeRouteInput(const QString& routeId, const QString& inputId) {
    bool ok = false;
    try {
        proxy_->callMethod(eqcore::dbus::kMethodRemoveRouteInput)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(routeId.toStdString(), inputId.toStdString())
            .storeResultsTo(ok);
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: RemoveRouteInput failed: %s", e.what());
    }
    return ok;
}

std::vector<std::pair<QString, double>> DbusClient::getRouteInputGains(const QString& routeId) {
    std::vector<std::pair<QString, double>> result;
    try {
        std::vector<InputGainStructRow> rows;
        proxy_->callMethod(eqcore::dbus::kMethodGetRouteInputGains)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(routeId.toStdString())
            .storeResultsTo(rows);
        for (const auto& r : rows) {
            result.emplace_back(QString::fromStdString(std::get<0>(r)), std::get<1>(r));
        }
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: GetRouteInputGains failed: %s", e.what());
    }
    return result;
}

} // namespace pipeeq
