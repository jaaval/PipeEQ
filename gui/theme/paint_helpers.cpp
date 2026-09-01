#include "paint_helpers.h"

#include <algorithm>
#include <cmath>

#include <QFontMetrics>
#include <QHash>
#include <QLinearGradient>
#include <QPixmap>

namespace pipeeq::theme::paint {

namespace {

// One gradient pixmap per (height, segment count, palette generation), WITH the
// segment separators already baked in.
//
// Both halves of that matter. Rebuilding a QLinearGradient per meter per frame
// is obvious waste; less obvious is that stroking the separators live costs ~23
// drawLine calls per meter per frame, which across a full rack at 30 Hz is
// several thousand primitive calls a second for something that never changes.
// Baked in, drawing a meter is a single drawPixmap of a sub-rect.
QPixmap meterGradient(int height, int segments, const Tokens& tokens) {
    static QHash<QString, QPixmap> cache;
    const QString key = QString("%1/%2/%3/%4")
                             .arg(height)
                             .arg(segments)
                             .arg(tokens.meterLow.name(), tokens.meterClip.name());
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) {
        return *it;
    }

    QPixmap pixmap(1, std::max(1, height));
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    QLinearGradient gradient(0, height, 0, 0); // bottom-up
    gradient.setColorAt(0.00, tokens.meterLow);
    gradient.setColorAt(0.62, tokens.meterLow);
    gradient.setColorAt(0.78, tokens.meterMid);
    gradient.setColorAt(0.92, tokens.meterHigh);
    gradient.setColorAt(1.00, tokens.meterClip);
    painter.fillRect(pixmap.rect(), gradient);

    const double segmentHeight = static_cast<double>(height) / std::max(1, segments);
    painter.setPen(QPen(tokens.surfaceSunken, 1));
    for (int i = 1; i < segments; ++i) {
        const int y = height - static_cast<int>(std::round(i * segmentHeight));
        if (y <= 0) {
            break;
        }
        painter.drawLine(0, y, 0, y);
    }
    painter.end();

    // Bounded: heights come from a handful of layout sizes, not arbitrary input.
    if (cache.size() > 64) {
        cache.clear();
    }
    cache.insert(key, pixmap);
    return pixmap;
}

// The hatched "no signal available" fill, likewise cached: it never changes, and
// a BDiagPattern brush is not cheap to re-render every frame.
QPixmap hatchTile(int height, const Tokens& tokens) {
    static QHash<QString, QPixmap> cache;
    const QString key = QString("%1/%2").arg(height).arg(tokens.textDisabled.name());
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) {
        return *it;
    }

    QPixmap pixmap(16, std::max(1, height));
    pixmap.fill(tokens.meterTrack);
    QPainter painter(&pixmap);
    QColor hatch = tokens.textDisabled;
    hatch.setAlpha(70);
    painter.setBrush(QBrush(hatch, Qt::BDiagPattern));
    painter.setPen(Qt::NoPen);
    painter.drawRect(pixmap.rect());
    painter.end();

    if (cache.size() > 32) {
        cache.clear();
    }
    cache.insert(key, pixmap);
    return pixmap;
}

} // namespace

QRectF snapped(const QRectF& rect, qreal devicePixelRatio) {
    const qreal ratio = devicePixelRatio <= 0 ? 1.0 : devicePixelRatio;
    const auto snap = [ratio](qreal value) { return std::round(value * ratio) / ratio; };
    return QRectF(snap(rect.left()), snap(rect.top()), snap(rect.width()), snap(rect.height()));
}

void drawMeter(QPainter& painter, const QRect& rect, double norm, const Tokens& tokens,
                int segments) {
    painter.fillRect(rect, tokens.meterTrack);
    if (norm <= 0.0 || rect.height() <= 0) {
        return;
    }
    norm = std::clamp(norm, 0.0, 1.0);

    const int segmentCount = std::max(1, segments);
    // Quantise UP, so a signal just above a segment boundary lights it - a meter
    // that rounds down looks unresponsive at low levels.
    const int lit = static_cast<int>(std::ceil(norm * segmentCount));
    const double segmentHeight = static_cast<double>(rect.height()) / segmentCount;
    const int litHeight = static_cast<int>(std::round(lit * segmentHeight));
    if (litHeight <= 0) {
        return;
    }

    // One blit, separators included.
    const QPixmap gradient = meterGradient(rect.height(), segmentCount, tokens);
    const QRect litRect(rect.left(), rect.bottom() - litHeight + 1, rect.width(), litHeight);
    painter.drawPixmap(litRect, gradient, QRect(0, gradient.height() - litHeight, 1, litHeight));
}

