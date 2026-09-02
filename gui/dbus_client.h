#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <utility>
#include <vector>

#include <QObject>
#include <QString>

#include <sdbus-c++/sdbus-c++.h>

#include "backend.h"

namespace pipeeq {

// Thin wrapper around an sdbus-c++ proxy to org.pipeeq.Daemon1. Method calls
// are synchronous (fine for a control GUI - they're infrequent and local; the
// planned rework moves them off the GUI thread). Signals received on the
// proxy's own background thread are re-emitted as Qt signals, which Qt
// automatically delivers to the GUI thread via a queued connection since this
// object lives on it.
class DbusClient : public Backend {
    Q_OBJECT

public:
    explicit DbusClient(QObject* parent = nullptr);
    ~DbusClient() override;

    bool isAvailable() const override { return proxy_ != nullptr; }

    std::vector<DeviceRow> listDevices() override;
    // One row per channel of every configured output.
    std::vector<StripRow> listStrips() override;
    std::vector<InputRow> listInputs() override;

    QString addOutput(const QString& deviceName, const QString& displayName) override;
    void removeOutput(const QString& outputId) override;
    bool setOutputAutoConnect(const QString& outputId, bool autoConnect) override;

    bool setChannelGain(const QString& outputId, uint32_t channelIndex, double gainDb) override;
    bool setChannelMuted(const QString& outputId, uint32_t channelIndex, bool muted) override;
    bool setChannelPosition(const QString& outputId, uint32_t channelIndex,
                             const QString& position) override;

    // The channel's EQ, via the daemon's channel-scoped convenience methods -
    // so this client needs to know nothing about EQ instances. Setting a band
    // count creates an instance for the channel on demand.
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
    int maxSendsPerOutput() const override { return kMaxSendsPerOutput; }

    bool setOutputDisplayName(const QString& outputId, const QString& displayName) override;
    bool setChannelDisplayName(const QString& outputId, uint32_t channelIndex,
                                const QString& displayName) override;
    bool setInputDisplayName(const QString& inputId, const QString& displayName) override;

    QString createLinkGroup(const QString& outputId, const QVector<uint32_t>& channels,
                             const QString& displayName) override;
    bool removeLinkGroup(const QString& outputId, const QString& groupId) override;
    bool setLinkGroupChannels(const QString& outputId, const QString& groupId,
                               const QVector<uint32_t>& channels) override;

    void setMeteringEnabled(bool enabled) override;

private:
    bool callSetter(const char* method, const std::string& a, const std::string& b);

    // Mirrors the daemon's kMaxInputs. Not discoverable over the bus today; if
    // that limit ever becomes configurable it needs to be reported instead of
    // duplicated here.
    static constexpr int kMaxSendsPerOutput = 8;

    std::unique_ptr<sdbus::IProxy> proxy_;
};

} // namespace pipeeq
