#include "level_meters.h"

#include <algorithm>

namespace pipeeq {

LevelMeters::LevelMeters(QObject* parent) : QObject(parent) {
    timer_.setInterval(kTickMs);
    timer_.setTimerType(Qt::CoarseTimer);
    connect(&timer_, &QTimer::timeout, this, &LevelMeters::tick);
}

QString LevelMeters::cellKey(const QString& ownerId, int channelIndex) {
    return ownerId + "#" + QString::number(channelIndex);
}

void LevelMeters::ingestRows(const QVector<MeterRow>& rows) {
    for (const MeterRow& row : rows) {
        for (int i = 0; i < row.peaksDb.size(); ++i) {
            Cell& cell = cells_[cellKey(row.id, i)];
            // Peak-combine rather than overwrite: two frames may arrive between
            // repaints, and the louder one is the one that matters.
            cell.pendingPeakDb = std::max(cell.pendingPeakDb, row.peaksDb[i]);
        }
    }
}

void LevelMeters::ingest(const QVector<MeterRow>& outputs, const QVector<MeterRow>& inputs) {
    ingestRows(outputs);
    ingestRows(inputs);
}

void LevelMeters::tick() {
    elapsedMs_ += kTickMs;
    const double releaseDb = kReleaseDbPerSecond * (static_cast<double>(kTickMs) / 1000.0);

    for (auto it = cells_.begin(); it != cells_.end(); ++it) {
        Cell& cell = *it;

        if (cell.pendingPeakDb > cell.levelDb) {
            cell.levelDb = cell.pendingPeakDb; // instant attack
        } else {
            cell.levelDb = std::max(kSilenceDb, cell.levelDb - releaseDb);
        }

        if (cell.pendingPeakDb >= cell.holdDb) {
            cell.holdDb = cell.pendingPeakDb;
            cell.holdSetAtMs = elapsedMs_;
        } else if (elapsedMs_ - cell.holdSetAtMs > kHoldMs) {
            cell.holdDb = cell.levelDb;
        }

        if (cell.pendingPeakDb >= kClipThresholdDb) {
            cell.clipped = true;
        }

        cell.pendingPeakDb = kSilenceDb;
    }

    emit levelsUpdated();
}

double LevelMeters::levelDb(const QString& ownerId, int channelIndex) const {
    const auto it = cells_.constFind(cellKey(ownerId, channelIndex));
    return it == cells_.constEnd() ? kSilenceDb : it->levelDb;
}

double LevelMeters::holdDb(const QString& ownerId, int channelIndex) const {
    const auto it = cells_.constFind(cellKey(ownerId, channelIndex));
    return it == cells_.constEnd() ? kSilenceDb : it->holdDb;
}

bool LevelMeters::clipped(const QString& ownerId, int channelIndex) const {
    const auto it = cells_.constFind(cellKey(ownerId, channelIndex));
    return it != cells_.constEnd() && it->clipped;
}

void LevelMeters::clearClip(const QString& ownerId, int channelIndex) {
    const auto it = cells_.find(cellKey(ownerId, channelIndex));
    if (it != cells_.end()) {
        it->clipped = false;
    }
}

void LevelMeters::clearAll() {
    cells_.clear();
}

void LevelMeters::setActive(bool active) {
    if (active == timer_.isActive()) {
        return;
    }
    if (active) {
        timer_.start();
    } else {
        timer_.stop();
        // Leave the cells at silence rather than at whatever they held, so
        // re-arming doesn't briefly show stale levels.
        for (auto it = cells_.begin(); it != cells_.end(); ++it) {
            *it = Cell{};
        }
        emit levelsUpdated();
    }
}

} // namespace pipeeq
