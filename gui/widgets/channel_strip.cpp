#include "channel_strip.h"

#include <algorithm>
#include <cmath>

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include "fader_taper.h"
#include "strip_metrics.h"
#include "model/level_meters.h"
#include "theme/paint_helpers.h"

namespace pipeeq {

namespace {

constexpr int kStripExtraPerMember = 14;
constexpr int kRowHeight = 18;
constexpr int kMeterWidth = 9;

// Dragging with Shift held moves at this fraction of pointer speed, for fine
// adjustment near unity.
constexpr double kFineDragFactor = 0.25;

// How far the pointer must travel before a press on an unselected strip's fader
// counts as a drag rather than a click.
constexpr int kDragThresholdPx = 3;

QString formatDb(double db) {
    if (taper::isSilent(db)) {
        return QStringLiteral("-∞");
    }
    return QString::number(db, 'f', 1);
}

// The label for the name row.
//
// The position badge above already shows the channel positions and the device
// header already shows the device, so repeating either here just wastes the one
// line of text a strip has. For a link group, the members' own names usually
// share a prefix ("Mains L" / "Mains R"), and that prefix is the group's real
// name - so use it rather than showing one member's name for the whole group,
// which reads as if the other member were missing.
QString stripLabel(const QVector<StripRow>& strips) {
    if (strips.isEmpty()) {
        return {};
    }
    if (strips.size() == 1) {
        return strips.front().channelName;
    }

    QString prefix = strips.front().channelName;
    for (const StripRow& strip : strips) {
        if (strip.channelName.isEmpty()) {
            prefix.clear();
            break;
        }
        int shared = 0;
        while (shared < prefix.size() && shared < strip.channelName.size() &&
               prefix.at(shared) == strip.channelName.at(shared)) {
            ++shared;
        }
        prefix.truncate(shared);
    }
    while (!prefix.isEmpty() && (prefix.back().isSpace() || prefix.back() == '-' ||
                                  prefix.back() == '_' || prefix.back() == '/')) {
        prefix.chop(1);
    }
    if (!prefix.isEmpty()) {
        return prefix;
    }

    // No usable common name: say how many channels are linked instead, which is
    // at least true and not redundant with the badge.
    return QString("%1 linked").arg(strips.size());
}

} // namespace

ChannelStrip::ChannelStrip(const LevelMeters* meters, QWidget* parent)
    : QWidget(parent), meters_(meters) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    tokens_ = theme::tokensFor(this);
    accent_ = tokens_.accent;
}

void ChannelStrip::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        tokens_ = theme::tokensFor(this);
    }
    QWidget::changeEvent(event);
}

void ChannelStrip::setStrips(const QVector<StripRow>& strips) {
    strips_ = strips;
    // A snapshot arriving mid-drag must not fight the local value; the store
    // guards the write, and this guards the display.
    if (!draggingFader_) {
        hasPendingGain_ = false;
    }
    recomputeLayout();
    updateGeometry();
    update();
}

QString ChannelStrip::primaryId() const {
    return strips_.isEmpty() ? QString() : strips_.front().id;
}

QString ChannelStrip::outputId() const {
    return strips_.isEmpty() ? QString() : strips_.front().outputId;
}

uint32_t ChannelStrip::primaryChannelIndex() const {
    return strips_.isEmpty() ? 0 : strips_.front().channelIndex;
}

void ChannelStrip::setSelected(bool selected) {
    if (selected_ == selected) {
        return;
    }
    selected_ = selected;
    update();
}

void ChannelStrip::setMarkedForLink(bool marked) {
    if (markedForLink_ == marked) {
        return;
    }
    markedForLink_ = marked;
    update();
}

void ChannelStrip::setLinkDropTarget(bool target) {
    if (linkDropTarget_ == target) {
        return;
    }
    linkDropTarget_ = target;
    update();
}

double ChannelStrip::gainDb() const {
    if (hasPendingGain_) {
        return pendingGainDb_;
    }
    return strips_.isEmpty() ? 0.0 : strips_.front().gainDb;
}

bool ChannelStrip::isDriven() const {
    return !strips_.isEmpty() && strips_.front().connected && strips_.front().driven;
}

QRect ChannelStrip::faderRect() const {
    return layout_.fader;
}

QRect ChannelStrip::meterAreaRect() const {
    return layout_.meterArea;
}

