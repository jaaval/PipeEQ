#include "knob.h"

#include <algorithm>
#include <cmath>

#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "theme/theme.h"

namespace pipeeq {

namespace {

// The knob's face, and the sweep it uses. 270 degrees with the dead zone at the
// bottom is the convention every hardware EQ uses, and it means the pointer is
// never ambiguous between "fully down" and "fully up".
constexpr int kDiameter = 46;
constexpr double kStartAngleDeg = 225.0; // Qt angles: counter-clockwise from 3 o'clock
constexpr double kSweepDeg = 270.0;

// Pixels of vertical drag for the full sweep. Roughly a hand's travel; short
// enough to cross the range in one gesture, long enough that 0.1 dB is
// reachable without the fine modifier.
constexpr double kDragPixelsForFullRange = 240.0;

// One wheel notch, as a fraction of the sweep. 1/64 puts gain at 0.75 dB and
// frequency at about a semitone and a half per notch.
constexpr double kWheelFraction = 1.0 / 64.0;

} // namespace

Knob::Knob(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::SizeVerCursor);
    // A definite size, not a preferred one. With a Preferred policy the band
    // row's layout shrank the knobs to under half their size as soon as the
    // row was at all tight, and a rotary control that resizes with its
    // container reads as a rendering fault rather than as a design.
    setFixedSize(kDiameter, kDiameter);
}

void Knob::setRange(double minimum, double maximum) {
    minimum_ = minimum;
    maximum_ = maximum;
    value_ = std::clamp(value_, minimum_, maximum_);
    update();
}

void Knob::setTaper(Taper taper) {
    taper_ = taper;
    update();
}

void Knob::setDefaultValue(double value) {
    defaultValue_ = value;
    haveDefault_ = true;
}

void Knob::setValue(double value) {
    const double clamped = std::clamp(value, minimum_, maximum_);
    if (clamped == value_) {
        return;
    }
    value_ = clamped;
    update();
}

// A logarithmic taper needs strictly positive bounds; guarded rather than
// asserted because a caller setting a range that includes zero should get a
// linear knob, not a NaN.
double Knob::normalized() const {
    if (maximum_ <= minimum_) {
        return 0.0;
    }
    if (taper_ == Taper::Logarithmic && minimum_ > 0.0) {
        return std::log(value_ / minimum_) / std::log(maximum_ / minimum_);
    }
    return (value_ - minimum_) / (maximum_ - minimum_);
}

double Knob::fromNormalized(double norm) const {
    const double clamped = std::clamp(norm, 0.0, 1.0);
    if (taper_ == Taper::Logarithmic && minimum_ > 0.0) {
        return minimum_ * std::pow(maximum_ / minimum_, clamped);
    }
    return minimum_ + clamped * (maximum_ - minimum_);
}

void Knob::applyNormalized(double norm) {
    const double next = fromNormalized(norm);
    if (next == value_) {
        return;
    }
    value_ = next;
    update();
    emit valueChanging(value_);
}

void Knob::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    dragOriginY_ = event->position().toPoint().y();
    dragOriginNorm_ = normalized();
    setFocus(Qt::MouseFocusReason);
    emit editBegan();
}

void Knob::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        return;
    }
    // Measured from where the drag STARTED, not from the last move: accumulating
    // per-move deltas drifts, because each one is rounded to the value's
    // precision and the error compounds over a long drag.
    const double travelled = dragOriginY_ - event->position().toPoint().y();
    const double scale = (event->modifiers() & Qt::ShiftModifier) ? 0.2 : 1.0;
    applyNormalized(dragOriginNorm_ + scale * travelled / kDragPixelsForFullRange);
}

void Knob::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !dragging_) {
        return;
    }
    dragging_ = false;
    emit editFinished();
}

void Knob::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !haveDefault_) {
        return;
    }
    // The press that preceded this already opened an edit.
    value_ = std::clamp(defaultValue_, minimum_, maximum_);
    update();
    emit valueChanging(value_);
}

void Knob::wheelEvent(QWheelEvent* event) {
    const double notches = event->angleDelta().y() / 120.0;
    if (notches == 0.0) {
        return;
    }
    const double scale = (event->modifiers() & Qt::ShiftModifier) ? 0.2 : 1.0;
    applyNormalized(normalized() + scale * notches * kWheelFraction);
    event->accept();
}

