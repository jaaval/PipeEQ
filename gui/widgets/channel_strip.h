#pragma once

#include <QRect>
#include <QVector>
#include <QWidget>

#include "backend.h"
#include "theme/theme.h"

namespace pipeeq {

class LevelMeters;

// One mixer strip: a single hardware output channel, or a whole link group
// rendered as one strip with one fader and one meter bar per member.
//
// Deliberately ONE self-painting widget rather than a container of child
// widgets. With 16 output channels each needing a fader, meter, mute, link
// badge, position badge, label and readout, the container approach is ~128
// widgets and 16 layout passes; this is 16 widgets. It also means a meter update
// is one paintEvent on one widget, restricted to the meter rect, which is what
// makes 30 Hz across a full rack affordable.
//
// The cost is owning hit-testing. That is contained by computing every rect once
// in resizeEvent and storing them, so paintEvent and mousePressEvent agree BY
// CONSTRUCTION rather than by two parallel sets of magic numbers.
class ChannelStrip : public QWidget {
    Q_OBJECT

public:
    ChannelStrip(const LevelMeters* meters, QWidget* parent = nullptr);

    // The strips this widget represents. More than one means a link group; they
    // share the fader, the mute and the sends, so only the first is written to.
    void setStrips(const QVector<StripRow>& strips);
    const QVector<StripRow>& strips() const { return strips_; }

    QString primaryId() const;
    QString outputId() const;
    uint32_t primaryChannelIndex() const;

    void setSelected(bool selected);
    bool isSelected() const { return selected_; }
    // Part of a multi-selection being assembled for linking. Drawn distinctly
    // from the primary selection, which drives the detail panel.
    void setMarkedForLink(bool marked);
    // Highlighted as the strip a link drag would land on.
    void setLinkDropTarget(bool target);
    void setAccentColor(const QColor& color) { accent_ = color; update(); }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Repaints only the meter area. The static parts - labels, badges, fader cap
    // - then never repaint on a meter tick, which is the biggest single win in
    // the whole metering path.
    void refreshMeters();

signals:
    void selectRequested();
    // Emitted continuously while dragging, so the UI tracks the pointer. The
    // store coalesces the resulting writes.
    void gainChanging(double gainDb);
    void gainEditBegan();
    void gainEditFinished();
    void muteToggled(bool muted);
    // The link badge was clicked: unlink if this is a group, otherwise toggle
    // this strip's membership of the pending multi-selection.
    void linkToggleRequested();
    void positionClicked();
    // Ctrl+click: add or remove this strip from the pending multi-selection.
    void linkSelectionToggled();
    // Dragging FROM the link badge. The rack resolves the position to a strip.
    void linkDragStarted();
    void linkDragMoved(const QPoint& globalPos);
    void linkDragFinished(const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    // Recomputed only on resize. Everything that draws or hit-tests reads these.
    struct Layout {
        QRect positionBadge;
        QRect name;
        QRect eqChip;
        QRect clip;
        QRect meterArea; // all member meters share this; each gets a slice
        QRect fader;
        QRect readout;
        QRect mute;
        QRect link;
    };

    void recomputeLayout();
    QRect meterRectFor(int memberIndex) const;
    double gainDb() const;
    bool isDriven() const;
    void applyGainFromY(int y);
    void commitGain(double gainDb);

    const LevelMeters* meters_;
    QVector<StripRow> strips_;
    Layout layout_;
    // Cached rather than queried per paint: resolving tokens touches the font
    // database, and this widget paints at the meter rate.
    theme::Tokens tokens_;
    // Last drawn level per member, so a tick that moved nothing can be skipped.
    QVector<double> lastDrawnLevelDb_;
    QVector<bool> lastDrawnClip_;
    QColor accent_;
    bool selected_ = false;
    bool markedForLink_ = false;
    bool linkDropTarget_ = false;
    bool draggingLink_ = false;
    bool draggingFader_ = false;
    // Local value while dragging, so the strip follows the pointer instead of
    // the round trip. The store's edit guard is what stops a snapshot
    // overwriting it.
    double pendingGainDb_ = 0.0;
    bool hasPendingGain_ = false;
};

} // namespace pipeeq
