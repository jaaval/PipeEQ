#pragma once

#include <memory>

#include <QObject>
#include <QVector>

#include "backend.h"
#include "model/model_types.h"

namespace pipeeq {

// Runs every backend call on a dedicated thread.
//
// This is not only about the meter rate. The daemon can block inside
// pw_thread_loop_lock() or inside its config write for tens of milliseconds,
// and previously that stall happened *inside a mouseMoveEvent handler* - so the
// UI froze in proportion to daemon latency, and killing the daemon mid-drag
// wedged the window for sdbus's 25-second default timeout.
//
// The Backend is created ON this thread rather than handed in, because the sdbus
// proxy is bound to the thread that constructs it.
class BackendWorker : public QObject {
    Q_OBJECT

public:
    explicit BackendWorker(bool demo, QObject* parent = nullptr);
    ~BackendWorker() override;

public slots:
    // Must be the first slot invoked on the worker thread.
    void initialize();
    // One round trip for everything, rather than the 1+N reads the window used
    // to issue per selection change.
    void requestSnapshot();
    void requestChannelDetail(const QString& outputId, uint32_t channelIndex);
    void requestOutputSends(const QString& outputId);
    void applyWrites(const QVector<WriteOp>& ops);
    void setMeteringEnabled(bool enabled);

    // Structural operations. Their return values aren't used by the UI - the
    // resulting snapshot is what it reacts to - so they don't go through the
    // WriteOp machinery, which exists for coalescing continuous changes.
    void addOutput(const QString& deviceName, const QString& displayName);
    void removeOutput(const QString& outputId);
    void addInput(const QString& displayName);
    void removeInput(const QString& inputId);

signals:
    void snapshotReady(const DaemonSnapshot& snapshot);
    void channelDetailReady(const QString& outputId, uint32_t channelIndex,
                             const QVector<eqcore::EqBand>& bands);
    void outputSendsReady(const QString& outputId, const QVector<SendEntry>& sends);
    // seq identifies the batch; ok is false if any write in it was refused, in
    // which case the caller resyncs rather than trusting its optimistic value.
    void writesCompleted(quint64 seq, bool ok, const QString& error);
    void availabilityChanged(bool available);

    // Forwarded from the backend, so the store only ever listens to this
    // object.
    void daemonOutputChanged(const QString& outputId);
    void daemonOutputsChanged();
    void daemonInputsChanged();
    void daemonDevicesChanged();
    void metersReceived(const QVector<MeterRow>& outputs, const QVector<MeterRow>& inputs);

private:
public:
    int maxSendsPerOutput() const { return backend_ ? backend_->maxSendsPerOutput() : 8; }

private:
    bool demo_;
    std::unique_ptr<Backend> backend_;
};

} // namespace pipeeq
