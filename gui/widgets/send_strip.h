#pragma once

#include <QRect>
#include <QWidget>

#include "backend.h"
#include "theme/theme.h"

namespace pipeeq {

class LevelMeters;

// One input sink's send into the current selection: name, channel-layout badge,
// on/off, a level fader and the input's own meter.
//
// Self-painting for the same reason ChannelStrip is - a rack of these plus a
// rack of output strips would otherwise be a few hundred widgets - and it shares
// the same fader taper, so a send fader and an output fader read on the same
// scale.
class SendStrip : public QWidget {
    Q_OBJECT

public:
    SendStrip(const LevelMeters* meters, QWidget* parent = nullptr);

    void setInput(const InputRow& input);
    // `routed` false means this input isn't sent here at all, which is distinct
    // from being sent at silence.
    void setSend(bool routed, double gainDb);
    // False when the output has no free send slot and this input has no slot
    // yet: the switch is then shown disabled rather than letting someone turn
    // on a send the daemon will refuse.
    void setCanRoute(bool canRoute);

    const QString& inputId() const { return input_.id; }

    // Width at scale 1, shared with the mixer strips below - see
    // strip_metrics.h. The detail panel widens these to match, within the room
    // it can spare from the EQ.
    static int naturalWidth();
    // Exposed for the same reason the mixer strip's are: which rectangle takes
    // a press is behaviour worth asserting against the real geometry.
    QRect faderRect() const { return layout_.fader; }
    QRect meterAreaRect() const { return layout_.meterArea; }
    void setWidthScale(double scale);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void refreshMeters();

signals:
    void routedToggled(bool routed);
    void levelChanging(double gainDb);
    void levelEditBegan();
    void levelEditFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Layout {
        QRect name;
        QRect layoutBadge;
        QRect meterArea;
        QRect fader;
        QRect readout;
        QRect onButton;
    };

    int scaledWidth() const;
    void recomputeLayout();
    void applyLevelFromY(int y);
    double widthScale_ = 1.0;
    void commitLevel(double gainDb);
    double level() const;

    const LevelMeters* meters_;
    InputRow input_;
    Layout layout_;
    theme::Tokens tokens_;
    bool routed_ = false;
    bool canRoute_ = true;
    double gainDb_ = 0.0;
    bool dragging_ = false;
    double pendingGainDb_ = 0.0;
    bool hasPending_ = false;
    QVector<double> lastDrawnLevelDb_;
};

} // namespace pipeeq