int ChannelStrip::naturalWidth() const {
    const int members = std::max(1, static_cast<int>(strips_.size()));
    return strip::kBaseWidth + (members - 1) * kStripExtraPerMember;
}

double ChannelStrip::maxWidthScale() {
    return strip::kMaxWidthScale;
}

void ChannelStrip::setWidthScale(double scale) {
    const double clamped = std::clamp(scale, 1.0, strip::kMaxWidthScale);
    if (clamped == widthScale_) {
        return;
    }
    widthScale_ = clamped;
    updateGeometry();
    recomputeLayout();
    update();
}

QSize ChannelStrip::sizeHint() const {
    return QSize(static_cast<int>(std::lround(naturalWidth() * widthScale_)), 250);
}

QSize ChannelStrip::minimumSizeHint() const {
    return QSize(sizeHint().width(), intrinsicMinimumHeight());
}

void ChannelStrip::recomputeLayout() {
    const int pad = 4;
    int y = pad;
    const int innerWidth = width() - 2 * pad;
    // The fader widens with the strip rather than staying 24 px in a strip
    // twice that wide: the proportions are what make it read as a fader, and a
    // wider grip is easier to hit - which matters more now that the meter
    // beside it no longer accepts a press.
    const int faderWidth = static_cast<int>(std::lround(strip::kFaderWidth * widthScale_));

    layout_.positionBadge = QRect(pad, y, innerWidth, kRowHeight);
    y += kRowHeight + 2;
    layout_.name = QRect(pad, y, innerWidth, kRowHeight - 2);
    y += kRowHeight;
    layout_.eqChip = QRect(pad, y, innerWidth, kRowHeight - 3);
    y += kRowHeight - 1;
    layout_.clip = QRect(pad, y, innerWidth, 4);
    y += 7;

    // The fader and the meters share the tall middle region: fader on the left,
    // one meter slice per group member to its right.
    const int bottomBlock = kRowHeight * 2 + 8;
    const int middleHeight = std::max(40, height() - y - bottomBlock - pad);
    layout_.fader = QRect(pad, y, faderWidth, middleHeight);
    layout_.meterArea =
        QRect(layout_.fader.right() + 3, y, std::max(kMeterWidth, innerWidth - faderWidth - 3),
               middleHeight);
    y += middleHeight + 3;

    layout_.readout = QRect(pad, y, innerWidth, kRowHeight - 3);
    y += kRowHeight - 1;
    layout_.mute = QRect(pad, y, innerWidth / 2 - 1, kRowHeight - 2);
    layout_.link = QRect(pad + innerWidth / 2 + 1, y, innerWidth / 2 - 1, kRowHeight - 2);
}

QRect ChannelStrip::meterRectFor(int memberIndex) const {
    const int members = std::max(1, static_cast<int>(strips_.size()));
    const int slice = layout_.meterArea.width() / members;
    return QRect(layout_.meterArea.left() + memberIndex * slice, layout_.meterArea.top(),
                  std::max(3, slice - 2), layout_.meterArea.height());
}

void ChannelStrip::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    recomputeLayout();
}

void ChannelStrip::refreshMeters(bool force) {
    if (!meters_ || strips_.isEmpty()) {
        return;
    }

    // Skip a tick that wouldn't visibly change anything. With meters quantised
    // into segments, the lit count is usually identical between frames, so this
    // bails far more often than the 0.5 dB threshold alone suggests.
    lastDrawnLevelDb_.resize(strips_.size());
    lastDrawnClip_.resize(strips_.size());
    bool changed = false;
    for (int i = 0; i < strips_.size(); ++i) {
        const StripRow& strip = strips_.at(i);
        const double levelDb =
            meters_->levelDb(strip.outputId, static_cast<int>(strip.channelIndex));
        const bool clipped = meters_->clipped(strip.outputId, static_cast<int>(strip.channelIndex));
        if (std::fabs(levelDb - lastDrawnLevelDb_[i]) >= 0.5 || clipped != lastDrawnClip_[i]) {
            changed = true;
        }
        lastDrawnLevelDb_[i] = levelDb;
        lastDrawnClip_[i] = clipped;
    }
    if (!changed && !force) {
        return;
    }

    // Only the meter region: the static parts - labels, badges, fader cap -
    // then never repaint on a meter tick, which is the biggest single win here.
    update(layout_.meterArea);
    update(layout_.clip);
}

