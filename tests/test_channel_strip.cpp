// Mouse behaviour of a mixer strip.
//
// This is the part of the UI a user touches most, and it is exactly the kind of
// logic that looks right in a screenshot while being wrong in the hand: whether
// a press lands on the fader or the meter, and whether a press is a click or the
// start of a drag, is invisible in a still image.
//
// Every gesture is a real QEvent delivered to the widget, aimed at the widget's
// own reported rectangles rather than at guessed coordinates.

#include <QApplication>
#include <QMouseEvent>
#include <QVector>

#include "check.h"
#include "model/level_meters.h"
#include "widgets/channel_strip.h"
#include "widgets/fader_taper.h"

using pipeeq::ChannelStrip;
using pipeeq::LevelMeters;
using pipeeq::StripRow;

namespace {

// A strip laid out at a realistic size, so its rectangles are real.
struct Fixture {
    LevelMeters meters;
    ChannelStrip strip{&meters};

    Fixture() {
        StripRow row;
        row.id = "output-1#0";
        row.outputId = "output-1";
        row.channelIndex = 0;
        row.position = "FL";
        row.channelName = "Mains";
        row.deviceName = "test-device";
        row.outputName = "Test";
        row.connected = true;
        row.driven = true;
        row.gainDb = 0.0;
        strip.setStrips({row});
        strip.resize(strip.sizeHint().width(), 300);
    }
};

// A counting observer, in place of QSignalSpy - which lives in Qt6::Test, and
// this project deliberately carries no test framework.
struct Count {
    int n = 0;
    QVector<double> values;

