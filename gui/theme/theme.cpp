#include "theme.h"

#include <array>

#include <QApplication>
#include <QCryptographicHash>
#include <QFontDatabase>
#include <QFontInfo>
#include <QStyleFactory>
#include <QWidget>

namespace pipeeq::theme {

namespace {

Tokens g_tokens;

QPalette darkPalette() {
    // Base greys. Kept a touch warm - a neutral grey next to saturated meter
    // colours reads as slightly blue, which looks accidental.
    const QColor window(0x22, 0x23, 0x25);
    const QColor base(0x1a, 0x1b, 0x1d);
    const QColor alternate(0x27, 0x28, 0x2b);
    const QColor button(0x2e, 0x2f, 0x33);
    const QColor text(0xdc, 0xdd, 0xe0);
    const QColor textDisabled(0x77, 0x79, 0x7e);
    const QColor accent(0x4a, 0x9e, 0xd9);

    QPalette palette;
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, alternate);
    palette.setColor(QPalette::ToolTipBase, button);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(0xff, 0x6b, 0x5b));
    palette.setColor(QPalette::Link, accent);
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, QColor(0x10, 0x14, 0x18));
    palette.setColor(QPalette::Mid, QColor(0x3c, 0x3e, 0x43));
    palette.setColor(QPalette::Dark, QColor(0x16, 0x17, 0x19));
    palette.setColor(QPalette::Shadow, QColor(0x0d, 0x0e, 0x0f));

    // Disabled roles have to be set explicitly; Fusion derives some of them,
    // but not consistently enough to rely on for the "device absent" look.
    palette.setColor(QPalette::Disabled, QPalette::Text, textDisabled);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, textDisabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, textDisabled);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x35, 0x3a, 0x40));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, textDisabled);
    return palette;
}

// Deliberately tiny. QSS is parsed per widget, caches poorly, doesn't compose
// with QPainter code at all, and silently disables native drawing of *parts* of
// a widget in ways that are miserable to debug. It's used here only for the few
// things QPalette cannot reach.
const char* kStyleSheet = R"(
QToolTip {
    border: 1px solid #3c3e43;
    padding: 4px 6px;
}
QScrollBar:vertical   { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle {
    background: #45474d;
    border-radius: 5px;
    min-width: 24px;
    min-height: 24px;
}
QScrollBar::handle:hover { background: #55575e; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QMenu { padding: 4px; }
QMenu::item { padding: 4px 20px 4px 20px; }
QHeaderView::section { padding: 3px 6px; border: 0; border-right: 1px solid #2e2f33; }
)";

Tokens tokensFromPalette(const QPalette& palette) {
    Tokens tokens;

    tokens.background = palette.color(QPalette::Window);
    tokens.surface = palette.color(QPalette::Button);
    tokens.surfaceSunken = palette.color(QPalette::Base);
    tokens.stripBackground = palette.color(QPalette::AlternateBase);
    tokens.border = palette.color(QPalette::Mid);
    tokens.gridLine = palette.color(QPalette::Mid).darker(115);

    tokens.text = palette.color(QPalette::Text);
    tokens.textDim = palette.color(QPalette::Text).darker(150);
    tokens.textDisabled = palette.color(QPalette::Disabled, QPalette::Text);

    tokens.accent = palette.color(QPalette::Highlight);
    tokens.accentText = palette.color(QPalette::HighlightedText);
    tokens.stripBackgroundSelected = tokens.accent.darker(320);

    tokens.muteActive = QColor(0xd9, 0x5b, 0x4a);
    tokens.linkActive = QColor(0xd8, 0xa4, 0x3c);
    tokens.warning = QColor(0xe0, 0xa8, 0x3a);

    tokens.meterLow = QColor(0x4c, 0xc0, 0x74);
    tokens.meterMid = QColor(0xa8, 0xcf, 0x4a);
    tokens.meterHigh = QColor(0xe4, 0xbb, 0x3a);
    tokens.meterClip = QColor(0xe5, 0x4b, 0x3c);
    tokens.meterTrack = palette.color(QPalette::Base).darker(115);

    tokens.curve = tokens.accent;
    tokens.curveFill = QColor(tokens.accent.red(), tokens.accent.green(), tokens.accent.blue(), 40);
    tokens.curveGhost = QColor(tokens.text.red(), tokens.text.green(), tokens.text.blue(), 70);
    tokens.handle = tokens.accent;
    tokens.handleSelected = QColor(0xff, 0xd2, 0x6b);

    tokens.uiFont = QApplication::font();

    QFont numeric = tokens.uiFont;
    // "tnum" gives fixed-advance digits in a proportional face. Not every font
    // has the feature, hence the monospace fallback below.
    numeric.setFeature("tnum", 1);
    if (!QFontInfo(numeric).fixedPitch()) {
        numeric.setFamilies(QFontDatabase::families(QFontDatabase::Latin).filter("Mono"));
        numeric.setStyleHint(QFont::Monospace);
    }
    tokens.numericFont = numeric;

    return tokens;
}

} // namespace

QColor Tokens::accentForDevice(const QString& nodeName) const {
    // Curated so nothing ever comes out muddy or illegible on the dark ground.
    static const std::array<QColor, 7> kHues = {
        QColor(0x4a, 0x9e, 0xd9), // blue
        QColor(0x5c, 0xc0, 0x9a), // teal
        QColor(0xc2, 0x8b, 0xdc), // violet
        QColor(0xe0, 0xa8, 0x4a), // amber
        QColor(0x6f, 0xb8, 0x5c), // green
        QColor(0xdd, 0x7d, 0x8e), // rose
        QColor(0x8f, 0xa4, 0xdc), // periwinkle
    };
    if (nodeName.isEmpty()) {
        return accent;
    }
    const QByteArray digest =
        QCryptographicHash::hash(nodeName.toUtf8(), QCryptographicHash::Md5);
    const auto index = static_cast<std::size_t>(static_cast<unsigned char>(digest.at(0))) % kHues.size();
    return kHues[index];
}

void install(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setPalette(darkPalette());
    app.setStyleSheet(QString::fromLatin1(kStyleSheet));
    g_tokens = tokensFromPalette(app.palette());
}

const Tokens& tokens() {
    return g_tokens;
}

Tokens tokensFor(const QWidget* widget) {
    if (!widget) {
        return g_tokens;
    }
    Tokens tokens = tokensFromPalette(widget->palette());
    // Metrics aren't derived from the palette, so carry the installed ones over
    // rather than silently resetting them to the struct's defaults.
    tokens.stripWidth = g_tokens.stripWidth;
    tokens.faderWidth = g_tokens.faderWidth;
    tokens.meterWidth = g_tokens.meterWidth;
    tokens.radius = g_tokens.radius;
    tokens.gap = g_tokens.gap;
    tokens.padding = g_tokens.padding;
    return tokens;
}

} // namespace pipeeq::theme
