#pragma once

#include <QHash>
#include <QScrollArea>
#include <QVector>
#include <QWidget>

#include "backend.h"

class QHBoxLayout;
class QLabel;

namespace pipeeq {

class AppState;
class ChannelStrip;

// The bottom row: every hardware output channel of every configured output,
// grouped by device, scrolling horizontally when there are more than fit.
//
// Link groups are collapsed into a single strip positioned at their LOWEST
// member index, so linking never reorders the rack under the cursor. Devices are
// ordered present-first, and that order is only recomputed on a topology change
// - re-sorting during interaction is exactly the class of bug the old code
// carefully avoided for its detail pane.
class StripRack : public QScrollArea {
    Q_OBJECT

public:
    explicit StripRack(AppState* state, QWidget* parent = nullptr);

    // Rebuilds from the store. Reuses existing strip widgets where the set is
    // unchanged, so a refresh can't destroy a widget mid-drag.
    void rebuild();
    // Pushes current values into the existing strips without rebuilding.
    void refreshValues();
    void refreshMeters();

    void setSelectedStripId(const QString& stripId);
    QString selectedStripId() const { return selectedStripId_; }

signals:
    void selectionChanged(const QString& stripId);
    void positionClicked(const QString& stripId);
    void linkToggleRequested(const QString& stripId);

private:
    // The strips that belong together in one widget: a link group, or one lone
    // channel.
    struct StripCluster {
        QString key; // stable across rebuilds
        QString deviceName;
        QVector<StripRow> members;
    };

    QVector<StripCluster> buildClusters() const;
    void connectStrip(ChannelStrip* strip);

    AppState* state_;
    QWidget* content_ = nullptr;
    QHBoxLayout* contentLayout_ = nullptr;
    // Keyed by cluster key, so a rebuild reuses widgets rather than recreating
    // them.
    QHash<QString, ChannelStrip*> stripWidgets_;
    QVector<QWidget*> deviceBlocks_;
    QString selectedStripId_;
};

} // namespace pipeeq
