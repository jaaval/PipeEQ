#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <QObject>
#include <QString>

#include <sdbus-c++/sdbus-c++.h>

#include "eq_band.h"

namespace pipeeq {

// One selectable stereo pair of one device. A 4-channel interface yields two
// of these for the same nodeName, so its output pairs can be driven as
// separate PipeEQ outputs with independent EQ.
struct DeviceRow {
    uint32_t id = 0;
    QString nodeName;
    QString description;
    QString pairLabel;    // e.g. "Rear L/R (ch 3-4)"
    QString leftChannel;  // SPA short name, empty = device default
    QString rightChannel;
};

struct RouteRow {
    QString id;
    QString deviceName;
    QString displayName;
    double gainDb = 0.0;
    bool muted = false;
    uint32_t bandCount = 0;
    // False while the output's device isn't present. Such a route is still
    // fully editable - the daemon keeps the settings and applies them when
    // the device turns up.
    bool connected = false;
    bool autoConnect = true;
    QString leftChannel;
    QString rightChannel;
};

struct InputRow {
    QString id;
    QString displayName;
};

// Thin wrapper around an sdbus-c++ proxy to org.pipeeq.Daemon1. Method calls
// are synchronous (fine for a control GUI - they're infrequent and local).
// Signals received on the proxy's own background thread are re-emitted as Qt
// signals, which Qt automatically delivers to the GUI thread via a queued
// connection since this object lives on it.
class DbusClient : public QObject {
    Q_OBJECT

public:
    explicit DbusClient(QObject* parent = nullptr);
    ~DbusClient() override;

    std::vector<DeviceRow> listDevices();
    std::vector<RouteRow> listRoutes();
    std::vector<eqcore::EqBand> getRouteBands(const QString& routeId);
    QString addRoute(const QString& deviceName, const QString& displayName, const QString& leftChannel,
                      const QString& rightChannel);
    void removeRoute(const QString& routeId);
    bool setRouteGain(const QString& routeId, double gainDb);
    bool setRouteMute(const QString& routeId, bool muted);
    bool setRouteBandCount(const QString& routeId, uint32_t count);
    bool setRouteBand(const QString& routeId, uint32_t index, const QString& type, double freqHz,
                       double gainDb, double q);
    bool setRouteAutoConnect(const QString& routeId, bool autoConnect);
    bool setRouteChannels(const QString& routeId, const QString& leftChannel, const QString& rightChannel);

    std::vector<InputRow> listInputs();
    QString addInput(const QString& displayName);
    void removeInput(const QString& inputId);
    bool setRouteInputGain(const QString& routeId, const QString& inputId, double gainDb);
    bool removeRouteInput(const QString& routeId, const QString& inputId);
    // Only entries for currently-active (subscribed) inputs are returned;
    // an input this route doesn't hear at all simply won't appear here.
    std::vector<std::pair<QString, double>> getRouteInputGains(const QString& routeId);

signals:
    void routeChanged(const QString& routeId);
    void inputsChanged();

private:
    std::unique_ptr<sdbus::IProxy> proxy_;
};

} // namespace pipeeq