void ChannelStrip::paintEvent(QPaintEvent* event) {
    if (strips_.isEmpty()) {
        return;
    }
    const theme::Tokens& tokens = tokens_;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const StripRow& primary = strips_.front();
    const bool driven = isDriven();
    const bool linked = strips_.size() > 1;

    // A meter tick invalidates only the meter and clip rects. Qt would clip the
    // rest away, but the DRAWING CALLS still cost - text layout, elision
    // metrics, rounded-rect paths - so skip them outright rather than relying on
    // the clip. This is what takes a full rack of meters from expensive to free.
    const QRect dynamicRegion = layout_.meterArea.united(layout_.clip);
    if (dynamicRegion.contains(event->rect())) {
        for (int i = 0; i < strips_.size(); ++i) {
            const StripRow& strip = strips_.at(i);
            const QRect meterRect = meterRectFor(i);
            if (!driven) {
                theme::paint::drawHatchedMeter(painter, meterRect, tokens);
                continue;
            }
            double levelDb = LevelMeters::kSilenceDb;
            double holdDb = LevelMeters::kSilenceDb;
            bool clipped = false;
            if (meters_) {
                levelDb = meters_->levelDb(strip.outputId, static_cast<int>(strip.channelIndex));
                holdDb = meters_->holdDb(strip.outputId, static_cast<int>(strip.channelIndex));
                clipped = meters_->clipped(strip.outputId, static_cast<int>(strip.channelIndex));
            }
            theme::paint::drawMeter(painter, meterRect, taper::dbToNorm(levelDb), tokens);
            theme::paint::drawMeterHold(painter, meterRect, taper::dbToNorm(holdDb), tokens);

            const int slice =
                std::max(4, layout_.clip.width() / static_cast<int>(strips_.size()) - 2);
            theme::paint::drawClipLed(
                painter, QRect(layout_.clip.left() + i * (slice + 2), layout_.clip.top(), slice, 4),
                clipped, tokens);
        }
        return;
    }

    // Background. A group gets a slightly raised ground plus an accent edge, so
    // "these channels move together" is visible without reading any text.
    QColor background = selected_ ? tokens.stripBackgroundSelected : tokens.stripBackground;
    if (!driven) {
        background = background.darker(115);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), tokens.radius, tokens.radius);

    // Three distinct outlines, in priority order: a live link drop target, a
    // strip marked for linking, then the primary selection. They have to be
    // told apart at a glance or a multi-select is impossible to follow.
    QColor outline = tokens.border;
    int outlineWidth = 1;
    if (linkDropTarget_) {
        outline = tokens.linkActive;
        outlineWidth = 3;
    } else if (markedForLink_) {
        outline = tokens.linkActive;
        outlineWidth = 2;
    } else if (selected_) {
        outline = tokens.accent;
        outlineWidth = 2;
    }
    painter.setPen(QPen(outline, outlineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), tokens.radius, tokens.radius);

    // Everything below is dimmed when the channel isn't carrying audio, while
    // staying fully legible and editable - that distinction is load-bearing.
    const QColor primaryText = driven ? tokens.text : tokens.textDisabled;
    const QColor secondaryText = driven ? tokens.textDim : tokens.textDisabled;

    // Position badge: all member positions, e.g. "FL+FR".
    QStringList positions;
    for (const StripRow& strip : strips_) {
        positions << (strip.position.isEmpty() ? QStringLiteral("?") : strip.position);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(linked ? accent_.darker(220) : tokens.surface);
    painter.drawRoundedRect(layout_.positionBadge, tokens.radius, tokens.radius);
    painter.setPen(driven ? accent_.lighter(140) : tokens.textDisabled);
    painter.setFont(tokens.uiFont);
    theme::paint::drawElidedText(painter, layout_.positionBadge, positions.join("+"),
                                  Qt::AlignCenter);

    painter.setPen(primaryText);
    theme::paint::drawElidedText(painter, layout_.name, stripLabel(strips_), Qt::AlignCenter);

    // EQ chip: how many bands this channel's curve has, if any.
    painter.setPen(secondaryText);
    const QString eqText =
        primary.bandCount > 0 ? QString("EQ %1").arg(primary.bandCount) : QStringLiteral("flat");
    theme::paint::drawElidedText(painter, layout_.eqChip, eqText, Qt::AlignCenter);

    // Clip LEDs and meters, one per member.
    for (int i = 0; i < strips_.size(); ++i) {
        const StripRow& strip = strips_.at(i);
        const QRect meterRect = meterRectFor(i);

        if (!driven) {
            theme::paint::drawHatchedMeter(painter, meterRect, tokens);
            continue;
        }

        double levelDb = LevelMeters::kSilenceDb;
        double holdDb = LevelMeters::kSilenceDb;
        bool clipped = false;
        if (meters_) {
            levelDb = meters_->levelDb(strip.outputId, static_cast<int>(strip.channelIndex));
            holdDb = meters_->holdDb(strip.outputId, static_cast<int>(strip.channelIndex));
            clipped = meters_->clipped(strip.outputId, static_cast<int>(strip.channelIndex));
        }
        theme::paint::drawMeter(painter, meterRect, taper::dbToNorm(levelDb), tokens);
        theme::paint::drawMeterHold(painter, meterRect, taper::dbToNorm(holdDb), tokens);

        const int slice = std::max(4, layout_.clip.width() / static_cast<int>(strips_.size()) - 2);
        theme::paint::drawClipLed(
            painter, QRect(layout_.clip.left() + i * (slice + 2), layout_.clip.top(), slice, 4),
            clipped, tokens);
    }

    theme::paint::drawFader(painter, layout_.fader, taper::dbToNorm(gainDb()),
                             taper::dbToNorm(taper::kUnityDb), tokens, driven);

    painter.setFont(tokens.numericFont);
    painter.setPen(primaryText);
    painter.drawText(layout_.readout, Qt::AlignCenter, formatDb(gainDb()));
    painter.setFont(tokens.uiFont);

    theme::paint::drawPill(painter, layout_.mute, QStringLiteral("M"), primary.muted,
                            tokens.muteActive, tokens, driven);
    const QString linkLabel =
        linked ? QString("⚭%1").arg(strips_.size()) : QStringLiteral("⚭");
    theme::paint::drawPill(painter, layout_.link, linkLabel, linked, tokens.linkActive, tokens,
                            true);

    if (hasFocus()) {
        theme::paint::drawFocusRing(painter, rect(), tokens);
    }
}