void drawHatchedMeter(QPainter& painter, const QRect& rect, const Tokens& tokens) {
    painter.drawTiledPixmap(rect, hatchTile(rect.height(), tokens));
}

void drawMeterHold(QPainter& painter, const QRect& rect, double norm, const Tokens& tokens) {
    if (norm <= 0.0) {
        return;
    }
    norm = std::clamp(norm, 0.0, 1.0);
    const int y = rect.bottom() - static_cast<int>(std::round(norm * rect.height()));
    painter.setPen(QPen(tokens.text, 1));
    painter.drawLine(rect.left(), y, rect.right(), y);
}

void drawFader(QPainter& painter, const QRect& rect, double norm, double unityNorm,
                const Tokens& tokens, bool enabled) {
    const int centerX = rect.center().x();
    const QRect track(centerX - 3, rect.top(), 6, rect.height());
    painter.setPen(Qt::NoPen);
    painter.setBrush(tokens.surfaceSunken);
    painter.drawRoundedRect(track, 3, 3);

    // Unity tick, so the fader can be returned to 0 dB by eye.
    const int unityY = rect.bottom() - static_cast<int>(std::round(unityNorm * rect.height()));
    painter.setPen(QPen(tokens.gridLine, 1));
    painter.drawLine(rect.left(), unityY, rect.right(), unityY);

    constexpr int kCapHeight = 14;
    const double clamped = std::clamp(norm, 0.0, 1.0);
    const int capCenter = rect.bottom() - static_cast<int>(std::round(clamped * rect.height()));
    const QRect cap(rect.left(), std::clamp(capCenter - kCapHeight / 2, rect.top(),
                                             rect.bottom() - kCapHeight),
                     rect.width(), kCapHeight);

    // The cap has to stand off the strip background hard enough to read as a
    // physical thing to grab. Derived from the palette rather than hard-coded,
    // but pushed much further than the neighbouring surface colours - at the
    // subtle end it disappears entirely and only the grip line shows.
    const QColor capFill = enabled ? tokens.border.lighter(175) : tokens.surface;
    const QColor capEdge = enabled ? tokens.text.darker(180) : tokens.textDisabled;
    painter.setPen(QPen(capEdge, 1));
    painter.setBrush(capFill);
    painter.drawRoundedRect(cap, 3, 3);
    // A grip line, which is what makes the cap read as grabbable.
    painter.setPen(QPen(enabled ? tokens.accent : tokens.textDisabled, 2));
    painter.drawLine(cap.left() + 4, cap.center().y(), cap.right() - 4, cap.center().y());
    painter.setBrush(Qt::NoBrush);
}

void drawPill(QPainter& painter, const QRect& rect, const QString& label, bool active,
               const QColor& activeColor, const Tokens& tokens, bool enabled) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(active ? activeColor : tokens.surface);
    painter.drawRoundedRect(rect, tokens.radius, tokens.radius);

    painter.setPen(active ? tokens.accentText : (enabled ? tokens.textDim : tokens.textDisabled));
    painter.drawText(rect, Qt::AlignCenter, label);
    painter.setBrush(Qt::NoBrush);
}

void drawClipLed(QPainter& painter, const QRect& rect, bool lit, const Tokens& tokens) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(lit ? tokens.meterClip : tokens.meterTrack);
    painter.drawRoundedRect(rect, 2, 2);
    painter.setBrush(Qt::NoBrush);
}

void drawElidedText(QPainter& painter, const QRect& rect, const QString& text, int alignment) {
    const QFontMetrics metrics(painter.font());
    painter.drawText(rect, alignment, metrics.elidedText(text, Qt::ElideRight, rect.width()));
}

void drawFocusRing(QPainter& painter, const QRect& rect, const Tokens& tokens) {
    QColor ring = tokens.accent;
    ring.setAlpha(200);
    painter.setPen(QPen(ring, 1, Qt::DotLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect.adjusted(1, 1, -2, -2), tokens.radius, tokens.radius);
}

} // namespace pipeeq::theme::paint