void Knob::keyPressEvent(QKeyEvent* event) {
    const double scale = (event->modifiers() & Qt::ShiftModifier) ? 0.2 : 1.0;
    switch (event->key()) {
    case Qt::Key_Up:
    case Qt::Key_Right:
        applyNormalized(normalized() + scale * kWheelFraction);
        return;
    case Qt::Key_Down:
    case Qt::Key_Left:
        applyNormalized(normalized() - scale * kWheelFraction);
        return;
    case Qt::Key_PageUp:
        applyNormalized(normalized() + 8.0 * kWheelFraction);
        return;
    case Qt::Key_PageDown:
        applyNormalized(normalized() - 8.0 * kWheelFraction);
        return;
    case Qt::Key_Home:
        if (haveDefault_ && defaultValue_ != value_) {
            value_ = std::clamp(defaultValue_, minimum_, maximum_);
            update();
            emit valueChanging(value_);
        }
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

void Knob::paintEvent(QPaintEvent*) {
    const theme::Tokens tokens = theme::tokens();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int side = std::min(width(), height());
    const QRectF face(QPointF((width() - side) / 2.0, (height() - side) / 2.0),
                      QSizeF(side, side));
    // The ring rides at the outer edge and the body fills most of what is
    // inside it. Insets of a few pixels each look negligible written down and
    // are not: they come off the DIAMETER twice over, so 7 px a side left a
    // 46 px knob drawing a 20 px circle.
    constexpr double kArcWidth = 3.5;
    const QRectF arc = face.adjusted(2.5, 2.5, -2.5, -2.5);

    const double norm = normalized();

    // Track: the full sweep, dim.
    QPen trackPen(isEnabled() ? tokens.meterTrack : tokens.textDisabled, kArcWidth);
    trackPen.setCapStyle(Qt::RoundCap);
    painter.setPen(trackPen);
    painter.drawArc(arc, static_cast<int>(kStartAngleDeg * 16),
                    static_cast<int>(-kSweepDeg * 16));

    // Value: from the origin to the current position. For a range spanning zero
    // the origin is zero, so a boost and a cut are visibly opposite gestures
    // rather than both being "more arc from the left".
    if (isEnabled()) {
        double originNorm = 0.0;
        if (minimum_ < 0.0 && maximum_ > 0.0 && taper_ == Taper::Linear) {
            originNorm = (0.0 - minimum_) / (maximum_ - minimum_);
        }
        QPen valuePen(tokens.accent, kArcWidth);
        valuePen.setCapStyle(Qt::RoundCap);
        painter.setPen(valuePen);
        painter.drawArc(arc, static_cast<int>((kStartAngleDeg - originNorm * kSweepDeg) * 16),
                        static_cast<int>(-(norm - originNorm) * kSweepDeg * 16));
    }

    // Body.
    const QRectF body = arc.adjusted(4, 4, -4, -4);
    painter.setPen(QPen(tokens.border, 1.0));
    painter.setBrush(isEnabled() ? tokens.surface : tokens.background);
    painter.drawEllipse(body);

    // Pointer.
    const double angleRad = (kStartAngleDeg - norm * kSweepDeg) * M_PI / 180.0;
    const QPointF centre = body.center();
    const double inner = body.width() * 0.20;
    const double outer = body.width() * 0.44;
    QPen pointerPen(isEnabled() ? tokens.text : tokens.textDisabled, 2.0);
    pointerPen.setCapStyle(Qt::RoundCap);
    painter.setPen(pointerPen);
    painter.drawLine(
        QPointF(centre.x() + inner * std::cos(angleRad), centre.y() - inner * std::sin(angleRad)),
        QPointF(centre.x() + outer * std::cos(angleRad), centre.y() - outer * std::sin(angleRad)));

    if (hasFocus()) {
        painter.setPen(QPen(tokens.accent, 1.0, Qt::DotLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(face.adjusted(0.5, 0.5, -0.5, -0.5));
    }
}

QSize Knob::sizeHint() const {
    return QSize(kDiameter, kDiameter);
}

// ------------------------------------------------------------------ KnobField --

KnobField::KnobField(const QString& label, QWidget* parent) : QWidget(parent) {
    const theme::Tokens tokens = theme::tokens();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* caption = new QLabel(label, this);
    caption->setAlignment(Qt::AlignHCenter);
    caption->setStyleSheet(QString("color: %1;").arg(tokens.textDim.name()));
    layout->addWidget(caption);

    knob_ = new Knob(this);
    layout->addWidget(knob_, 0, Qt::AlignHCenter);

    field_ = new QDoubleSpinBox(this);
    // No arrows: that is the whole point of the knob above it. The field keeps
    // its keyboard and wheel handling.
    field_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    field_->setAlignment(Qt::AlignHCenter);
    field_->setFont(tokens.numericFont);
    field_->setKeyboardTracking(false);
    // One width for all three, so the knobs above them line up in a row rather
    // than drifting with how many digits each value happens to need.
    field_->setFixedWidth(84);
    layout->addWidget(field_, 0, Qt::AlignHCenter);

    connect(knob_, &Knob::valueChanging, this, [this](double value) {
        pushToField(value);
        emit valueEdited(value);
    });
    connect(knob_, &Knob::editBegan, this, &KnobField::editBegan);
    connect(knob_, &Knob::editFinished, this, &KnobField::editFinished);

    connect(field_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (suppress_) {
            return;
        }
        knob_->setValue(value);
        emit valueEdited(value);
    });
}

void KnobField::setRange(double minimum, double maximum) {
    knob_->setRange(minimum, maximum);
    field_->setRange(minimum, maximum);
}

void KnobField::setDecimals(int decimals) {
    field_->setDecimals(decimals);
}

void KnobField::setSuffix(const QString& suffix) {
    field_->setSuffix(suffix);
}

void KnobField::setTaper(Knob::Taper taper) {
    knob_->setTaper(taper);
}

void KnobField::setDefaultValue(double value) {
    knob_->setDefaultValue(value);
}

void KnobField::setSingleStep(double step) {
    field_->setSingleStep(step);
}

double KnobField::value() const {
    return field_->value();
}

void KnobField::setValue(double value) {
    suppress_ = true;
    field_->setValue(value);
    suppress_ = false;
    knob_->setValue(value);
}

void KnobField::pushToField(double value) {
    suppress_ = true;
    field_->setValue(value);
    suppress_ = false;
}

} // namespace pipeeq