// ------------------------------------------------------------- interaction --

void ChannelStrip::applyGainFromY(int y) {
    const QRect track = layout_.fader;
    if (track.height() <= 0) {
        return;
    }
    const double norm =
        std::clamp(static_cast<double>(track.bottom() - y) / track.height(), 0.0, 1.0);
    commitGain(taper::normToDb(norm));
}

void ChannelStrip::commitGain(double gainDb) {
    pendingGainDb_ = gainDb;
    hasPendingGain_ = true;
    update();
    emit gainChanging(gainDb);
}

void ChannelStrip::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    const QPoint pos = event->pos();

    if (layout_.mute.contains(pos)) {
        if (!strips_.isEmpty()) {
            emit muteToggled(!strips_.front().muted);
        }
        return;
    }
    if (layout_.link.contains(pos)) {
        // The badge is the only link affordance: the strip body selects, and
        // grabbing the fader must never link. A press here starts a possible
        // drag; a release without movement is treated as a click.
        draggingLink_ = true;
        emit linkDragStarted();
        return;
    }
    if (layout_.positionBadge.contains(pos)) {
        emit positionClicked();
        return;
    }
    if (layout_.clip.contains(pos)) {
        // Clips latch on purpose - one that flashes for a frame is one nobody
        // sees - so there has to be a way to clear them. Nothing hit-tested
        // this rect before, so a single transient left the indicator red for
        // the life of the process.
        emit clipClearRequested();
        return;
    }

    if (event->modifiers() & Qt::ControlModifier) {
        // Keyboard/accessibility parity for drag-to-link, and the only way to
        // link non-adjacent channels comfortably.
        emit linkSelectionToggled();
        return;
    }

    // Captured BEFORE the emit: selection is applied synchronously, so asking
    // afterwards always says yes and the first click on a strip would jump its
    // fader - the very thing this distinction exists to prevent.
    const bool wasSelected = selected_;
    emit selectRequested();

    // The FADER only. The meter used to count as fader too, so clicking a
    // strip's meter to select it moved the gain - which is most of the strip's
    // area, and the meter is the one part of it that looks like a readout
    // rather than a control.
    if (!layout_.fader.contains(pos)) {
        return;
    }

    faderPressPos_ = pos;
    if (wasSelected) {
        // Clicking the track of a strip you are already on jumps to that value,
        // which is what mixer users expect.
        draggingFader_ = true;
        emit gainEditBegan();
        applyGainFromY(pos.y());
        return;
    }
    // First click on an unselected strip selects it and leaves the gain alone.
    // A DRAG is unambiguous, though, so it still takes effect on this first
    // touch - see mouseMoveEvent.
    faderArmed_ = true;
}

