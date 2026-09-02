// The rotary control for the EQ band parameters.
//
// Worth its own suite because a knob is almost entirely arithmetic - position
// to value, through a taper - and that arithmetic is invisible in a screenshot.
// A knob whose logarithmic taper is subtly wrong looks completely correct and
// makes the bottom two octaves unreachable.
//
// Every gesture here is driven by a real QEvent sent to the widget, so what is
// asserted is what a mouse or wheel would actually produce, not a helper
// method's idea of it.

#include <cmath>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include "check.h"
#include "widgets/knob.h"

using pipeeq::Knob;
using pipeeq::KnobField;

namespace {

// ------------------------------------------------------------------- helpers --

void sendWheel(QWidget* widget, int notches, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPointF centre(widget->width() / 2.0, widget->height() / 2.0);
    QWheelEvent event(centre, widget->mapToGlobal(centre.toPoint()), QPoint(),
                      QPoint(0, notches * 120), Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
    QApplication::sendEvent(widget, &event);
}

// A press, a move to `dy` pixels above the press, and a release. Positive dy is
// upward, which is the direction that raises the value.
void sendDrag(QWidget* widget, int dy, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPointF start(widget->width() / 2.0, widget->height() / 2.0);
    const QPointF end(start.x(), start.y() - dy);

    QMouseEvent press(QEvent::MouseButtonPress, start, widget->mapToGlobal(start.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, modifiers);
    QApplication::sendEvent(widget, &press);
    QMouseEvent move(QEvent::MouseMove, end, widget->mapToGlobal(end.toPoint()), Qt::NoButton,
                     Qt::LeftButton, modifiers);
    QApplication::sendEvent(widget, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, end, widget->mapToGlobal(end.toPoint()),
                        Qt::LeftButton, Qt::NoButton, modifiers);
    QApplication::sendEvent(widget, &release);
}

void sendKey(QWidget* widget, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(widget, &event);
}

// ---------------------------------------------------------------- the taper --

// The property that matters for frequency: equal arc, equal RATIO. Half the
// travel of a 20 Hz - 20 kHz knob must land on the geometric mean, ~632 Hz, not
// on the arithmetic one, 10 kHz. Get this wrong and everything below 1 kHz -
// which is most of what anyone equalises - is crammed into the first tenth.
void testLogarithmicTaperIsRatiometric() {
    Knob knob;
    knob.setRange(20.0, 20000.0);
    knob.setTaper(Knob::Taper::Logarithmic);

    // Drag the full sweep from the bottom, then half of it, and compare.
    knob.setValue(20.0);
    sendDrag(&knob, 240); // kDragPixelsForFullRange
    CHECK_NEAR(knob.value(), 20000.0, 1.0);

    knob.setValue(20.0);
    sendDrag(&knob, 120);
    CHECK_NEAR(knob.value(), std::sqrt(20.0 * 20000.0), 1.0);

    // And the ratio is the same wherever it is taken: a quarter-sweep from
    // 100 Hz multiplies by the same factor as a quarter-sweep from 1 kHz.
    knob.setValue(100.0);
    sendDrag(&knob, 60);
    const double ratioLow = knob.value() / 100.0;
    knob.setValue(1000.0);
    sendDrag(&knob, 60);
    const double ratioHigh = knob.value() / 1000.0;
    CHECK_NEAR(ratioLow, ratioHigh, 1e-6);
    CHECK(ratioLow > 1.0);
}

void testLinearTaperIsProportional() {
    Knob knob;
    knob.setRange(-24.0, 24.0);
    knob.setTaper(Knob::Taper::Linear);

    knob.setValue(-24.0);
    sendDrag(&knob, 120);
    CHECK_NEAR(knob.value(), 0.0, 1e-6);

    knob.setValue(0.0);
    sendDrag(&knob, 60);
    CHECK_NEAR(knob.value(), 12.0, 1e-6);
}

// A logarithmic taper divides by the minimum and takes a log of the ratio, so a
// range touching zero or going negative would produce an infinity or a NaN and
// paint the pointer at an undefined angle. Such a range falls back to linear
// rather than misbehaving.
void testLogarithmicTaperWithANonPositiveMinimumFallsBackToLinear() {
    Knob knob;
    knob.setRange(-6.0, 6.0);
    knob.setTaper(Knob::Taper::Logarithmic);

    knob.setValue(-6.0);
    sendDrag(&knob, 120);
    CHECK(std::isfinite(knob.value()));
    CHECK_NEAR(knob.value(), 0.0, 1e-6);
}

// ---------------------------------------------------------------- gestures --

void testDragIsMeasuredFromWhereItStarted() {
    Knob knob;
    knob.setRange(0.0, 100.0);

    // Two half-sweeps in separate gestures land at the same place as one, only
    // if each is measured from its own origin.
    knob.setValue(0.0);
    sendDrag(&knob, 120);
    const double halfway = knob.value();
    CHECK_NEAR(halfway, 50.0, 1e-6);
    sendDrag(&knob, 120);
    CHECK_NEAR(knob.value(), 100.0, 1e-6);
}

// A long drag arrives as dozens of move events and must end up where the
// pointer is, not somewhere accumulated.
//
// What this actually pins down is that each move is applied to the position the
// PRESS was at. Basing it on the current value instead - which is the tempting
// way to write it - makes every move compound the last and sends the value off
// the end of the range. It does not, despite an earlier comment here claiming
// otherwise, demonstrate anything about rounding: a faithful per-move
// accumulation is the same arithmetic to within 1e-14, far inside the tolerance
// below. Kept because the bug it does catch is the likely one.
void testManyMovesInOneDragDoNotDrift() {
    Knob knob;
    knob.setRange(0.0, 1.0);
    knob.setValue(0.0);

    const QPointF start(knob.width() / 2.0, knob.height() / 2.0);
    QMouseEvent press(QEvent::MouseButtonPress, start, knob.mapToGlobal(start.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&knob, &press);

    for (int dy = 1; dy <= 120; ++dy) {
        const QPointF at(start.x(), start.y() - dy);
        QMouseEvent move(QEvent::MouseMove, at, knob.mapToGlobal(at.toPoint()), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&knob, &move);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, start, knob.mapToGlobal(start.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&knob, &release);

    // 120 of 240 pixels, however many events it took to get there.
    CHECK_NEAR(knob.value(), 0.5, 1e-9);
}

void testShiftDragIsFiner() {
    Knob coarse;
    coarse.setRange(0.0, 100.0);
    coarse.setValue(0.0);
    sendDrag(&coarse, 120);

    Knob fine;
    fine.setRange(0.0, 100.0);
    fine.setValue(0.0);
    sendDrag(&fine, 120, Qt::ShiftModifier);

    CHECK_NEAR(fine.value(), coarse.value() * 0.2, 1e-6);
}

void testWheelStepsAndIsSymmetric() {
    Knob knob;
    knob.setRange(-24.0, 24.0);
    knob.setValue(0.0);

    sendWheel(&knob, 1);
    const double up = knob.value();
    CHECK_NEAR(up, 48.0 / 64.0, 1e-6); // one 64th of the sweep

    sendWheel(&knob, -1);
    CHECK_NEAR(knob.value(), 0.0, 1e-6);
}

void testWheelUsesTheTaper() {
    Knob knob;
    knob.setRange(20.0, 20000.0);
    knob.setTaper(Knob::Taper::Logarithmic);

    // The same notch is the same RATIO at both ends, not the same number of Hz.
    knob.setValue(50.0);
    sendWheel(&knob, 1);
    const double lowRatio = knob.value() / 50.0;

    knob.setValue(5000.0);
    sendWheel(&knob, 1);
    const double highRatio = knob.value() / 5000.0;

    CHECK_NEAR(lowRatio, highRatio, 1e-9);
}

void testValueIsClampedAtBothEnds() {
    Knob knob;
    knob.setRange(0.1, 10.0);
    knob.setTaper(Knob::Taper::Logarithmic);

    knob.setValue(5.0);
    sendDrag(&knob, 5000);
    CHECK_NEAR(knob.value(), 10.0, 1e-9);

    sendDrag(&knob, -5000);
    CHECK_NEAR(knob.value(), 0.1, 1e-9);

    // And setValue itself clamps, so a value from the daemon outside the
    // control's range cannot push the pointer off the dial.
    knob.setValue(1e6);
    CHECK_NEAR(knob.value(), 10.0, 1e-9);
    knob.setValue(-1.0);
    CHECK_NEAR(knob.value(), 0.1, 1e-9);
}

void testDoubleClickReturnsToTheDefault() {
    Knob knob;
    knob.setRange(-24.0, 24.0);
    knob.setDefaultValue(0.0);
    knob.setValue(-9.0);

    const QPointF centre(knob.width() / 2.0, knob.height() / 2.0);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, centre,
                            knob.mapToGlobal(centre.toPoint()), Qt::LeftButton, Qt::LeftButton,
                            Qt::NoModifier);
    QApplication::sendEvent(&knob, &doubleClick);
    CHECK_NEAR(knob.value(), 0.0, 1e-9);
}

// Without a default set, a double-click must do nothing rather than snap to
// whatever zero-initialised member happened to be there.
void testDoubleClickWithoutADefaultDoesNothing() {
    Knob knob;
    knob.setRange(20.0, 20000.0);
    knob.setTaper(Knob::Taper::Logarithmic);
    knob.setValue(440.0);

    const QPointF centre(knob.width() / 2.0, knob.height() / 2.0);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, centre,
                            knob.mapToGlobal(centre.toPoint()), Qt::LeftButton, Qt::LeftButton,
                            Qt::NoModifier);
    QApplication::sendEvent(&knob, &doubleClick);
    CHECK_NEAR(knob.value(), 440.0, 1e-9);
}

void testArrowKeysStepLikeTheWheel() {
    Knob knob;
    knob.setRange(0.0, 64.0);
    knob.setValue(0.0);

    sendKey(&knob, Qt::Key_Up);
    CHECK_NEAR(knob.value(), 1.0, 1e-6);
    sendKey(&knob, Qt::Key_Down);
    CHECK_NEAR(knob.value(), 0.0, 1e-6);
    sendKey(&knob, Qt::Key_PageUp);
    CHECK_NEAR(knob.value(), 8.0, 1e-6);
}

// ------------------------------------------------------------------ signals --

// The store needs a begin/end pair around a drag so a snapshot arriving mid-drag
// does not overwrite the value under the cursor. An unbalanced pair is worse
// than none: a begin with no end leaves the control permanently held.
void testDragEmitsABalancedEditPair() {
    Knob knob;
    knob.setRange(0.0, 1.0);

    int began = 0;
    int finished = 0;
    int changes = 0;
    QObject::connect(&knob, &Knob::editBegan, [&] { ++began; });
    QObject::connect(&knob, &Knob::editFinished, [&] { ++finished; });
    QObject::connect(&knob, &Knob::valueChanging, [&](double) { ++changes; });

    sendDrag(&knob, 60);
    CHECK_EQ(began, 1);
    CHECK_EQ(finished, 1);
    CHECK_EQ(changes, 1);

    // A wheel notch is a discrete change, not a gesture: it reports the new
    // value and opens no edit that would then have to be closed.
    sendWheel(&knob, 1);
    CHECK_EQ(began, 1);
    CHECK_EQ(finished, 1);
    CHECK_EQ(changes, 2);
}

// A double-click arrives as press, release, double-click, release. The extra
// release must not emit a second editFinished for an edit that was already
// closed - which is how a hold ends up released one time too many.
void testDoubleClickLeavesTheEditPairBalanced() {
    Knob knob;
    knob.setRange(-24.0, 24.0);
    knob.setDefaultValue(0.0);
    knob.setValue(6.0);

    int began = 0;
    int finished = 0;
    QObject::connect(&knob, &Knob::editBegan, [&] { ++began; });
    QObject::connect(&knob, &Knob::editFinished, [&] { ++finished; });

    const QPointF centre(knob.width() / 2.0, knob.height() / 2.0);
    const QPoint global = knob.mapToGlobal(centre.toPoint());
    const auto send = [&](QEvent::Type type, Qt::MouseButton pressed) {
        QMouseEvent event(type, centre, global, Qt::LeftButton, pressed, Qt::NoModifier);
        QApplication::sendEvent(&knob, &event);
    };
    send(QEvent::MouseButtonPress, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, Qt::NoButton);
    send(QEvent::MouseButtonDblClick, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, Qt::NoButton);

    CHECK_EQ(began, finished);
    CHECK_NEAR(knob.value(), 0.0, 1e-9);
}

// setValue is for pushing a value in from outside. If it emitted, selecting a
// band would look exactly like the user having just edited it, and the editor
// would write the value straight back to the daemon.
// A double-click is its own gesture: the release before it closed the previous
// edit, so it must open one - and it must leave the knob grabbed, or a
// double-click followed by a drag does nothing at all.
void testDoubleClickOpensAnEditAndLeavesTheKnobGrabbed() {
    Knob knob;
    knob.setRange(-24.0, 24.0);
    knob.setDefaultValue(0.0);
    knob.setValue(-12.0);

    int began = 0;
    int finished = 0;
    QObject::connect(&knob, &Knob::editBegan, [&] { ++began; });
    QObject::connect(&knob, &Knob::editFinished, [&] { ++finished; });

    const QPointF centre(knob.width() / 2.0, knob.height() / 2.0);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, centre,
                             knob.mapToGlobal(centre.toPoint()), Qt::LeftButton, Qt::LeftButton,
                             Qt::NoModifier);
    QApplication::sendEvent(&knob, &doubleClick);
    CHECK_EQ(began, 1);
    CHECK_EQ(finished, 0);
    CHECK_NEAR(knob.value(), 0.0, 1e-9);

    // Still grabbed: dragging on from the reset value works without letting go.
    const QPointF moved(centre.x(), centre.y() - 60);
    QMouseEvent move(QEvent::MouseMove, moved, knob.mapToGlobal(moved.toPoint()), Qt::NoButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&knob, &move);
    CHECK_NEAR(knob.value(), 12.0, 1e-6); // a quarter sweep up from 0 dB

    QMouseEvent release(QEvent::MouseButtonRelease, moved, knob.mapToGlobal(moved.toPoint()),
                         Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&knob, &release);
    CHECK_EQ(finished, 1);
}

void testSetValueDoesNotEmit() {
    Knob knob;
    knob.setRange(0.0, 100.0);
    int changes = 0;
    QObject::connect(&knob, &Knob::valueChanging, [&](double) { ++changes; });

    knob.setValue(42.0);
    CHECK_EQ(changes, 0);
    CHECK_NEAR(knob.value(), 42.0, 1e-9);
}

// ---------------------------------------------------------------- KnobField --

void testFieldAndKnobStayInStep() {
    KnobField control("Freq");
    control.setRange(20.0, 20000.0);
    control.setDecimals(1);
    control.setTaper(Knob::Taper::Logarithmic);

    int edits = 0;
    double lastEdit = 0.0;
    QObject::connect(&control, &KnobField::valueEdited, [&](double value) {
        ++edits;
        lastEdit = value;
    });

    // Pushing a value in updates both halves and emits nothing.
    control.setValue(440.0);
    CHECK_EQ(edits, 0);
    CHECK_NEAR(control.value(), 440.0, 0.05);

    // Turning the knob reports the change and the field follows it.
    auto* knob = control.findChild<Knob*>();
    CHECK(knob != nullptr);
    if (knob) {
        sendWheel(knob, 1);
        CHECK_EQ(edits, 1);
        CHECK(lastEdit > 440.0);
        // The field is the readout, so it must agree with the knob - within the
        // rounding its own decimals impose.
        CHECK_NEAR(control.value(), knob->value(), 0.05);
    }
}

void testTypingIntoTheFieldMovesTheKnob() {
    KnobField control("Gain");
    control.setRange(-24.0, 24.0);
    control.setDecimals(2);
    control.setValue(0.0);

    int edits = 0;
    QObject::connect(&control, &KnobField::valueEdited, [&](double) { ++edits; });

    auto* field = control.findChild<QDoubleSpinBox*>();
    auto* knob = control.findChild<Knob*>();
    CHECK(field != nullptr);
    CHECK(knob != nullptr);
    if (!field || !knob) {
        return;
    }
    field->setValue(-7.5);
    CHECK_EQ(edits, 1);
    CHECK_NEAR(knob->value(), -7.5, 1e-9);
}

// The two halves must not feed each other: knob -> field -> knob would emit
// twice per notch, and with rounding in between could oscillate.
void testOneGestureEmitsOnce() {
    KnobField control("Q");
    control.setRange(0.1, 10.0);
    control.setDecimals(3);
    control.setTaper(Knob::Taper::Logarithmic);
    control.setValue(0.707);

    int edits = 0;
    QObject::connect(&control, &KnobField::valueEdited, [&](double) { ++edits; });

    auto* knob = control.findChild<Knob*>();
    CHECK(knob != nullptr);
    if (knob) {
        sendWheel(knob, 1);
        CHECK_EQ(edits, 1);
    }
}

} // namespace

int main(int argc, char** argv) {
    // Offscreen: the events here are constructed and delivered directly, so no
    // display is needed and none should be required of a test run.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    RUN(testLogarithmicTaperIsRatiometric);
    RUN(testLinearTaperIsProportional);
    RUN(testLogarithmicTaperWithANonPositiveMinimumFallsBackToLinear);

    RUN(testDragIsMeasuredFromWhereItStarted);
    RUN(testManyMovesInOneDragDoNotDrift);
    RUN(testShiftDragIsFiner);
    RUN(testWheelStepsAndIsSymmetric);
    RUN(testWheelUsesTheTaper);
    RUN(testValueIsClampedAtBothEnds);
    RUN(testDoubleClickReturnsToTheDefault);
    RUN(testDoubleClickWithoutADefaultDoesNothing);
    RUN(testArrowKeysStepLikeTheWheel);

    RUN(testDragEmitsABalancedEditPair);
    RUN(testDoubleClickLeavesTheEditPairBalanced);
    RUN(testDoubleClickOpensAnEditAndLeavesTheKnobGrabbed);
    RUN(testSetValueDoesNotEmit);

    RUN(testFieldAndKnobStayInStep);
    RUN(testTypingIntoTheFieldMovesTheKnob);
    RUN(testOneGestureEmitsOnce);

    return pipeeq::test::summary("knob");
}
