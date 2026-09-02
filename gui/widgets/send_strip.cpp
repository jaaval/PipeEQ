#include "send_strip.h"

#include <algorithm>
#include <cmath>

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include "fader_taper.h"
#include "strip_metrics.h"
#include "model/level_meters.h"
#include "theme/paint_helpers.h"

namespace pipeeq {

namespace {

constexpr int kRowHeight = 18;

QString formatDb(double db) {
    if (taper::isSilent(db)) {
        return QStringLiteral("-∞");
    }
    return QString::number(db, 'f', 1);
}

// "5.1" reads faster than "6 ch" and is what the layout actually is; fall back
// to a channel count for anything without a common name.
QString layoutBadge(const QVector<QString>& positions) {
    switch (positions.size()) {
    case 0:
        return QStringLiteral("-");
    case 1:
        return QStringLiteral("mono");
    case 2:
        return QStringLiteral("stereo");
    case 6:
        return QStringLiteral("5.1");
    case 8:
        return QStringLiteral("7.1");
    case 12:
        return QStringLiteral("7.1.4");
    default:
        return QString("%1 ch").arg(positions.size());
    }
}

} // namespace

SendStrip::SendStrip(const LevelMeters* meters, QWidget* parent)
    : QWidget(parent), meters_(meters) {
    setFocusPolicy(Qt::StrongFocus);
    tokens_ = theme::tokensFor(this);
}

void SendStrip::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        tokens_ = theme::tokensFor(this);
    }
    QWidget::changeEvent(event);
}

void SendStrip::setInput(const InputRow& input) {
    input_ = input;
    setAccessibleName(QString("Send from %1").arg(input.displayName));
    setAccessibleDescription(
        QStringLiteral("Up and down arrows set the level, space switches the send on or off."));
    // Recomputed here as well as on resize, the way the mixer strip does it on
    // setStrips. Depending on a resize event alone leaves the layout - and so
    // every hit-test - empty until one arrives, which is a state the widget
    // should not have.
    recomputeLayout();
    updateGeometry();
    update();
}

void SendStrip::setSend(bool routed, double gainDb) {
    routed_ = routed;
    gainDb_ = gainDb;
    if (!dragging_) {
        hasPending_ = false;
    }
    update();
}

void SendStrip::setCanRoute(bool canRoute) {
    canRoute_ = canRoute;
    update();
}

double SendStrip::level() const {
    return hasPending_ ? pendingGainDb_ : gainDb_;
}

int SendStrip::naturalWidth() {
    return strip::kBaseWidth;
}

void SendStrip::setWidthScale(double scale) {
    const double clamped = std::clamp(scale, 1.0, strip::kMaxWidthScale);
    if (clamped == widthScale_) {
        return;
    }
    widthScale_ = clamped;
    updateGeometry();
    recomputeLayout();
    update();
}

int SendStrip::scaledWidth() const {
    return static_cast<int>(std::lround(naturalWidth() * widthScale_));
}

QSize SendStrip::sizeHint() const {
    return QSize(scaledWidth(), 180);
}

// The same width as the size hint: these sit in a horizontal row with a
// trailing stretch, and letting them compress below their width would squeeze
// the fader out from under the pointer.
QSize SendStrip::minimumSizeHint() const {
    return QSize(scaledWidth(), 150);
}

void SendStrip::recomputeLayout() {
    const int pad = 4;
    const int innerWidth = width() - 2 * pad;
    int y = pad;

    layout_.name = QRect(pad, y, innerWidth, kRowHeight - 2);
    y += kRowHeight;
    layout_.layoutBadge = QRect(pad, y, innerWidth, kRowHeight - 3);
    y += kRowHeight;

    const int bottomBlock = kRowHeight * 2 + 6;
    const int middleHeight = std::max(36, height() - y - bottomBlock - pad);
    const int faderWidth = static_cast<int>(std::lround(strip::kFaderWidth * widthScale_));
    layout_.fader = QRect(pad, y, faderWidth, middleHeight);
    layout_.meterArea = QRect(layout_.fader.right() + 3, y,
                               std::max(8, innerWidth - faderWidth - 3), middleHeight);
    y += middleHeight + 3;

    layout_.readout = QRect(pad, y, innerWidth, kRowHeight - 3);
    y += kRowHeight - 1;
    layout_.onButton = QRect(pad, y, innerWidth, kRowHeight - 2);
}

void SendStrip::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    recomputeLayout();
}

void SendStrip::refreshMeters() {
    if (!meters_) {
        return;
    }
    const int channels = std::max(1, static_cast<int>(input_.positions.size()));
    lastDrawnLevelDb_.resize(channels);
    bool changed = false;
    for (int i = 0; i < channels; ++i) {
        const double levelDb = meters_->levelDb(input_.id, i);
        if (std::fabs(levelDb - lastDrawnLevelDb_[i]) >= 0.5) {
            changed = true;
        }
        lastDrawnLevelDb_[i] = levelDb;
    }
    if (changed) {
        update(layout_.meterArea);
    }
}

