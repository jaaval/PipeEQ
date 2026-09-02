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
#include "widgets/send_strip.h"
#include "widgets/strip_metrics.h"

using pipeeq::ChannelStrip;
using pipeeq::LevelMeters;
using pipeeq::StripRow;

namespace {

// A strip laid out at a realistic size.
//
// The resize comes FIRST, and that ordering is load-bearing: a widget that has
// never been shown receives no resize event, so the layout is only ever
// computed by setStrips - and if the resize follows it, everything is laid out
// at QWidget's default 640x480 while the widget is 66 px wide. That is not
// hypothetical, it is what this fixture did: the meter rectangle came out ten
// times wider than the strip, so every press aimed at a point outside it.
// testTheFaderAndTheMeterDoNotOverlap now asserts the layout fits the widget,
// which is what catches it.
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
        strip.resize(strip.sizeHint().width(), 300);
        strip.setStrips({row});
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

    // And the layout must fit the widget it belongs to. Without this the rects
    // can be computed for a completely different size and still satisfy
    // everything above - which is exactly what happened, at 640x480 in a 66 px
    // strip.
    CHECK(f.strip.faderRect().left() >= 0);
    CHECK(f.strip.faderRect().top() >= 0);
    CHECK(f.strip.meterAreaRect().right() < f.strip.width());
    CHECK(f.strip.meterAreaRect().bottom() < f.strip.height());
}

// ------------------------------------------------------ which way is louder --

// Up is louder. Unasserted until now: the only value check in this suite was
// "below unity", and unity sits at 80% of the travel, so an inverted fader -
// press low, get loud - produced -2.9 dB where it should produce -30 dB and
// passed. A strip that ran backwards would have shipped.
void testDraggingUpRaisesTheGain() {
    Fixture f;
    f.strip.setSelected(true);
    Count changing;
    changing.hook(&f.strip, &ChannelStrip::gainChanging);

    const QRect fader = f.strip.faderRect();
    press(f.strip, QPoint(fader.center().x(), fader.bottom() - 1));
    release(f.strip, QPoint(fader.center().x(), fader.bottom() - 1));
    const double atBottom = changing.values.value(0);

    press(f.strip, QPoint(fader.center().x(), fader.top() + 1));
    release(f.strip, QPoint(fader.center().x(), fader.top() + 1));
    const double atTop = changing.values.value(1);

    CHECK(atTop > atBottom);
    CHECK(atTop > pipeeq::taper::kMaxDb - 1.5);
    CHECK(pipeeq::taper::isSilent(atBottom) || atBottom <= pipeeq::taper::kMinDb + 1.5);

    // And the ends EXACTLY, which is stronger than the mapping check below
    // because it does not restate the formula: the first pixel of the track is
    // maximum and the last is silence. It was not so - the divisor was the
    // track's height rather than its travel, so the topmost pixel came out
    // ~0.25 dB short and maximum was only reachable by dragging off the edge.
    press(f.strip, QPoint(fader.center().x(), fader.top()));
    release(f.strip, QPoint(fader.center().x(), fader.top()));
    CHECK_NEAR(changing.values.value(2), pipeeq::taper::kMaxDb, 1e-9);

    press(f.strip, QPoint(fader.center().x(), fader.bottom()));
    release(f.strip, QPoint(fader.center().x(), fader.bottom()));
    CHECK(pipeeq::taper::isSilent(changing.values.value(3)));
}