void ChannelStrip::mouseMoveEvent(QMouseEvent* event) {
    if (draggingLink_) {
        emit linkDragMoved(event->globalPosition().toPoint());
        return;
    }
    if (faderArmed_) {
        // Past the threshold this is a drag, not a click, and a drag says
        // plainly what it means - so the fader takes it even though this strip
        // was not selected when the press landed. The threshold is what keeps a
        // click with a pixel of hand tremor from counting as one.
        if ((event->pos() - faderPressPos_).manhattanLength() < kDragThresholdPx) {
            return;
        }
        faderArmed_ = false;
        draggingFader_ = true;
        emit gainEditBegan();
    }
    if (!draggingFader_) {
        return;
    }
    if (event->modifiers() & Qt::ShiftModifier) {
        // Fine drag: move a fraction of the pointer's travel from wherever the
        // value currently is, rather than jumping to the absolute position.
        const QRect track = layout_.fader;
        if (track.height() <= 0) {
            return;
        }
        const double currentNorm = taper::dbToNorm(gainDb());
        const double targetNorm =
            std::clamp(static_cast<double>(track.bottom() - event->pos().y()) / track.height(), 0.0,
                        1.0);
        const double blended = currentNorm + (targetNorm - currentNorm) * kFineDragFactor;
        commitGain(taper::normToDb(blended));
        return;
    }
    applyGainFromY(event->pos().y());
}

void ChannelStrip::mouseReleaseEvent(QMouseEvent* event) {
    if (draggingLink_) {
        draggingLink_ = false;
        emit linkDragFinished(event->globalPosition().toPoint());
        return;
    }
    // An armed press that never moved was a plain click: it selected the strip
    // and that is all it should do.
    faderArmed_ = false;
    if (!draggingFader_) {
        return;
    }
    draggingFader_ = false;
    emit gainEditFinished();
}

void ChannelStrip::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (layout_.fader.contains(event->pos()) || layout_.readout.contains(event->pos())) {
        // Unity, not minimum. Double-clicking a fader to silence it would be a
        // surprising default and is already available at the bottom detent.
        emit gainEditBegan();
        commitGain(taper::kUnityDb);
        emit gainEditFinished();
    }
}

void ChannelStrip::wheelEvent(QWheelEvent* event) {
    if (!layout_.fader.contains(event->position().toPoint()) &&
        !layout_.meterArea.contains(event->position().toPoint())) {
        // Anywhere else on the strip, let the rack scroll horizontally. Without
        // this discipline, scrolling the rack randomly changes gains.
        event->ignore();
        return;
    }

    const double steps = event->angleDelta().y() / 120.0;
    double stepDb = 1.0;
    if (event->modifiers() & Qt::ShiftModifier) {
        stepDb = 0.2;
    } else if (event->modifiers() & Qt::ControlModifier) {
        stepDb = 6.0;
    }

    const double current = taper::isSilent(gainDb()) ? taper::kMinDb : gainDb();
    emit gainEditBegan();
    commitGain(std::clamp(current + steps * stepDb, taper::kMinDb, taper::kMaxDb));
    emit gainEditFinished();
    event->accept();
}

void ChannelStrip::keyPressEvent(QKeyEvent* event) {
    // A full keyboard path matters more than usual here: once stock widgets are
    // gone, nothing is reachable without one.
    const double current = taper::isSilent(gainDb()) ? taper::kMinDb : gainDb();
    const auto nudge = [&](double delta) {
        emit gainEditBegan();
        commitGain(std::clamp(current + delta, taper::kMinDb, taper::kMaxDb));
        emit gainEditFinished();
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
        emit gainEditBegan();
        commitGain(taper::kUnityDb);
        emit gainEditFinished();
        return;
    case Qt::Key_End:
        emit gainEditBegan();
        commitGain(taper::kSilenceDb);
        emit gainEditFinished();
        return;
    case Qt::Key_M:
        if (!strips_.isEmpty()) {
            emit muteToggled(!strips_.front().muted);
        }
        return;
    case Qt::Key_Space:
        emit selectRequested();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

} // namespace pipeeq
