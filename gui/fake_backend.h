#pragma once

#include <map>
#include <vector>

#include <QTimer>

#include "backend.h"

namespace pipeeq {

// A Backend implementation with no daemon, no session bus and no PipeWire
// behind it.
//
// This exists so the UI can be built, demoed and screenshotted against a fixed,
// deliberately interesting topology: a 6-channel card, a 4-channel interface,
// and one device that is absent so the "waiting" presentation is always
// exercised. Screenshots are then comparable across runs and don't depend on
// whatever hardware happens to be plugged into the machine.
//
// It also generates plausible moving levels, which is what lets the metering UI
// be built before the daemon side of it is wired up.
class FakeBackend : public Backend {
    Q_OBJECT

public:
    explicit FakeBackend(QObject* parent = nullptr);

    bool isAvailable() const override { return true; }

    std::vector<DeviceRow> listDevices() override;
    std::vector<StripRow> listStrips() override;
    std::vector<InputRow> listInputs() override;

    QString addOutput(const QString& deviceName, const QString& displayName) override;
    void removeOutput(const QString& outputId) override;
    bool setOutputAutoConnect(const QString& outputId, bool autoConnect) override;

    bool setChannelGain(const QString& outputId, uint32_t channelIndex, double gainDb) override;
    bool setChannelMuted(const QString& outputId, uint32_t channelIndex, bool muted) override;
    bool setChannelPosition(const QString& outputId, uint32_t channelIndex,
                             const QString& position) override;

    std::vector<eqcore::EqBand> getChannelEqBands(const QString& outputId,
                                                   uint32_t channelIndex) override;
    bool setChannelEqBandCount(const QString& outputId, uint32_t channelIndex,
                                uint32_t count) override;
    bool setChannelEqBand(const QString& outputId, uint32_t channelIndex, uint32_t index,
                           const QString& type, double freqHz, double gainDb, double q) override;

    QString addInput(const QString& displayName) override;
    void removeInput(const QString& inputId) override;

    bool setSend(const QString& outputId, uint32_t channelIndex, const QString& inputId,
                  double gainDb) override;
    bool removeSend(const QString& outputId, uint32_t channelIndex,
                     const QString& inputId) override;
    QVector<SendEntry> getOutputSends(const QString& outputId) override;
    int maxSendsPerOutput() const override { return 8; }

    void setMeteringEnabled(bool enabled) override;

private:
    struct Channel {
        QString position;
        QString displayName;
        double gainDb = 0.0;
        bool muted = false;
        QString groupId;
        std::vector<eqcore::EqBand> bands;
        std::map<QString, double> sendsDb;
    };
    struct Output {
        QString id;
        QString deviceName;
        QString displayName;
        bool connected = true;
        bool autoConnect = true;
        std::vector<Channel> channels;
    };

    Output* findOutput(const QString& outputId);
    Channel* findChannel(const QString& outputId, uint32_t channelIndex);
    // Mirrors the daemon: gain, mute and sends are linked across a group.
    std::vector<Channel*> linkedChannels(const QString& outputId, uint32_t channelIndex);
    void emitMeters();

    std::vector<DeviceRow> devices_;
    std::vector<Output> outputs_;
    std::vector<InputRow> inputs_;
    int nextOutputIndex_ = 1;
    int nextInputIndex_ = 1;

    QTimer meterTimer_;
    double meterPhase_ = 0.0;
};

} // namespace pipeeq
