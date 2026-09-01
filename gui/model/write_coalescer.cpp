#include "write_coalescer.h"

namespace pipeeq {

WriteCoalescer::WriteCoalescer(QObject* parent) : QObject(parent) {
    timer_.setInterval(kFlushIntervalMs);
    timer_.setTimerType(Qt::CoarseTimer);
    timer_.setSingleShot(false);
    connect(&timer_, &QTimer::timeout, this, &WriteCoalescer::onTimeout);
}

void WriteCoalescer::enqueue(const WriteOp& op) {
    if (op.coalescable()) {
        const QString key = op.coalesceKey();
        if (!latest_.contains(key)) {
            latestOrder_.push_back(key);
        }
        latest_.insert(key, op);
    } else {
        ordered_.push_back(op);
    }

    // Started lazily and stopped when everything drains, so an idle GUI has no
    // timer running at all.
    if (!timer_.isActive()) {
        timer_.start();
    }
}

void WriteCoalescer::flushNow() {
    onTimeout();
}

void WriteCoalescer::onTimeout() {
    if (!hasPending()) {
        timer_.stop();
        return;
    }

    QVector<WriteOp> batch;
    batch.reserve(ordered_.size() + latest_.size());

    // Structural first: a band-count change has to reach the daemon before the
    // band values that depend on it.
    batch.append(ordered_);
    ordered_.clear();

    for (const QString& key : latestOrder_) {
        const auto it = latest_.constFind(key);
        if (it != latest_.constEnd()) {
            batch.push_back(*it);
        }
    }
    latest_.clear();
    latestOrder_.clear();

    if (!batch.isEmpty()) {
        emit writesReady(batch);
    }
    if (!hasPending()) {
        timer_.stop();
    }
}

} // namespace pipeeq
