#include "eq_curve_widget.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QActionGroup>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include "theme/theme.h"

namespace pipeeq {

namespace {

struct FilterTypeEntry {
    eqcore::FilterType type;
    const char* label;
};

const FilterTypeEntry kFilterTypes[] = {
    {eqcore::FilterType::Peaking, "Peaking"},
    {eqcore::FilterType::LowShelf, "Low shelf"},
    {eqcore::FilterType::HighShelf, "High shelf"},
    {eqcore::FilterType::LowPass, "Low pass"},
    {eqcore::FilterType::HighPass, "High pass"},
};

QString shortTypeName(eqcore::FilterType type) {
    switch (type) {
    case eqcore::FilterType::Peaking:
        return QStringLiteral("PK");
    case eqcore::FilterType::LowShelf:
        return QStringLiteral("LS");
    case eqcore::FilterType::HighShelf:
        return QStringLiteral("HS");
    case eqcore::FilterType::LowPass:
        return QStringLiteral("LP");
    case eqcore::FilterType::HighPass:
        return QStringLiteral("HP");
    }
    return QStringLiteral("?");
}

QString formatFreq(double freqHz) {
    if (freqHz >= 1000.0) {
        return QString::number(freqHz / 1000.0, 'f', freqHz >= 10000.0 ? 0 : 1) + "k";
    }
    return QString::number(freqHz, 'f', 0);
}

} // namespace

EqCurveWidget::EqCurveWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(160);
    setMouseTracking(true);
    // Needed for the keyboard path; the widget had no focus policy at all
    // before, so none of it was reachable without a mouse.
    setFocusPolicy(Qt::StrongFocus);
}

void EqCurveWidget::setPreviewMode(bool preview) {
    preview_ = preview;
    maxGainDb_ = preview ? 18.0 : 24.0;
    setMinimumHeight(preview ? 56 : 160);
    setMouseTracking(!preview);
    setFocusPolicy(preview ? Qt::NoFocus : Qt::StrongFocus);
    update();
}

void EqCurveWidget::setSampleRateHz(double sampleRateHz) {
    if (sampleRateHz <= 0.0) {
        sampleRateHz = kDefaultSampleRateHz;
    }
    if (qFuzzyCompare(sampleRateHz_, sampleRateHz)) {
        return;
    }
    sampleRateHz_ = sampleRateHz;
    update();
}

void EqCurveWidget::setBypassed(bool bypassed) {
    if (bypassed_ == bypassed) {
        return;
    }
    bypassed_ = bypassed;
    update();
}

void EqCurveWidget::setBands(const std::vector<eqcore::EqBand>& bands) {
    bands_ = bands;
    if (selectedIndex_ >= static_cast<int>(bands_.size())) {
        selectedIndex_ = bands_.empty() ? -1 : static_cast<int>(bands_.size()) - 1;
        emit selectedBandChanged(selectedIndex_);
    }
    update();
}

void EqCurveWidget::setSelectedBand(int index) {
    const int clamped =
        (index >= 0 && index < static_cast<int>(bands_.size())) ? index : -1;
    if (clamped == selectedIndex_) {
        return;
    }
    selectedIndex_ = clamped;
    update();
}

// ------------------------------------------------------------------ mapping --

QRectF EqCurveWidget::plotRect() const {
    if (preview_) {
        return QRectF(3, 3, width() - 6, height() - 6);
    }
    // Room for the axis labels along the left and bottom.
    constexpr double kLeft = 34.0;
    constexpr double kBottom = 18.0;
    constexpr double kMargin = 10.0;
    return QRectF(kLeft, kMargin, std::max(10.0, width() - kLeft - kMargin),
                   std::max(10.0, height() - kMargin - kBottom));
}

double EqCurveWidget::freqToX(double freqHz) const {
    const double logMin = std::log10(kMinFreqHz);
    const double logMax = std::log10(kMaxFreqHz);
    const double t =
        (std::log10(std::clamp(freqHz, kMinFreqHz, kMaxFreqHz)) - logMin) / (logMax - logMin);
    const QRectF r = plotRect();
    return r.left() + t * r.width();
}

double EqCurveWidget::xToFreq(double x) const {
    const QRectF r = plotRect();
    const double t = std::clamp((x - r.left()) / r.width(), 0.0, 1.0);
    const double logMin = std::log10(kMinFreqHz);
    const double logMax = std::log10(kMaxFreqHz);
    return std::pow(10.0, logMin + t * (logMax - logMin));
}

