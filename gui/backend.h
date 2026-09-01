#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <QObject>
#include <QString>
#include <QVector>

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
// `id` is a composite "<outputId>#<channelIndex>" so selection and list
// diffing can key off a single string; use outputId/channelIndex for anything
// that talks to the daemon.
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

// Per-channel peak levels for one output, in dB.
struct MeterRow {
    QString id;
    QVector<double> peaksDb;
};

// What the UI talks to.
//
// Abstract so the whole interface can be driven by a fake: the real backend
// needs a running daemon, a session bus and PipeWire, which makes it useless
// for building a screenshot-checkable UI or for running anywhere headless.
// FakeBackend implements this against fixed data and synthetic levels, which is
// what --demo selects.
class Backend : public QObject {
    Q_OBJECT

public:
    explicit Backend(QObject* parent = nullptr);
    ~Backend() override;

    // False when there is no daemon to talk to. Distinguishing this from
    // "connected, but no outputs configured" matters: the two used to be
    // indistinguishable, so a daemon that wasn't running looked like a working
    // one with nothing set up.
    virtual bool isAvailable() const = 0;

    virtual std::vector<DeviceRow> listDevices() = 0;
    virtual std::vector<StripRow> listStrips() = 0;
    virtual std::vector<InputRow> listInputs() = 0;

    virtual QString addOutput(const QString& deviceName, const QString& displayName) = 0;
    virtual void removeOutput(const QString& outputId) = 0;
    virtual bool setOutputAutoConnect(const QString& outputId, bool autoConnect) = 0;

    virtual bool setChannelGain(const QString& outputId, uint32_t channelIndex, double gainDb) = 0;
    virtual bool setChannelMuted(const QString& outputId, uint32_t channelIndex, bool muted) = 0;
    virtual bool setChannelPosition(const QString& outputId, uint32_t channelIndex,
                                     const QString& position) = 0;

    virtual std::vector<eqcore::EqBand> getChannelEqBands(const QString& outputId,
                                                           uint32_t channelIndex) = 0;
    virtual bool setChannelEqBandCount(const QString& outputId, uint32_t channelIndex,
                                        uint32_t count) = 0;
    virtual bool setChannelEqBand(const QString& outputId, uint32_t channelIndex, uint32_t index,
                                   const QString& type, double freqHz, double gainDb, double q) = 0;

    virtual QString addInput(const QString& displayName) = 0;
    virtual void removeInput(const QString& inputId) = 0;

    virtual bool setSend(const QString& outputId, uint32_t channelIndex, const QString& inputId,
                          double gainDb) = 0;
    virtual bool removeSend(const QString& outputId, uint32_t channelIndex,
                             const QString& inputId) = 0;
    // Only inputs actually routed to this channel appear; one this channel
    // doesn't hear at all is simply absent, which is distinct from 0 dB.
    virtual std::vector<std::pair<QString, double>> getChannelSends(const QString& outputId,
                                                                     uint32_t channelIndex) = 0;

    // Arms or disarms level reporting. The daemon's lease expires on its own,
    // so a watcher re-arms periodically rather than relying on a clean
    // shutdown.
    virtual void setMeteringEnabled(bool enabled) = 0;

signals:
    void outputChanged(const QString& outputId);
    void outputsChanged();
    void inputsChanged();
    void devicesChanged();
    void availabilityChanged(bool available);
    void metersReceived(const QVector<MeterRow>& outputs, const QVector<MeterRow>& inputs);
};

} // namespace pipeeq