void SendStrip::paintEvent(QPaintEvent* event) {
    const theme::Tokens& tokens = tokens_;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const int channels = std::max(1, static_cast<int>(input_.positions.size()));
    const auto drawMeters = [&] {
        const int slice = layout_.meterArea.width() / channels;
        for (int i = 0; i < channels; ++i) {
            const QRect meterRect(layout_.meterArea.left() + i * slice, layout_.meterArea.top(),
                                   std::max(3, slice - 1), layout_.meterArea.height());
            const double levelDb =
                meters_ ? meters_->levelDb(input_.id, i) : LevelMeters::kSilenceDb;
            theme::paint::drawMeter(painter, meterRect, taper::dbToNorm(levelDb), tokens, 20);
        }
    };

    // Meter-only repaint, same reasoning as ChannelStrip.
    if (layout_.meterArea.contains(event->rect())) {
        drawMeters();
        return;
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(tokens.stripBackground);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), tokens.radius, tokens.radius);
    painter.setPen(QPen(tokens.border, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), tokens.radius, tokens.radius);

    painter.setPen(routed_ ? tokens.text : tokens.textDim);
    theme::paint::drawElidedText(painter, layout_.name, input_.displayName, Qt::AlignCenter);

    painter.setPen(tokens.textDim);
    theme::paint::drawElidedText(painter, layout_.layoutBadge, layoutBadge(input_.positions),
                                  Qt::AlignCenter);

    drawMeters();

    theme::paint::drawFader(painter, layout_.fader, taper::dbToNorm(level()),
                             taper::dbToNorm(taper::kUnityDb), tokens, routed_);

    painter.setFont(tokens.numericFont);
    painter.setPen(routed_ ? tokens.text : tokens.textDisabled);
    painter.drawText(layout_.readout, Qt::AlignCenter, routed_ ? formatDb(level()) : "off");
    painter.setFont(tokens.uiFont);

    const bool enabled = routed_ || canRoute_;
    theme::paint::drawPill(painter, layout_.onButton, routed_ ? "ON" : "off", routed_,
                            tokens.accent, tokens, enabled);

    if (hasFocus()) {
        theme::paint::drawFocusRing(painter, rect(), tokens);
    }
}

void SendStrip::applyLevelFromY(int y) {
    if (layout_.fader.height() <= 0) {
        return;
    }
    // height() - 1: bottom() is the last pixel of the track, so dividing by the
    // full height leaves the topmost pixel short of maximum.
    const double travel = std::max(1, layout_.fader.height() - 1);
    const double norm =
        std::clamp(static_cast<double>(layout_.fader.bottom() - y) / travel, 0.0, 1.0);
    commitLevel(taper::normToDb(norm));
}

void SendStrip::commitLevel(double gainDb) {
    pendingGainDb_ = gainDb;
    hasPending_ = true;
    update();
    emit levelChanging(gainDb);
}

void SendStrip::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    setFocus(Qt::MouseFocusReason);

    if (layout_.onButton.contains(event->pos())) {
        if (routed_ || canRoute_) {
            emit routedToggled(!routed_);
        }
        return;
    }
    if (!routed_) {
        return; // nothing to set a level on until it's routed
    }
    // The FADER only, matching the mixer strips below: the meter is a readout,
    // and it is most of a strip's width. There is no select-first rule to apply
    // here, though - a send strip is never the thing being selected, so a press
    // on its fader acts at once.
    if (layout_.fader.contains(event->pos())) {
        dragging_ = true;
        emit levelEditBegan();
        applyLevelFromY(event->pos().y());
    }
}

void SendStrip::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        applyLevelFromY(event->pos().y());
    }
}

void SendStrip::mouseReleaseEvent(QMouseEvent* /*event*/) {
    if (!dragging_) {
        return;
    }
    dragging_ = false;
    emit levelEditFinished();
}

void SendStrip::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!routed_ || event->button() != Qt::LeftButton) {
        return;
    }
    if (layout_.fader.contains(event->pos()) || layout_.readout.contains(event->pos())) {
        emit levelEditBegan();
        commitLevel(taper::kUnityDb);
        emit levelEditFinished();
    }
}

// A send strip takes focus, so it is a tab stop - and it had no key handling at
// all, which made every one of them a dead stop with no keyboard way to switch
// a send on or set its level. Narrowing the press region to the fader made that
// worse, not better.
void SendStrip::keyPressEvent(QKeyEvent* event) {
    const auto nudge = [&](double delta) {
        if (!routed_) {
            return;
        }
        const double current = taper::isSilent(level()) ? taper::kMinDb : level();
        emit levelEditBegan();
        commitLevel(std::clamp(current + delta, taper::kMinDb, taper::kMaxDb));
        emit levelEditFinished();
    };

    switch (event->key()) {
    case Qt::Key_Up:
        nudge(event->modifiers() & Qt::ShiftModifier ? 0.2 : 1.0);
        return;
    case Qt::Key_Down:
        nudge(event->modifiers() & Qt::ShiftModifier ? -0.2 : -1.0);
        return;
    case Qt::Key_PageUp:
        nudge(6.0);
        return;
    case Qt::Key_PageDown:
        nudge(-6.0);
        return;
    case Qt::Key_Home:
        if (routed_) {
            emit levelEditBegan();
            commitLevel(taper::kUnityDb);
            emit levelEditFinished();
        }
        return;
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (routed_ || canRoute_) {
            emit routedToggled(!routed_);
        }
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void SendStrip::wheelEvent(QWheelEvent* event) {
    if (!routed_ || (!layout_.fader.contains(event->position().toPoint()) &&
                      !layout_.meterArea.contains(event->position().toPoint()))) {
        event->ignore();
        return;
    }
    if (event->angleDelta().y() == 0) {
        event->ignore(); // horizontal wheel: not a level change
        return;
    }
    const double steps = event->angleDelta().y() / 120.0;
    const double stepDb = (event->modifiers() & Qt::ShiftModifier) ? 0.2 : 1.0;
    const double current = taper::isSilent(level()) ? taper::kMinDb : level();
    emit levelEditBegan();
    commitLevel(std::clamp(current + steps * stepDb, taper::kMinDb, taper::kMaxDb));
    emit levelEditFinished();
    event->accept();
}

} // namespace pipeeq
