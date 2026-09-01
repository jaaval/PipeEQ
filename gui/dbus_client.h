#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <QObject>
#include <QVector>
#include <QString>

#include <sdbus-c++/sdbus-c++.h>

#include "eq_band.h"

namespace pipeeq {

// One output device, with the channel layout it advertises.
struct DeviceRow {
    uint32_t id = 0;
    QString nodeName;
    QString description;
    // SPA channel short names in device channel order, e.g. {FL, FR, RL, RR}.
    QVector<QString> positions;
    bool inUse = false; // already claimed by a PipeEQ output
};

// One mono hardware output CHANNEL - the strip.
//
// This transitional client keeps the old one-row-per-editable-thing shape the
// current window is built around, but a row is now a channel rather than a
// stereo-pair output. `id` is a composite "<outputId>#<channelIndex>" so the
// existing selection and list-diffing logic keeps working unchanged; use
// outputId/channelIndex for anything that talks to the daemon.
struct StripRow {
    QString id;
    QString outputId;
    uint32_t channelIndex = 0;
    QString deviceName;
    QString outputName;
    QString position;
    QString channelName;
    double gainDb = 0.0;
    bool muted = false;
    uint32_t bandCount = 0;
    // False while the output's device isn't present, or while its device
    // doesn't currently offer this channel. Such a strip is still fully
    // editable - the daemon keeps the settings and applies them when the
    // device turns up.
    bool connected = false;
    bool driven = false;
    bool autoConnect = true;
    QString groupId;

    // "Speakers - FL", the label the window shows.
    QString label() const {
        const QString name = channelName.isEmpty() ? position : channelName;
        return name.isEmpty() ? outputName : (outputName + " - " + name);
    }
};

struct InputRow {
    QString id;
    QString displayName;
    QVector<QString> positions;
};

// Thin wrapper around an sdbus-c++ proxy to org.pipeeq.Daemon1. Method calls
// are synchronous (fine for a control GUI - they're infrequent and local; the
// planned rework moves them off the GUI thread). Signals received on the
// proxy's own background thread are re-emitted as Qt signals, which Qt
// automatically delivers to the GUI thread via a queued connection since this
// object lives on it.
class DbusClient : public QObject {
    Q_OBJECT

public:
    explicit DbusClient(QObject* parent = nullptr);
    ~DbusClient() override;

    std::vector<DeviceRow> listDevices();
    // One row per channel of every configured output.
    std::vector<StripRow> listStrips();

    QString addOutput(const QString& deviceName, const QString& displayName);
    void removeOutput(const QString& outputId);
    bool setOutputAutoConnect(const QString& outputId, bool autoConnect);

    bool setChannelGain(const QString& outputId, uint32_t channelIndex, double gainDb);
    bool setChannelMuted(const QString& outputId, uint32_t channelIndex, bool muted);
    bool setChannelPosition(const QString& outputId, uint32_t channelIndex, const QString& position);

    // The channel's EQ, via the daemon's channel-scoped convenience methods -
    // so this client needs to know nothing about EQ instances. Setting a band
    // count creates an instance for the channel on demand.
    std::vector<eqcore::EqBand> getChannelEqBands(const QString& outputId, uint32_t channelIndex);
    bool setChannelEqBandCount(const QString& outputId, uint32_t channelIndex, uint32_t count);
    bool setChannelEqBand(const QString& outputId, uint32_t channelIndex, uint32_t index,
                           const QString& type, double freqHz, double gainDb, double q);

    std::vector<InputRow> listInputs();
    QString addInput(const QString& displayName);
    void removeInput(const QString& inputId);

    bool setSend(const QString& outputId, uint32_t channelIndex, const QString& inputId, double gainDb);
    bool removeSend(const QString& outputId, uint32_t channelIndex, const QString& inputId);
    // Only inputs actually routed to this channel appear; one this channel
    // doesn't hear at all is simply absent, which is distinct from 0 dB.
    std::vector<std::pair<QString, double>> getChannelSends(const QString& outputId,
                                                             uint32_t channelIndex);

signals:
    void outputChanged(const QString& outputId);
    void outputsChanged();
    void inputsChanged();
    void devicesChanged();

private:
    std::unique_ptr<sdbus::IProxy> proxy_;
};

} // namespace pipeeq
