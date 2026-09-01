#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVector>

#include "model_types.h"

namespace pipeeq {

// Holds meter levels and runs their ballistics.
//
// The ballistics live here rather than in the meter widgets, and that placement
// is the important decision: a widget that simply paints whatever it was last
// handed FREEZES when the level signal stops. Decaying here means a stalled or
// disarmed daemon makes the meters fall to silence, which is the correct
// failure mode and is impossible to get otherwise.
//
// One timer drives every meter's repaint. N widget-owned timers at the meter
// rate is a scheduling mess, and they beat against each other.
class LevelMeters : public QObject {
    Q_OBJECT

public:
    explicit LevelMeters(QObject* parent = nullptr);

    // A frame from the daemon (or the fake backend). Frames arriving faster
    // than the repaint tick are peak-combined rather than overwritten, so a
    // transient between two paints isn't lost.
    void ingest(const QVector<MeterRow>& outputs, const QVector<MeterRow>& inputs);

    // Smoothed level for one channel, in dB. Silence is kSilenceDb.
    double levelDb(const QString& ownerId, int channelIndex) const;
    // The peak-hold marker, which lingers above the level for kHoldMs.
    double holdDb(const QString& ownerId, int channelIndex) const;
    // Latched until clearClip(); a clip that flashes for one frame is a clip
    // nobody sees.
    bool clipped(const QString& ownerId, int channelIndex) const;
    void clearClip(const QString& ownerId, int channelIndex);
    void clearAll();

    void setActive(bool active);
    bool isActive() const { return timer_.isActive(); }

    static constexpr double kSilenceDb = -144.0;
    static constexpr double kClipThresholdDb = -0.2;
    // Attack is instantaneous - a peak meter that misses transients is useless.
    // Release is a fall rate, so decay is independent of frame timing.
    static constexpr double kReleaseDbPerSecond = 20.0;
    static constexpr int kHoldMs = 1500;
    static constexpr int kTickMs = 33; // ~30 Hz

signals:
    // One signal per tick, not one per channel: at 30 Hz across 24 channels
    // that would be 720 emissions a second of pure overhead. Consumers pull the
    // values they need by id.
    void levelsUpdated();

private:
    struct Cell {
        double levelDb = kSilenceDb;
        double pendingPeakDb = kSilenceDb; // highest value since the last tick
        double holdDb = kSilenceDb;
        qint64 holdSetAtMs = 0;
        bool clipped = false;
    };

    void tick();
    static QString cellKey(const QString& ownerId, int channelIndex);
    void ingestRows(const QVector<MeterRow>& rows);

    QHash<QString, Cell> cells_;
    QTimer timer_;
    qint64 elapsedMs_ = 0;
};

} // namespace pipeeq
