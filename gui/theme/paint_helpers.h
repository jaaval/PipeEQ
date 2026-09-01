#pragma once

#include <QColor>
#include <QPainter>
#include <QRect>
#include <QString>

#include "theme.h"

namespace pipeeq::theme::paint {

// Shared drawing primitives.
//
// Two of these exist for performance rather than tidiness, and the reasons are
// worth keeping in view: the meter gradient is cached as a pixmap because
// rebuilding a QLinearGradient at the meter rate across two dozen strips is
// pure waste, and meters are drawn as discrete segments because that makes the
// lit count usually UNCHANGED between frames, which lets a repaint be skipped
// entirely. The aesthetic choice and the cheap choice coincide.

// Snaps a rect to whole device pixels. Hand-drawn 1 px lines land on half
// pixels at fractional scaling and come out as a blurry 2 px smear otherwise.
QRectF snapped(const QRectF& rect, qreal devicePixelRatio);

// Fills `rect` up to `norm` (0..1) of its height with the meter gradient,
// bottom-up, quantised into segments.
void drawMeter(QPainter& painter, const QRect& rect, double norm, const Tokens& tokens,
                int segments = 24);

// A hatched fill, for a meter whose device isn't present: an empty meter reads
// as "silence", which is a lie when the truth is "no signal available".
void drawHatchedMeter(QPainter& painter, const QRect& rect, const Tokens& tokens);

// The peak-hold marker: a thin line at `norm`.
void drawMeterHold(QPainter& painter, const QRect& rect, double norm, const Tokens& tokens);

// A fader track with a cap at `norm`, plus a unity tick.
void drawFader(QPainter& painter, const QRect& rect, double norm, double unityNorm,
                const Tokens& tokens, bool enabled);

// A small pill-shaped button. `active` picks the accent colour.
void drawPill(QPainter& painter, const QRect& rect, const QString& label, bool active,
               const QColor& activeColor, const Tokens& tokens, bool enabled);

// The clip indicator.
void drawClipLed(QPainter& painter, const QRect& rect, bool lit, const Tokens& tokens);

// Text elided to fit, drawn with the given alignment.
void drawElidedText(QPainter& painter, const QRect& rect, const QString& text, int alignment);

// A focus ring just inside `rect`.
void drawFocusRing(QPainter& painter, const QRect& rect, const Tokens& tokens);

} // namespace pipeeq::theme::paint