double EqCurveWidget::gainToY(double gainDb) const {
    const double t = (std::clamp(gainDb, -maxGainDb_, maxGainDb_) + maxGainDb_) / (2.0 * maxGainDb_);
    const QRectF r = plotRect();
    return r.bottom() - t * r.height();
}

double EqCurveWidget::yToGain(double y) const {
    const QRectF r = plotRect();
    const double t = std::clamp((r.bottom() - y) / r.height(), 0.0, 1.0);
    return t * 2.0 * maxGainDb_ - maxGainDb_;
}

int EqCurveWidget::hitTestBand(QPointF pos) const {
    int best = -1;
    double bestDist = kHitRadiusPx;
    for (std::size_t i = 0; i < bands_.size(); ++i) {
        const QPointF p(freqToX(bands_[i].freqHz), gainToY(bands_[i].gainDb));
        const double dist = QLineF(p, pos).length();
        if (dist <= bestDist) {
            bestDist = dist;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// ------------------------------------------------------------------ painting --

// A different glyph per filter type, so the curve alone says what each band is
// without consulting a table.
void EqCurveWidget::drawHandle(QPainter& painter, const eqcore::EqBand& band, int index,
                                bool selected) const {
    const theme::Tokens tokens = theme::tokensFor(this);
    const QPointF p(freqToX(band.freqHz), gainToY(band.gainDb));
    const double radius = selected ? 7.0 : 5.0;

    painter.setBrush(selected ? tokens.handleSelected : tokens.handle);
    painter.setPen(QPen(tokens.text, selected ? 2.0 : 1.0));

    switch (band.type) {
    case eqcore::FilterType::Peaking:
        painter.drawEllipse(p, radius, radius);
        break;
    case eqcore::FilterType::LowShelf:
    case eqcore::FilterType::HighShelf: {
        // A wedge, pointing the way the shelf acts.
        const double direction = band.type == eqcore::FilterType::LowShelf ? -1.0 : 1.0;
        QPolygonF wedge;
        wedge << QPointF(p.x() + direction * radius, p.y() - radius)
              << QPointF(p.x() + direction * radius, p.y() + radius)
              << QPointF(p.x() - direction * radius, p.y());
        painter.drawPolygon(wedge);
        break;
    }
    case eqcore::FilterType::LowPass:
    case eqcore::FilterType::HighPass: {
        QRectF box(p.x() - radius, p.y() - radius, radius * 2, radius * 2);
        painter.drawRect(box);
        break;
    }
    }

    painter.setFont(tokens.numericFont);
    painter.setPen(selected ? tokens.handleSelected : tokens.textDim);
    painter.drawText(p + QPointF(radius + 2, -radius - 1), QString::number(index + 1));
}

void EqCurveWidget::paintEvent(QPaintEvent* /*event*/) {
    const theme::Tokens tokens = theme::tokensFor(this);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF r = plotRect();
    painter.fillRect(rect(), tokens.surfaceSunken);
    painter.setPen(QPen(tokens.border, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(r);

    if (!preview_) {
        painter.setPen(QPen(tokens.gridLine, 1, Qt::DotLine));
        painter.setFont(tokens.numericFont);
        for (double freq : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
            const double x = freqToX(freq);
            painter.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        for (double gain : {-18.0, -12.0, -6.0, 6.0, 12.0, 18.0}) {
            const double y = gainToY(gain);
            painter.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }

        // Axis labels. Without them the vertical scale is a guess, which makes
        // the whole plot decorative rather than usable.
        painter.setPen(tokens.textDim);
        for (double gain : {-18.0, -12.0, -6.0, 0.0, 6.0, 12.0, 18.0}) {
            const double y = gainToY(gain);
            painter.drawText(QRectF(0, y - 7, r.left() - 5, 14), Qt::AlignRight | Qt::AlignVCenter,
                              QString::number(static_cast<int>(gain)));
        }
        for (double freq : {100.0, 1000.0, 10000.0}) {
            const double x = freqToX(freq);
            painter.drawText(QRectF(x - 22, r.bottom() + 2, 44, 14), Qt::AlignCenter,
                              formatFreq(freq));
        }
    }

    painter.setPen(QPen(tokens.textDim, 1.5));
    painter.drawLine(QPointF(r.left(), gainToY(0.0)), QPointF(r.right(), gainToY(0.0)));

    if (bands_.empty()) {
        if (!preview_) {
            painter.setFont(tokens.uiFont);
            painter.setPen(tokens.textDim);
            painter.drawText(r, Qt::AlignCenter, "No bands. Double-click to add one.");
        }
        return;
    }

    // One evaluation per pixel column, batched so each band's coefficients are
    // computed once for the whole curve rather than once per point.
    const int steps = std::max(1, static_cast<int>(r.width()));
    std::vector<double> freqs(static_cast<std::size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        freqs[static_cast<std::size_t>(i)] = xToFreq(r.left() + i);
    }

    const auto pathFor = [&](const std::vector<eqcore::EqBand>& bands) {
        std::vector<double> responseDb(freqs.size(), 0.0);
        eqcore::eqResponseCurveDb(bands, freqs, sampleRateHz_, responseDb);
        QPainterPath path;
        for (int i = 0; i <= steps; ++i) {
            const double x = r.left() + i;
            const double y = gainToY(responseDb[static_cast<std::size_t>(i)]);
            if (i == 0) {
                path.moveTo(x, y);
            } else {
                path.lineTo(x, y);
            }
        }
        return path;
    };

    // Individual band responses, faintly, behind the sum. Without these,
    // adjusting Q has no visible feedback beyond "the summed curve wiggled".
    if (!preview_ && bands_.size() > 1) {
        painter.setPen(QPen(tokens.curveGhost, 1, Qt::DashLine));
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            painter.drawPath(pathFor({bands_[i]}));
        }
    }

    const QPainterPath sum = pathFor(bands_);

    // Fill under the curve as well as stroking it: at preview size the stroke
    // alone is hard to read against the grid.
    QPainterPath fill = sum;
    fill.lineTo(r.right(), gainToY(0.0));
    fill.lineTo(r.left(), gainToY(0.0));
    fill.closeSubpath();
    painter.setBrush(bypassed_ ? QColor(tokens.textDisabled.red(), tokens.textDisabled.green(),
                                         tokens.textDisabled.blue(), 30)
                                : tokens.curveFill);
    painter.setPen(Qt::NoPen);
    painter.drawPath(fill);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(bypassed_ ? tokens.textDisabled : tokens.curve, 2,
                         bypassed_ ? Qt::DashLine : Qt::SolidLine));
    painter.drawPath(sum);

    // No handles in preview mode: they would be unusable at that size and just
    // add visual noise.
    if (!preview_) {
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            drawHandle(painter, bands_[i], static_cast<int>(i),
                        static_cast<int>(i) == selectedIndex_);
        }
    }

    if (bypassed_ && !preview_) {
        painter.setFont(tokens.uiFont);
        painter.setPen(tokens.warning);
        painter.drawText(r.adjusted(0, 6, -6, 0), Qt::AlignTop | Qt::AlignRight, "BYPASSED");
    }
}

// --------------------------------------------------------------- interaction --

void EqCurveWidget::mousePressEvent(QMouseEvent* event) {
    if (preview_) {
        event->ignore(); // let the EqPreview host handle the click
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    setFocus(Qt::MouseFocusReason);

    const int index = hitTestBand(event->position());
    if (index < 0) {
        return;
    }

    // Alt-click deletes without going through the context menu, which is the
    // gesture people reach for after the first few bands.
    if (event->modifiers() & Qt::AltModifier) {
        emit bandRemoveRequested(index);
        return;
    }

    setSelectedBand(index);
    emit selectedBandChanged(index);
    draggingIndex_ = index;
    emit bandEditBegan(index);
}

void EqCurveWidget::mouseMoveEvent(QMouseEvent* event) {
    if (preview_ || draggingIndex_ < 0 ||
        draggingIndex_ >= static_cast<int>(bands_.size())) {
        return;
    }
    eqcore::EqBand& band = bands_[static_cast<std::size_t>(draggingIndex_)];
    band.freqHz = xToFreq(event->position().x());
    band.gainDb = std::clamp(yToGain(event->position().y()), -maxGainDb_, maxGainDb_);
    update();
    emit bandEdited(draggingIndex_, band);
}

void EqCurveWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (preview_) {
        event->ignore();
        return;
    }
    if (draggingIndex_ >= 0) {
        emit bandEditFinished(draggingIndex_);
    }
    draggingIndex_ = -1;
}

void EqCurveWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (preview_ || event->button() != Qt::LeftButton) {
        if (preview_) {
            event->ignore();
        }
        return;
    }

    const int index = hitTestBand(event->position());
    if (index >= 0) {
        // On a handle: reset that band's gain to flat, which is the common
        // "undo what I just did to this band" gesture.
        eqcore::EqBand band = bands_[static_cast<std::size_t>(index)];
        band.gainDb = 0.0;
        bands_[static_cast<std::size_t>(index)] = band;
        update();
        emit bandEditBegan(index);
        emit bandEdited(index, band);
        emit bandEditFinished(index);
        return;
    }

    if (!plotRect().contains(event->position())) {
        return;
    }
    emit bandAddRequested(xToFreq(event->position().x()),
                           std::clamp(yToGain(event->position().y()), -maxGainDb_, maxGainDb_));
}

void EqCurveWidget::wheelEvent(QWheelEvent* event) {
    if (preview_) {
        event->ignore();
        return;
    }
    int index = hitTestBand(event->position());
    if (index < 0) {
        index = selectedIndex_;
    }
    if (index < 0 || index >= static_cast<int>(bands_.size())) {
        event->ignore();
        return;
    }

    eqcore::EqBand& band = bands_[static_cast<std::size_t>(index)];
    const double steps = event->angleDelta().y() / 120.0;
    band.q = std::clamp(band.q * std::pow(1.1, steps), 0.1, 10.0);
    update();
    emit bandEditBegan(index);
    emit bandEdited(index, band);
    emit bandEditFinished(index);
    event->accept();
}

void EqCurveWidget::keyPressEvent(QKeyEvent* event) {
    if (preview_ || selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(bands_.size())) {
        QWidget::keyPressEvent(event);
        return;
    }

    eqcore::EqBand band = bands_[static_cast<std::size_t>(selectedIndex_)];
    const bool fine = event->modifiers() & Qt::ShiftModifier;
    bool changed = false;

    switch (event->key()) {
    case Qt::Key_Left:
        // Logarithmic step, so a nudge moves a comparable musical interval at
        // both ends of the range rather than being useless at one of them.
        band.freqHz = std::clamp(band.freqHz / (fine ? 1.01 : 1.06), kMinFreqHz, kMaxFreqHz);
        changed = true;
        break;
    case Qt::Key_Right:
        band.freqHz = std::clamp(band.freqHz * (fine ? 1.01 : 1.06), kMinFreqHz, kMaxFreqHz);
        changed = true;
        break;
    case Qt::Key_Up:
        band.gainDb = std::clamp(band.gainDb + (fine ? 0.1 : 0.5), -maxGainDb_, maxGainDb_);
        changed = true;
        break;
    case Qt::Key_Down:
        band.gainDb = std::clamp(band.gainDb - (fine ? 0.1 : 0.5), -maxGainDb_, maxGainDb_);
        changed = true;
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        emit bandRemoveRequested(selectedIndex_);
        return;
    case Qt::Key_Tab:
        setSelectedBand((selectedIndex_ + 1) % static_cast<int>(bands_.size()));
        emit selectedBandChanged(selectedIndex_);
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }

    if (changed) {
        bands_[static_cast<std::size_t>(selectedIndex_)] = band;
        update();
        emit bandEditBegan(selectedIndex_);
        emit bandEdited(selectedIndex_, band);
        emit bandEditFinished(selectedIndex_);
    }
}

void EqCurveWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (preview_) {
        return;
    }
    const int index = hitTestBand(event->pos());
    if (index < 0) {
        return;
    }
    setSelectedBand(index);
    emit selectedBandChanged(index);

    QMenu menu(this);
    auto* typeMenu = menu.addMenu("Filter type");
    auto* group = new QActionGroup(typeMenu);
    group->setExclusive(true);
    for (const FilterTypeEntry& entry : kFilterTypes) {
        QAction* action = typeMenu->addAction(entry.label);
        action->setCheckable(true);
        action->setChecked(bands_[static_cast<std::size_t>(index)].type == entry.type);
        group->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, index, type = entry.type] { emit bandTypeChangeRequested(index, type); });
    }
    menu.addSeparator();
    QAction* remove = menu.addAction("Remove band " + QString::number(index + 1));
    connect(remove, &QAction::triggered, this, [this, index] { emit bandRemoveRequested(index); });

    menu.exec(event->globalPos());
}

} // namespace pipeeq