// The whole mapping, not only its direction: a press lands on the value the
// shared taper puts at that fraction of the travel. This is what pins the
// pixel-to-dB arithmetic, including which edge it measures from.
void testPressMapsThroughTheSharedTaper() {
    Fixture f;
    f.strip.setSelected(true);
    Count changing;
    changing.hook(&f.strip, &ChannelStrip::gainChanging);

    const QRect fader = f.strip.faderRect();
    for (const double fraction : {0.25, 0.5, 0.75}) {
        const int y = static_cast<int>(fader.bottom() - fraction * fader.height());
        press(f.strip, QPoint(fader.center().x(), y));
        release(f.strip, QPoint(fader.center().x(), y));
        const double expected = pipeeq::taper::normToDb(
            static_cast<double>(fader.bottom() - y) / (fader.height() - 1));
        CHECK_NEAR(changing.values.value(changing.n - 1), expected, 1e-6);
    }
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

// ------------------------------------------------------- the send strips --

// A routed send strip, laid out at a realistic size.
struct SendFixture {
    LevelMeters meters;
    pipeeq::SendStrip strip{&meters};

    SendFixture() {
        strip.resize(strip.sizeHint().width(), 240);
        pipeeq::InputRow input;
        input.id = "input-1";
        input.displayName = "Music";
        strip.setCanRoute(true);
        strip.setSend(true, 0.0);
        strip.setInput(input);
    }
};

// The same rule as the mixer strips below, for the same reason - and it matters
// more here now that the two rows are deliberately the same width, because
// behaving differently on a meter press would read as a bug rather than as a
// distinction.
void testPressingASendMeterDoesNotMoveItsFader() {
    SendFixture f;
    int changes = 0;
    QObject::connect(&f.strip, &pipeeq::SendStrip::levelChanging, &f.strip,
                      [&](double) { ++changes; });

    const QRect meter = f.strip.meterAreaRect();
    const QPoint at(meter.center().x(), meter.top() + meter.height() / 2);
    // Both rects must be real, or the rest of this proves nothing: an empty
    // rect contains no point and intersects nothing, so every assertion here
    // would pass against a widget that had never computed a layout at all.
    CHECK(!f.strip.faderRect().isEmpty());
    CHECK(!meter.isEmpty());
    CHECK(!f.strip.faderRect().intersects(meter));

    QMouseEvent press(QEvent::MouseButtonPress, at, f.strip.mapToGlobal(at), Qt::LeftButton,
                       Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&f.strip, &press);
    CHECK_EQ(changes, 0);

    // But its own fader still acts at once: a send strip is never the thing
    // being selected, so there is no first click to spend on selecting it.
    const QRect fader = f.strip.faderRect();
    const QPoint onFader(fader.center().x(), fader.top() + fader.height() * 3 / 4);
    QMouseEvent onIt(QEvent::MouseButtonPress, onFader, f.strip.mapToGlobal(onFader),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&f.strip, &onIt);
    CHECK_EQ(changes, 1);
}

// Up is louder here too. Same gap, same reason: nothing in this suite looked
// at a send's value, only at whether one was emitted.
void testDraggingASendUpRaisesItsLevel() {
    SendFixture f;
    QVector<double> values;
    QObject::connect(&f.strip, &pipeeq::SendStrip::levelChanging, &f.strip,
                      [&](double value) { values.push_back(value); });

    const QRect fader = f.strip.faderRect();
    const QPoint low(fader.center().x(), fader.bottom() - 1);
    const QPoint high(fader.center().x(), fader.top() + 1);

    QMouseEvent atLow(QEvent::MouseButtonPress, low, f.strip.mapToGlobal(low), Qt::LeftButton,
                       Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&f.strip, &atLow);
    QMouseEvent up(QEvent::MouseButtonRelease, low, f.strip.mapToGlobal(low), Qt::LeftButton,
                    Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&f.strip, &up);

    QMouseEvent atHigh(QEvent::MouseButtonPress, high, f.strip.mapToGlobal(high), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&f.strip, &atHigh);

    CHECK_EQ(values.size(), 2);
    if (values.size() == 2) {
        CHECK(values.at(1) > values.at(0));
        CHECK(values.at(1) > pipeeq::taper::kMaxDb - 1.5);
        CHECK(pipeeq::taper::isSilent(values.at(0)) ||
              values.at(0) <= pipeeq::taper::kMinDb + 1.5);
    }
}

// The two rows are the same width by construction, not by two constants that
// happen to agree.
void testSendAndMixerStripsShareABaseWidth() {
    Fixture mixer;
    CHECK_EQ(pipeeq::SendStrip::naturalWidth(), mixer.strip.naturalWidth());
}

// -------------------------------------------- the sends / EQ width split --

// The panel's own margins, as the detail panel passes them.
constexpr int kMargins = 16;

// How wide the EQ ends up, given a plan.
int eqWidth(int panelWidth, const pipeeq::strip::SendsPlan& plan) {
    return panelWidth - kMargins - pipeeq::strip::kBodySpacing - plan.width;
}

// The stated design, asserted as numbers rather than by reference to itself.
//
// Every other check here compares against the constants, so it passes whatever
// they hold: "up to double" is satisfied by a cap of 5, and "the EQ keeps 40%"
// by a share of 5%. This is the one place the intended values are written down
// twice on purpose, so changing one of them has to be deliberate.
void testTheStatedMetricsAreWhatWasAskedFor() {
    CHECK_NEAR(pipeeq::strip::kMaxWidthScale, 2.0, 1e-9);       // "up to double"
    CHECK_NEAR(pipeeq::strip::kEqMinimumShare, 0.40, 1e-9);     // "at least 40%"
    CHECK_EQ(pipeeq::SendStrip::naturalWidth(), pipeeq::strip::kBaseWidth);
}

// The default: the sends match the mixer strips below.
void testSendsMatchTheMixerStripsWhenThereIsRoom() {
    const auto plan = pipeeq::strip::planSends(1904, kMargins, 4, 300, 2.0);
    CHECK_NEAR(plan.scale, 2.0, 1e-9);
    // Four strips at double width, plus the spacing between them.
    CHECK_EQ(plan.width, 4 * 2 * pipeeq::strip::kBaseWidth + 3 * pipeeq::strip::kSendSpacing);
    // And the curve is left with far more than its floor.
    CHECK(eqWidth(1904, plan) > 1904 / 2);
}

// The rule that motivated all of this: sends grow with the number of inputs,
// the EQ does not, so at some point the sends have to give way.
//
// Asserted as an EXACT scale and width, not as an inequality. "Less than
// double and at least natural" is satisfied by collapsing straight back to
// natural width the instant any reduction is needed - which throws away the
// whole point of the rule, that the sends take the requested scale as far as
// what is left allows. It is also satisfied by an off-by-one in the spacing
// term, because over-counting spacing hands the EQ more room and the floor is
// one-sided. Both were live: neither was caught before this.
void testSendsNarrowRatherThanCrowdingTheEq() {
    const auto plan = pipeeq::strip::planSends(1000, kMargins, 8, 300, 2.0);

    // body = 1000 - 16 - 10 = 974; the EQ's 40% floor is 400, so the sends may
    // have 574. Eight strips carry seven gaps, so each gets (574 - 35) / 8 px.
    const int perStrip = (574 - 7 * pipeeq::strip::kSendSpacing) / 8;
    CHECK_NEAR(plan.scale, static_cast<double>(perStrip) / pipeeq::strip::kBaseWidth, 1e-9);
    CHECK(plan.scale > 1.0); // it really is scaling, not falling back
    CHECK(plan.scale < 2.0);
    CHECK_EQ(plan.width, 8 * perStrip + 7 * pipeeq::strip::kSendSpacing);
    CHECK(eqWidth(1000, plan) >= static_cast<int>(1000 * pipeeq::strip::kEqMinimumShare));
}

// Once even the natural width will not fit, the strips stop shrinking - the row
// scrolls instead, which it already knows how to do. Shrinking below natural
// would trade a scrollbar for an illegible fader.
void testSendsNeverShrinkBelowTheirNaturalWidth() {
    const auto plan = pipeeq::strip::planSends(940, kMargins, 12, 300, 2.0);
    CHECK_NEAR(plan.scale, 1.0, 1e-9);
    // body = 940 - 16 - 10 = 914, the EQ floor is 376, so the sends get 538 -
    // less than twelve natural strips need, and the column is capped there so
    // the row scrolls. Asserted exactly, so a cap that quietly handed the EQ
    // less than its floor would show up here.
    CHECK_EQ(plan.width, 914 - 376);
    CHECK(eqWidth(940, plan) >= static_cast<int>(940 * pipeeq::strip::kEqMinimumShare));
}

// A request below natural or above the cap is not honoured either way.
void testRequestedScaleIsClamped() {
    CHECK_NEAR(pipeeq::strip::planSends(1904, kMargins, 2, 300, 0.3).scale, 1.0, 1e-9);
    CHECK_NEAR(pipeeq::strip::planSends(1904, kMargins, 2, 300, 9.0).scale,
                pipeeq::strip::kMaxWidthScale, 1e-9);
}

// With no inputs the column still has to hold its header.
void testHeaderIsAFloorOnTheColumnWidth() {
    const auto plan = pipeeq::strip::planSends(1904, kMargins, 0, 300, 2.0);
    CHECK_EQ(plan.width, 300);
    const auto oneStrip = pipeeq::strip::planSends(1904, kMargins, 1, 300, 2.0);
    CHECK_EQ(oneStrip.width, 300); // one strip is narrower than the header
}

// The invariants, swept rather than sampled: across every window width the
// application permits and every send count it allows, the EQ keeps its share,
// the scale stays within bounds, and - the one that was missing - the strips
// FIT the column the same plan sizes for them.
//
// That last one was violated for real. The scale was derived from exact
// fractional widths while each strip rounds its own width half up, so a few
// strips overran the column by 1-3 px, the column was clamped to what the EQ
// could spare, and the row grew the horizontal scrollbar this rule exists to
// avoid - taking ~15 px off the strips' height with it, in precisely the
// regime where the EQ's floor is binding.
void testEqKeepsItsShareAcrossEveryWindowAndInputCount() {
    int atFullScale = 0;
    int partiallyScaled = 0;
    int atNaturalWidth = 0;

    for (int panelWidth = 940; panelWidth <= 3840; panelWidth += 37) {
        for (int sends = 1; sends <= 12; ++sends) {
            const auto plan = pipeeq::strip::planSends(panelWidth, kMargins, sends, 300, 2.0);

            const int floorWidth = static_cast<int>(panelWidth * pipeeq::strip::kEqMinimumShare);
            if (eqWidth(panelWidth, plan) < floorWidth) {
                CHECK_EQ(eqWidth(panelWidth, plan), floorWidth); // reports the offending size
                return;
            }
            if (plan.scale < 1.0 || plan.scale > pipeeq::strip::kMaxWidthScale) {
                CHECK_NEAR(plan.scale, 1.0, 0.0); // ditto
                return;
            }

            // Rounded exactly as SendStrip::scaledWidth() rounds it.
            const int perStrip =
                static_cast<int>(pipeeq::strip::kBaseWidth * plan.scale + 0.5);
            const int stripsWidth =
                sends * perStrip + pipeeq::strip::kSendSpacing * (sends - 1);
            // Only where the column is sized by its strips; where the header or
            // the hard floor sets the width, the row scrolls by design.
            if (stripsWidth > plan.width && plan.scale > 1.0) {
                CHECK_EQ(stripsWidth, plan.width); // reports the offending size
                return;
            }

            if (plan.scale >= pipeeq::strip::kMaxWidthScale) {
                ++atFullScale;
            } else if (plan.scale > 1.0) {
                ++partiallyScaled;
            } else {
                ++atNaturalWidth;
            }
        }
    }

    // A sweep that never reaches the boundary proves nothing about it. All
    // three regimes have to appear, or the invariants above were only ever
    // checked where they could not have been violated.
    CHECK(atFullScale > 0);
    CHECK(partiallyScaled > 0);
    CHECK(atNaturalWidth > 0);
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

    RUN(testDraggingUpRaisesTheGain);
    RUN(testPressMapsThroughTheSharedTaper);

    RUN(testPressingASendMeterDoesNotMoveItsFader);
    RUN(testDraggingASendUpRaisesItsLevel);
    RUN(testSendAndMixerStripsShareABaseWidth);

    RUN(testTheStatedMetricsAreWhatWasAskedFor);
    RUN(testSendsMatchTheMixerStripsWhenThereIsRoom);
    RUN(testSendsNarrowRatherThanCrowdingTheEq);
    RUN(testSendsNeverShrinkBelowTheirNaturalWidth);
    RUN(testRequestedScaleIsClamped);
    RUN(testHeaderIsAFloorOnTheColumnWidth);
    RUN(testEqKeepsItsShareAcrossEveryWindowAndInputCount);

    return pipeeq::test::summary("channel_strip");
}
