#pragma once

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>

class QApplication;
class QWidget;

namespace pipeeq::theme {

// Semantic colours and metrics for the custom-painted widgets.
//
// Custom painting isn't a stylistic choice here: faders with integrated
// meters, channel strips, knobs and the EQ curve are all things no stock
// widget can be, so most pixels are painted by hand regardless. That leaves
// the theme's job as *supplying* colours and metrics rather than restyling
// stock controls - which is what this struct is.
//
// Every colour that has a QPalette role is derived from the palette, so the
// two can't drift apart: QStyle reads the palette, which is what makes the
// stock widgets that remain (menus, line edits, tooltips, scroll areas) come
// out dark for free.
struct Tokens {
    // Surfaces.
    QColor background;      // the window ground
    QColor surface;         // panels raised off it
    QColor surfaceSunken;   // wells: plot areas, meter tracks
    QColor stripBackground;
    QColor stripBackgroundSelected;
    QColor border;
    QColor gridLine;

    // Text.
    QColor text;
    QColor textDim;      // secondary labels
    QColor textDisabled; // absent devices

    // Accents and state.
    QColor accent;
    QColor accentText; // legible on top of `accent`
    QColor muteActive;
    QColor linkActive;
    QColor warning;

    // Meters, low to clipping. Deliberately four stops rather than a
    // continuous gradient: a segmented meter quantises to a fixed number of
    // lit segments, so the lit count usually doesn't change between frames and
    // a repaint can be skipped. The aesthetic choice and the cheap choice
    // coincide here.
    QColor meterLow;
    QColor meterMid;
    QColor meterHigh;
    QColor meterClip;
    QColor meterTrack;

    // EQ curve.
    QColor curve;
    QColor curveFill;
    QColor curveGhost; // a sibling channel's curve, drawn behind
    QColor handle;
    QColor handleSelected;

    // Metrics, in logical pixels.
    int stripWidth = 64;
    int faderWidth = 18;
    int meterWidth = 10;
    int radius = 3;
    int gap = 6;
    int padding = 8;

    // The UI font, and a tabular-figures variant for anything numeric.
    //
    // The numeric font is not a detail: every dB readout updates at the meter
    // rate, and with proportional digits the text visibly wobbles as values
    // change. Tabular figures are the difference between "looks like a mixer"
    // and "looks like a web page".
    QFont uiFont;
    QFont numericFont;

    // A stable, readable accent per device, so a rack of many strips is
    // scannable. Hashed into a small curated list rather than free HSV, which
    // reliably produces mud.
    QColor accentForDevice(const QString& nodeName) const;
};

// Installs the Fusion style, the dark palette and the small stylesheet, and
// populates tokens(). Call once, before creating any widget.
//
// Fusion is mandatory rather than cosmetic: a platform style (Breeze, GTK,
// Windows) partially ignores a custom palette and produces an inconsistent
// half-dark UI, and it makes screenshots incomparable across machines.
void install(QApplication& app);

const Tokens& tokens();

// Tokens for a widget, honouring any palette override set on it or an
// ancestor - so marking one strip "selected" or "absent" can be done by
// setting a modified palette rather than threading a state enum through every
// draw call.
Tokens tokensFor(const QWidget* widget);

} // namespace pipeeq::theme
