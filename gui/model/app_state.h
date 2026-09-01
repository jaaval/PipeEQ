#pragma once

#include <memory>

#include <QHash>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QVector>

#include "backend_worker.h"
#include "edit_guard.h"
#include "level_meters.h"
#include "model_types.h"
#include "write_coalescer.h"

namespace pipeeq {

// The single source of truth the UI reads from.
//
// Replaces MainWindow-as-store, where every widget handler called D-Bus
// directly and the window itself held the vectors. That does not survive the
// mixer UI: with N strips and their sends all on screen at once, per-widget
// synchronous calls are both a latency problem and a correctness one.
//
// Reads are served from a cache that a background thread refreshes, so nothing
// the UI asks for can block. Writes go out through a coalescer, guarded so a
// value coming back from the daemon can't overwrite a control the user is
// actively holding.
class AppState : public QObject {
    Q_OBJECT

public:
    explicit AppState(bool demo, QObject* parent = nullptr);
    ~AppState() override;

    // ---- cached reads, all instant ----
    bool isAvailable() const { return snapshot_.available; }
    const QVector<DeviceRow>& devices() const { return snapshot_.devices; }
    const QVector<StripRow>& strips() const { return snapshot_.strips; }
    const QVector<InputRow>& inputs() const { return snapshot_.inputs; }
    const StripRow* findStrip(const QString& stripId) const;
    const DeviceRow* findDevice(const QString& nodeName) const;

    // Cached per-channel detail. Empty until the first detail fetch for that
    // channel completes; requestChannelDetail() asks for it.
    QVector<eqcore::EqBand> channelBands(const QString& outputId, uint32_t channelIndex) const;
    QVector<QPair<QString, double>> channelSends(const QString& outputId,
                                                  uint32_t channelIndex) const;
    void requestChannelDetail(const QString& outputId, uint32_t channelIndex);

    LevelMeters& meters() { return meters_; }

    // ---- edit lifecycle ----
    // Bracket a drag or a keyboard nudge with these. Between them, and for a
    // short grace period after, daemon values for that field are held back
    // instead of yanking the control out from under the user.
    void beginEdit(const EditKey& key);
    void endEdit(const EditKey& key);

    // ---- writes ----
    // Continuous parameters. These are coalesced, so calling one per mouse-move
    // event is fine and expected.
    void setChannelGain(const QString& outputId, uint32_t channelIndex, double gainDb);
    void setSend(const QString& outputId, uint32_t channelIndex, const QString& inputId,
                  double gainDb);
    void setChannelEqBand(const QString& outputId, uint32_t channelIndex, uint32_t bandIndex,
                           const eqcore::EqBand& band);

    // Discrete parameters. Queued in order and flushed immediately - latency
    // matters more than rate for a one-shot toggle.
    void setChannelMuted(const QString& outputId, uint32_t channelIndex, bool muted);
    void setChannelPosition(const QString& outputId, uint32_t channelIndex, const QString& position);
    void setOutputAutoConnect(const QString& outputId, bool autoConnect);
    void setChannelEqBandCount(const QString& outputId, uint32_t channelIndex, uint32_t count);
    void removeSend(const QString& outputId, uint32_t channelIndex, const QString& inputId);

    // ---- topology ----
    void addOutput(const QString& deviceName, const QString& displayName);
    void removeOutput(const QString& outputId);
    void addInput(const QString& displayName);
    void removeInput(const QString& inputId);

    void setMeteringEnabled(bool enabled);
    void refresh();

signals:
    // The device/strip/input SET changed: racks and lists must rebuild.
    void topologyChanged();
    // Values changed on strips that already existed. Separate from
    // topologyChanged so a refresh doesn't destroy widgets under the cursor.
    void stripsUpdated();
    void channelDetailUpdated(const QString& outputId, uint32_t channelIndex);
    void availabilityChanged(bool available);
    void errorReported(const QString& message);

private:
    QString detailKey(const QString& outputId, uint32_t channelIndex) const;
    void enqueue(const WriteOp& op, const EditKey& key, bool flushNow);
    void onSnapshotReady(const DaemonSnapshot& snapshot);
    void onWritesCompleted(quint64 seq, bool ok, const QString& error);

    // A slow safety resync. The daemon does emit change signals now, but one
    // dropped signal should not leave the UI permanently wrong - and unlike the
    // 3 s blocking poll this replaces, this one costs the GUI thread nothing.
    static constexpr int kResyncIntervalMs = 8000;
    static constexpr int kMeterRearmIntervalMs = 1000;

    struct ChannelDetail {
        QVector<eqcore::EqBand> bands;
        QVector<QPair<QString, double>> sends;
    };

    QThread workerThread_;
    BackendWorker* worker_ = nullptr;

    DaemonSnapshot snapshot_;
    QHash<QString, ChannelDetail> details_;

    EditGuard guard_;
    WriteCoalescer coalescer_;
    LevelMeters meters_;
    QTimer resyncTimer_;
    QTimer meterRearmTimer_;

    // Maps a batch back to the keys it wrote, so completion can clear exactly
    // those pending counts.
    QHash<quint64, QVector<EditKey>> inFlight_;
    quint64 nextSeq_ = 1;
    bool meteringWanted_ = false;
};

} // namespace pipeeq
