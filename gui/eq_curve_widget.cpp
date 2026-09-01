#include "eq_curve_widget.h"

#include "theme/theme.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

namespace pipeeq {

EqCurveWidget::EqCurveWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(160);
    setMouseTracking(true);
}

void EqCurveWidget::setBands(const std::vector<eqcore::EqBand>& bands) {
    bands_ = bands;
    update();
}

QRectF EqCurveWidget::plotRect() const {
    constexpr double margin = 12.0;
    return QRectF(margin, margin, width() - 2 * margin, height() - 2 * margin);
}

double EqCurveWidget::freqToX(double freqHz) const {
    const double logMin = std::log10(kMinFreqHz);
    const double logMax = std::log10(kMaxFreqHz);
    const double t = (std::log10(std::clamp(freqHz, kMinFreqHz, kMaxFreqHz)) - logMin) / (logMax - logMin);
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
    const double t = (std::clamp(gainDb, -kMaxGainDb, kMaxGainDb) + kMaxGainDb) / (2.0 * kMaxGainDb);
    const QRectF r = plotRect();
    return r.bottom() - t * r.height();
}

double EqCurveWidget::yToGain(double y) const {
    const QRectF r = plotRect();
    const double t = std::clamp((r.bottom() - y) / r.height(), 0.0, 1.0);
    return t * 2.0 * kMaxGainDb - kMaxGainDb;
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

void EqCurveWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Colours come from the theme tokens, which are themselves derived from the
    // palette - so this still renders sensibly in a bare QWidget harness with
    // no theme installed.
    const theme::Tokens tokens = theme::tokensFor(this);

    const QRectF r = plotRect();
    painter.fillRect(rect(), tokens.surfaceSunken);
    painter.setPen(QPen(tokens.border, 1));
    painter.drawRect(r);

    painter.setPen(QPen(tokens.gridLine, 1, Qt::DotLine));
    for (double freq : {100.0, 1000.0, 10000.0}) {
        const double x = freqToX(freq);
        painter.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
    for (double gain : {-12.0, 0.0, 12.0}) {
        const double y = gainToY(gain);
        painter.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    painter.setPen(QPen(tokens.textDim, 1.5));
    painter.drawLine(QPointF(r.left(), gainToY(0.0)), QPointF(r.right(), gainToY(0.0)));

    if (!bands_.empty()) {
        // One evaluation per pixel column, batched so each band's coefficients
        // are computed once for the whole curve rather than once per point.
        const int steps = std::max(1, static_cast<int>(r.width()));
        std::vector<double> freqs(static_cast<std::size_t>(steps) + 1);
        for (int i = 0; i <= steps; ++i) {
            freqs[static_cast<std::size_t>(i)] = xToFreq(r.left() + i);
        }
        std::vector<double> responseDb(freqs.size(), 0.0);
        eqcore::eqResponseCurveDb(bands_, freqs, kSampleRateHz, responseDb);

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
        // Fill under the curve as well as stroking it: at preview size the
        // stroke alone is hard to read against the grid.
        QPainterPath fill = path;
        fill.lineTo(r.right(), gainToY(0.0));
        fill.lineTo(r.left(), gainToY(0.0));
        fill.closeSubpath();
        painter.setBrush(tokens.curveFill);
        painter.setPen(Qt::NoPen);
        painter.drawPath(fill);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(tokens.curve, 2));
        painter.drawPath(path);
    }

    for (std::size_t i = 0; i < bands_.size(); ++i) {
        const QPointF p(freqToX(bands_[i].freqHz), gainToY(bands_[i].gainDb));
        painter.setBrush(tokens.handle);
        painter.setPen(QPen(tokens.text, 1));
        painter.drawEllipse(p, 5, 5);
        painter.setFont(tokens.numericFont);
        painter.drawText(p + QPointF(7, -7), QString::number(i + 1));
    }
}

void EqCurveWidget::mousePressEvent(QMouseEvent* event) {
    draggingIndex_ = hitTestBand(event->position());
}

void EqCurveWidget::mouseMoveEvent(QMouseEvent* event) {
    if (draggingIndex_ < 0 || draggingIndex_ >= static_cast<int>(bands_.size())) {
        return;
    }
    eqcore::EqBand& band = bands_[static_cast<std::size_t>(draggingIndex_)];
    band.freqHz = xToFreq(event->position().x());
    band.gainDb = std::clamp(yToGain(event->position().y()), -kMaxGainDb, kMaxGainDb);
    update();
    emit bandEdited(draggingIndex_, band);
}

void EqCurveWidget::mouseReleaseEvent(QMouseEvent* /*event*/) {
    draggingIndex_ = -1;
}

void EqCurveWidget::wheelEvent(QWheelEvent* event) {
    const int index = hitTestBand(event->position());
    if (index < 0) {
        return;
    }
    eqcore::EqBand& band = bands_[static_cast<std::size_t>(index)];
    const double steps = event->angleDelta().y() / 120.0;
    band.q = std::clamp(band.q * std::pow(1.1, steps), 0.1, 10.0);
    update();
    emit bandEdited(index, band);
}

} // namespace pipeeq
