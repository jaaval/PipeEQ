#pragma once

#include <QColor>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QString>
#include <QWidget>

namespace pipeeq {

// A single line of text that shortens itself with an ellipsis instead of
// refusing to shrink.
//
// QLabel reports its full text width as its minimum size hint, so a long line
// in a horizontal layout cannot give way: Qt honours the minimum and the text
// runs underneath whatever sits beside it. That is exactly what happened to the
// selection line - "<device> · linked (gain, mute, sends and EQ move together)"
// disappeared under the Rename/Position buttons on a narrow window.
//
// Deliberately not a QLabel subclass, and deliberately without Q_OBJECT: it has
// no signals or slots, so it needs no moc, and the whole widget is this one
// paintEvent.
class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    void setText(const QString& text) {
        if (text == text_) {
            return;
        }
        text_ = text;
        // The full text is the tooltip, so eliding never hides information -
        // it only stops it from overlapping something else.
        setToolTip(text);
        updateGeometry();
        update();
    }
    QString text() const { return text_; }

    void setTextColor(const QColor& color) {
        color_ = color;
        update();
    }

    void clear() { setText(QString()); }

    QSize sizeHint() const override {
        const QFontMetrics metrics(font());
        return QSize(metrics.horizontalAdvance(text_), metrics.height());
    }

    // Room for an ellipsis and no more: with an Ignored horizontal policy the
    // layout is free to hand over anything from this up to sizeHint().
    QSize minimumSizeHint() const override {
        const QFontMetrics metrics(font());
        return QSize(metrics.horizontalAdvance(QStringLiteral("...")), metrics.height());
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (text_.isEmpty()) {
            return;
        }
        QPainter painter(this);
        painter.setPen(color_.isValid() ? color_ : palette().color(foregroundRole()));
        const QFontMetrics metrics(painter.font());
        painter.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(text_, Qt::ElideRight, width()));
    }

private:
    QString text_;
    QColor color_;
};

} // namespace pipeeq