    void hook(ChannelStrip* strip, void (ChannelStrip::*signal)()) {
        QObject::connect(strip, signal, strip, [this] { ++n; });
    }
    void hook(ChannelStrip* strip, void (ChannelStrip::*signal)(double)) {
        QObject::connect(strip, signal, strip, [this](double value) {
            ++n;
            values.push_back(value);
        });
    }
};

void press(ChannelStrip& strip, QPoint at) {
    QMouseEvent event(QEvent::MouseButtonPress, at, strip.mapToGlobal(at), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&strip, &event);
}

void move(ChannelStrip& strip, QPoint to) {
    QMouseEvent event(QEvent::MouseMove, to, strip.mapToGlobal(to), Qt::NoButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&strip, &event);
}

void release(ChannelStrip& strip, QPoint at) {
    QMouseEvent event(QEvent::MouseButtonRelease, at, strip.mapToGlobal(at), Qt::LeftButton,
                      Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&strip, &event);
}

// A point well inside the fader track, away from the value currently there.
QPoint inFader(const ChannelStrip& strip) {
    const QRect fader = strip.faderRect();
    return QPoint(fader.center().x(), fader.top() + fader.height() * 3 / 4);
}

QPoint inMeter(const ChannelStrip& strip) {
    const QRect meter = strip.meterAreaRect();
    return QPoint(meter.center().x(), meter.top() + meter.height() / 2);
}

// ------------------------------------------------------- the two are distinct --

// If these overlapped, every assertion below would be meaningless.
void testTheFaderAndTheMeterDoNotOverlap() {
    Fixture f;
    CHECK(f.strip.faderRect().width() > 0);
    CHECK(f.strip.meterAreaRect().width() > 0);
    CHECK(!f.strip.faderRect().intersects(f.strip.meterAreaRect()));
    CHECK(f.strip.faderRect().contains(inFader(f.strip)));
    CHECK(f.strip.meterAreaRect().contains(inMeter(f.strip)));
}

// ----------------------------------------------------------- the meter is not --

// The whole reported complaint: selecting a strip by clicking it moved the gain,
// because the meter counted as fader. The meter is the one part of a strip that
// looks like a readout rather than a control, and it is most of its width.
void testPressingTheMeterSelectsButDoesNotTouchTheGain() {
    Fixture f;
    f.strip.setSelected(true);
    Count selected;
    selected.hook(&f.strip, &ChannelStrip::selectRequested);
    Count began;
    began.hook(&f.strip, &ChannelStrip::gainEditBegan);
    Count changing;
    changing.hook(&f.strip, &ChannelStrip::gainChanging);

    press(f.strip, inMeter(f.strip));
    release(f.strip, inMeter(f.strip));

    CHECK_EQ(selected.n, 1);
    CHECK_EQ(began.n, 0);
    CHECK_EQ(changing.n, 0);
}

// Nor by dragging across it: a drag that starts on the meter is not a fader
// gesture at all.
void testDraggingFromTheMeterDoesNotMoveTheFader() {
    Fixture f;
    f.strip.setSelected(true);
    Count changing;
    changing.hook(&f.strip, &ChannelStrip::gainChanging);

    const QPoint start = inMeter(f.strip);
    press(f.strip, start);
    move(f.strip, QPoint(start.x(), start.y() - 60));
    release(f.strip, QPoint(start.x(), start.y() - 60));

    CHECK_EQ(changing.n, 0);
}

// ------------------------------------------- select first, then adjust --

void testFirstClickOnAnUnselectedStripOnlySelects() {
    Fixture f;
    f.strip.setSelected(false);
    Count selected;
    selected.hook(&f.strip, &ChannelStrip::selectRequested);
    Count began;
    began.hook(&f.strip, &ChannelStrip::gainEditBegan);
    Count changing;
    changing.hook(&f.strip, &ChannelStrip::gainChanging);

    const QPoint at = inFader(f.strip);
    press(f.strip, at);
    release(f.strip, at);

    CHECK_EQ(selected.n, 1);
    CHECK_EQ(began.n, 0);
    CHECK_EQ(changing.n, 0);
}

void testClickOnAnAlreadySelectedStripJumpsTheFader() {
    Fixture f;
    f.strip.setSelected(true);
    Count began;
    began.hook(&f.strip, &ChannelStrip::gainEditBegan);
    Count finished;
    finished.hook(&f.strip, &ChannelStrip::gainEditFinished);
    Count changing;
    changing.hook(&f.strip, &ChannelStrip::gainChanging);

    const QPoint at = inFader(f.strip);
    press(f.strip, at);
    CHECK_EQ(began.n, 1);
    CHECK_EQ(changing.n, 1);
    release(f.strip, at);
    CHECK_EQ(finished.n, 1);

    // Three quarters of the way down the track is well below unity.
    const double reported = changing.values.value(0);
    CHECK(reported < pipeeq::taper::kUnityDb);
}

// A drag says plainly what it means, so it takes effect on the first touch even
// though the strip was not selected when the press landed.
void testDraggingAnUnselectedStripMovesItOnTheFirstTouch() {
    Fixture f;
    f.strip.setSelected(false);
    Count selected;
    selected.hook(&f.strip, &ChannelStrip::selectRequested);
    Count began;
    began.hook(&f.strip, &ChannelStrip::gainEditBegan);
    Count changing;
    changing.hook(&f.strip, &ChannelStrip::gainChanging);
    Count finished;
    finished.hook(&f.strip, &ChannelStrip::gainEditFinished);

    const QPoint at = inFader(f.strip);
    press(f.strip, at);
    CHECK_EQ(began.n, 0); // nothing yet: this could still be a click

    move(f.strip, QPoint(at.x(), at.y() - 40));
    CHECK_EQ(began.n, 1);
    CHECK(changing.n >= 1);

    release(f.strip, QPoint(at.x(), at.y() - 40));
    CHECK_EQ(finished.n, 1);
    CHECK_EQ(selected.n, 1);
    // And the edit pair is balanced, or the store would hold the control
    // forever.
    CHECK_EQ(began.n, finished.n);
}

// A press with a pixel of hand tremor is a click, not a drag.
void testTinyMovementIsStillAClick() {
    Fixture f;
    f.strip.setSelected(false);
    Count began;
    began.hook(&f.strip, &ChannelStrip::gainEditBegan);

    const QPoint at = inFader(f.strip);
    press(f.strip, at);
    move(f.strip, QPoint(at.x() + 1, at.y() - 1));
    CHECK_EQ(began.n, 0);
    release(f.strip, QPoint(at.x() + 1, at.y() - 1));
    CHECK_EQ(began.n, 0);
}

// An armed press that was released must not linger: a move afterwards is not
// part of any gesture, and treating it as one would move the fader with no
// button held.
void testMovingAfterAReleaseDoesNothing() {
    Fixture f;
    f.strip.setSelected(false);
    Count changing;
    changing.hook(&f.strip, &ChannelStrip::gainChanging);

    const QPoint at = inFader(f.strip);
    press(f.strip, at);
    release(f.strip, at);
    move(f.strip, QPoint(at.x(), at.y() - 80));

    CHECK_EQ(changing.n, 0);
}

// Once selected, the strip behaves as before - the arming is a one-time cost of
// arriving at a strip, not a permanent extra click.
void testSecondClickAfterSelectionAdjusts() {
    Fixture f;
    f.strip.setSelected(false);

    const QPoint at = inFader(f.strip);
    press(f.strip, at);
    release(f.strip, at);

    // What the rack does in response to selectRequested.
    f.strip.setSelected(true);

    Count changing;
    changing.hook(&f.strip, &ChannelStrip::gainChanging);
    press(f.strip, at);
    CHECK_EQ(changing.n, 1);
    release(f.strip, at);
}

// ------------------------------------------------------------ width scaling --

void testWidthScaleWidensTheStripAndItsFader() {
    Fixture f;
    const int naturalWidth = f.strip.sizeHint().width();
    const int naturalFader = f.strip.faderRect().width();
    CHECK_EQ(naturalWidth, f.strip.naturalWidth());

    f.strip.setWidthScale(2.0);
    f.strip.resize(f.strip.sizeHint().width(), 300);
    CHECK_EQ(f.strip.sizeHint().width(), naturalWidth * 2);
    // The fader widens with the strip: the proportions are what make it read as
    // a fader, and a wider grip is easier to hit.
    CHECK(f.strip.faderRect().width() > naturalFader);
    CHECK(!f.strip.faderRect().intersects(f.strip.meterAreaRect()));
}

void testWidthScaleIsClamped() {
    Fixture f;
    const int naturalWidth = f.strip.naturalWidth();

    // Never narrower than natural, however little room there is.
    f.strip.setWidthScale(0.25);
    CHECK_EQ(f.strip.sizeHint().width(), naturalWidth);

    // And never wider than the cap, however much.
    f.strip.setWidthScale(12.0);
    CHECK_EQ(f.strip.sizeHint().width(),
             static_cast<int>(naturalWidth * ChannelStrip::maxWidthScale()));
}

// The hit-testing has to follow the scale, or a widened strip would take
// presses at the fader's old coordinates.
void testHitTestingFollowsTheWidthScale() {
    Fixture f;
    f.strip.setWidthScale(2.0);
    f.strip.resize(f.strip.sizeHint().width(), 300);
    f.strip.setSelected(true);

    Count changing;
    changing.hook(&f.strip, &ChannelStrip::gainChanging);
    // A point that is inside the meter at scale 1 and still inside the meter
    // here - it must not move the gain either way.
    press(f.strip, inMeter(f.strip));
    release(f.strip, inMeter(f.strip));
    CHECK_EQ(changing.n, 0);

    press(f.strip, inFader(f.strip));
    CHECK_EQ(changing.n, 1);
    release(f.strip, inFader(f.strip));
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    RUN(testTheFaderAndTheMeterDoNotOverlap);

    RUN(testPressingTheMeterSelectsButDoesNotTouchTheGain);
    RUN(testDraggingFromTheMeterDoesNotMoveTheFader);

    RUN(testFirstClickOnAnUnselectedStripOnlySelects);
    RUN(testClickOnAnAlreadySelectedStripJumpsTheFader);
    RUN(testDraggingAnUnselectedStripMovesItOnTheFirstTouch);
    RUN(testTinyMovementIsStillAClick);
    RUN(testMovingAfterAReleaseDoesNothing);
    RUN(testSecondClickAfterSelectionAdjusts);

    RUN(testWidthScaleWidensTheStripAndItsFader);
    RUN(testWidthScaleIsClamped);
    RUN(testHitTestingFollowsTheWidthScale);

    return pipeeq::test::summary("channel_strip");
}
