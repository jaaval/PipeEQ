#pragma once

#include <QColor>
#include <QWidget>

class QDoubleSpinBox;

namespace pipeeq {

// A rotary control: drag it vertically or roll the wheel over it.
//
// Rotary rather than a slider because an EQ band has three parameters that all
// want the same amount of room and none of which is a level - a row of three
// horizontal sliders reads as a hierarchy that isn't there. And a knob's travel
// is unbounded by its own width, which is what lets frequency cover three
// decades in 44 pixels.
//
// Vertical drag only, deliberately: an angular drag (follow the mouse round the
// rim) sounds more natural and is worse, because it makes the gain-per-pixel
// depend on how far from the centre you happened to grab, and it jumps when the
// pointer crosses the centre.
class Knob : public QWidget {
    Q_OBJECT

public:
    // How position maps to value. Frequency and Q are perceived
    // multiplicatively - 100 -> 200 Hz is the same interval as 1k -> 2k - so a
    // linear knob would spend nine tenths of its travel above 2 kHz and be
    // unusable at the bottom.
    enum class Taper { Linear, Logarithmic };

    explicit Knob(QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    void setTaper(Taper taper);
    // The value a double-click returns to. Not set by default, in which case a
    // double-click does nothing.
    void setDefaultValue(double value);

    double value() const { return value_; }
    // Sets without emitting: for pushing a new selection into the control.
    void setValue(double value);

signals:
    // While dragging or on each wheel notch.
    void valueChanging(double value);
    // Around a drag, so a caller can hold off a remote refresh for its duration.
    // A wheel notch emits neither: it is a discrete change, not a gesture.
    void editBegan();
    void editFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    QSize sizeHint() const override;

private:
    double normalized() const;
    double fromNormalized(double norm) const;
    void applyNormalized(double norm);

    double minimum_ = 0.0;
    double maximum_ = 1.0;
    double value_ = 0.0;
    double defaultValue_ = 0.0;
    bool haveDefault_ = false;
    Taper taper_ = Taper::Linear;

    bool dragging_ = false;
    int dragOriginY_ = 0;
    double dragOriginNorm_ = 0.0;
};

// A knob with its number underneath: the knob for coarse and continuous
// adjustment, the field for typing an exact value and for reading one back.
//
// The field keeps its wheel handling and loses its arrows - the knob is the
// arrows now, and rather better at it: the arrows stepped frequency by 1 Hz,
// which is 20000 clicks to cross the range.
class KnobField : public QWidget {
    Q_OBJECT

public:
    KnobField(const QString& label, QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    void setDecimals(int decimals);
    void setSuffix(const QString& suffix);
    void setTaper(Knob::Taper taper);
    void setDefaultValue(double value);
    void setSingleStep(double step);

    double value() const;
    // Sets both halves without emitting.
    void setValue(double value);

signals:
    void valueEdited(double value);
    void editBegan();
    void editFinished();

private:
    void pushToField(double value);

    Knob* knob_ = nullptr;
    QDoubleSpinBox* field_ = nullptr;
    bool suppress_ = false;
};

} // namespace pipeeq
