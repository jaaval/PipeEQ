#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVector>

#include "model_types.h"

namespace pipeeq {

// Turns a stream of user input into a bounded stream of daemon writes.
//
// Without this, every QSlider::valueChanged and every curve mouse-move became
// one immediate blocking D-Bus round trip into a daemon that (before the
// debounce landed) also rewrote the whole config file each time. A single EQ
// handle drag could be hundreds of writes and hundreds of GUI-thread stalls.
//
// Two structures, because the two kinds of write have different requirements:
//
//   - `latest_`: latest-value-wins, for continuous parameters. Dragging a fader
//     through 200 intermediate values only needs the newest one.
//   - `ordered_`: FIFO, for discrete and structural operations. "Set band count
//     to 3" followed by "set band 2" is not interchangeable with the reverse,
//     so these must not be collapsed or reordered.
//
// Ordered ops flush before coalesced ones for the same reason.
class WriteCoalescer : public QObject {
    Q_OBJECT

public:
    explicit WriteCoalescer(QObject* parent = nullptr);

    // Adds a write. Continuous params supersede any pending write with the same
    // identity; everything else is queued in order.
    void enqueue(const WriteOp& op);

    // Sends whatever is pending right now. Called on control release, so the
    // final value of a drag lands with no perceptible lag rather than waiting
    // out the timer.
    void flushNow();

    bool hasPending() const { return !latest_.isEmpty() || !ordered_.isEmpty(); }

    // 40 ms, i.e. at most 25 writes a second per control. Fast enough that a
    // drag feels live, slow enough that the daemon isn't the bottleneck. Coarse
    // because a few ms of jitter is irrelevant here and coarse timers coalesce
    // with other wakeups.
    static constexpr int kFlushIntervalMs = 40;

signals:
    void writesReady(const QVector<WriteOp>& ops);

private:
    void onTimeout();

    QHash<QString, WriteOp> latest_;
    QVector<QString> latestOrder_; // stable emission order, for reproducibility
    QVector<WriteOp> ordered_;
    QTimer timer_;
};

} // namespace pipeeq
