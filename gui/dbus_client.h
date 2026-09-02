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

// Thin wrapper around an sdbus-c++ proxy to org.pipeeq.Daemon1.
//
// Method calls are synchronous, which is fine because this object lives on
// BackendWorker's thread rather than the GUI thread - blocking here never
// stalls a repaint. The proxy is constructed there for the same reason it must
// be: an sdbus proxy is bound to its creating thread.
//
// Signals arrive on the proxy's own background thread and are re-emitted as Qt
// signals; the store's connections to them are queued, which is what carries
// them safely to the GUI thread.
class DbusClient : public Backend {
    Q_OBJECT

public:
    explicit DbusClient(QObject* parent = nullptr);
    ~DbusClient() override;

    // Reflects whether a call has actually SUCCEEDED, not merely whether a
    // proxy could be constructed. sdbus::createProxy performs no round trip and
    // does not check that anyone owns the destination name, so the old
    // `proxy_ != nullptr` was true with no daemon running at all - and the UI
    // then showed "No outputs configured" instead of "not connected".
    bool isAvailable() const override { return available_; }

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

    // Set by any successful call, cleared by any failure. Probed once at
    // construction so the first paint is already correct.
    void probeAvailability();

    std::unique_ptr<sdbus::IProxy> proxy_;
    bool available_ = false;
};

} // namespace pipeeq
