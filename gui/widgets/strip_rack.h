#pragma once

#include <QHash>
#include <QScrollArea>
#include <QSet>
#include <QStringList>
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

    // Collapse state is remembered per PipeWire node.name, so a rack with three
    // unplugged interfaces isn't mostly placeholders every time the app starts.
    void setCollapsedDevices(const QStringList& nodeNames);
    QStringList collapsedDevices() const;

    // The height at which every strip is fully visible: the tallest device
    // block's chrome plus a strip's own minimum. Accounts for the device
    // headers and the collapse buttons, which a hand-tuned constant does not -
    // and which is how the EQ page ended up clipping the mute/link row off
    // every strip once absent devices grew a collapse button.
    int contentMinimumHeight() const;

    // Links whatever is currently marked. Also reachable with the L key.
    void linkMarkedChannels();
    void clearLinkMarks();
    int markedCount() const { return static_cast<int>(linkMarks_.size()); }

signals:
    void selectionChanged(const QString& stripId);
    void positionClicked(const QString& stripId);
    // Human-readable progress or refusal, for the status bar.
    void statusMessage(const QString& message);
    void collapsedDevicesChanged();

private:
    // The strips that belong together in one widget: a link group, or one lone
    // channel.
    struct StripCluster {
        QString key; // stable across rebuilds
        QString deviceName;
        QVector<StripRow> members;
    };

    QVector<StripCluster> buildClusters() const;
    int stripMinimumHeight() const;
    void connectStrip(ChannelStrip* strip);
    ChannelStrip* stripAtGlobalPos(const QPoint& globalPos) const;
    void applyLinkMarks();
    void toggleLinkMark(ChannelStrip* strip);
    // Asks before discarding, then links. Returns false if it refused.
    bool linkChannels(ChannelStrip* anchor, const QVector<ChannelStrip*>& others);

    AppState* state_;
    QWidget* content_ = nullptr;
    QHBoxLayout* contentLayout_ = nullptr;
    // Keyed by cluster key, so a rebuild reuses widgets rather than recreating
    // them.
    QHash<QString, ChannelStrip*> stripWidgets_;
    // Height the tallest device block adds around its strips row, plus the
    // content margins and the frame. Recomputed in rebuild().
    int chromeHeight_ = 26;
    QVector<QWidget*> deviceBlocks_;
    QString selectedStripId_;
    // Cluster keys marked for linking. Keyed by cluster rather than strip id so
    // a rebuild doesn't lose the marks.
    QSet<QString> linkMarks_;
    QSet<QString> collapsedDevices_;
    ChannelStrip* linkDragSource_ = nullptr;
    ChannelStrip* linkDropTarget_ = nullptr;
};

} // namespace pipeeq
