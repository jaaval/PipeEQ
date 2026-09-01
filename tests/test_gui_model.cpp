// The GUI store's pure logic: write coalescing, edit guarding, meter
// ballistics.
//
// These are the pieces that fix the D-Bus write storm and the
// "don't clobber the fader the user is dragging" problem, so they are asserted
// here rather than only observed with dbus-monitor.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>

#include "model/edit_guard.h"
#include "model/level_meters.h"
#include "model/write_coalescer.h"
#include "widgets/fader_taper.h"

#include "check.h"

namespace {

using namespace pipeeq;

// Runs the event loop for `ms`, so timer-driven behaviour actually fires.
void pump(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    QCoreApplication::processEvents();
}

WriteOp gainOp(const QString& outputId, uint32_t channel, double gainDb) {
    WriteOp op;
    op.kind = WriteOp::Kind::ChannelGain;
    op.outputId = outputId;
    op.channelIndex = channel;
    op.doubleArg = gainDb;
    return op;
}

WriteOp muteOp(const QString& outputId, uint32_t channel, bool muted) {
    WriteOp op;
    op.kind = WriteOp::Kind::ChannelMute;
    op.outputId = outputId;
    op.channelIndex = channel;
    op.boolArg = muted;
    return op;
}

WriteOp bandCountOp(const QString& outputId, uint32_t channel, uint32_t count) {
    WriteOp op;
    op.kind = WriteOp::Kind::ChannelEqBandCount;
    op.outputId = outputId;
    op.channelIndex = channel;
    op.uintArg = count;
    return op;
}

// ------------------------------------------------------- write coalescing --

// The headline property: a drag's worth of intermediate values collapses to the
// newest one, and only the newest one is sent.
void testCoalescesToLatestValue() {
    WriteCoalescer coalescer;
    QVector<WriteOp> sent;
    QObject::connect(&coalescer, &WriteCoalescer::writesReady,
                     [&](const QVector<WriteOp>& ops) { sent.append(ops); });

    for (int i = 0; i < 200; ++i) {
        coalescer.enqueue(gainOp("output-1", 0, -60.0 + i * 0.3));
    }
    CHECK(sent.isEmpty()); // nothing goes out before the interval elapses

    pump(WriteCoalescer::kFlushIntervalMs * 3);

    CHECK_EQ(sent.size(), 1);
    if (!sent.isEmpty()) {
        // The LAST value, not the first: a fader that lands on the value it
        // started from would be worse than no coalescing at all.
        CHECK_NEAR(sent.front().doubleArg, -60.0 + 199 * 0.3, 1e-9);
    }
}

// 200 enqueues spread over 5 seconds must produce at most one write per flush
// interval - which is the ≤25/s bound the whole design rests on.
void testWriteRateIsBounded() {
    WriteCoalescer coalescer;
    int writes = 0;
    QObject::connect(&coalescer, &WriteCoalescer::writesReady,
                     [&](const QVector<WriteOp>& ops) { writes += ops.size(); });

    QElapsedTimer timer;
    timer.start();
    int enqueued = 0;
    // ~10 ms apart, i.e. faster than the flush interval, as a real drag is.
    while (timer.elapsed() < 1000) {
        coalescer.enqueue(gainOp("output-1", 0, -20.0 + enqueued * 0.01));
        ++enqueued;
        pump(10);
    }
    coalescer.flushNow();
    pump(WriteCoalescer::kFlushIntervalMs * 2);

    CHECK(enqueued > 50); // the test itself has to be a realistic drag
    // At 40 ms per flush, one second cannot produce more than ~25 writes plus
    // the forced final flush.
    CHECK(writes <= 27);
    // And it must not have swallowed everything.
    CHECK(writes >= 2);
}

// Different controls must not collapse into each other.
void testDistinctKeysDoNotCollapse() {
    WriteCoalescer coalescer;
    QVector<WriteOp> sent;
    QObject::connect(&coalescer, &WriteCoalescer::writesReady,
                     [&](const QVector<WriteOp>& ops) { sent.append(ops); });

    coalescer.enqueue(gainOp("output-1", 0, -3.0));
    coalescer.enqueue(gainOp("output-1", 1, -6.0));
    coalescer.enqueue(gainOp("output-2", 0, -9.0));
    pump(WriteCoalescer::kFlushIntervalMs * 3);

    CHECK_EQ(sent.size(), 3);
}

// Structural ops must keep their order and must never be collapsed: "set band
// count to 3" then "set band 2" is not interchangeable with the reverse.
void testOrderedOpsArePreservedAndNotCollapsed() {
    WriteCoalescer coalescer;
    QVector<WriteOp> sent;
    QObject::connect(&coalescer, &WriteCoalescer::writesReady,
                     [&](const QVector<WriteOp>& ops) { sent.append(ops); });

    coalescer.enqueue(muteOp("output-1", 0, true));
    coalescer.enqueue(muteOp("output-1", 0, false));
    coalescer.enqueue(bandCountOp("output-1", 0, 3));
    pump(WriteCoalescer::kFlushIntervalMs * 3);

    CHECK_EQ(sent.size(), 3);
    if (sent.size() == 3) {
        CHECK(sent[0].boolArg);
        CHECK(!sent[1].boolArg);
        CHECK_EQ(sent[2].uintArg, 3u);
    }
}

// A band-count change has to reach the daemon before the band values that
// depend on it, even when the band value was enqueued first.
void testStructuralOpsFlushBeforeCoalescedOnes() {
    WriteCoalescer coalescer;
    QVector<WriteOp> sent;
    QObject::connect(&coalescer, &WriteCoalescer::writesReady,
                     [&](const QVector<WriteOp>& ops) { sent.append(ops); });

    WriteOp band;
    band.kind = WriteOp::Kind::ChannelEqBand;
    band.outputId = "output-1";
    band.uintArg = 2;
    coalescer.enqueue(band);
    coalescer.enqueue(bandCountOp("output-1", 0, 3));
    pump(WriteCoalescer::kFlushIntervalMs * 3);

    CHECK_EQ(sent.size(), 2);
    if (sent.size() == 2) {
        CHECK(sent[0].kind == WriteOp::Kind::ChannelEqBandCount);
        CHECK(sent[1].kind == WriteOp::Kind::ChannelEqBand);
    }
}

// Releasing a control must land its final value immediately rather than waiting
// out the interval.
void testFlushNowSendsImmediately() {
    WriteCoalescer coalescer;
    int writes = 0;
    QObject::connect(&coalescer, &WriteCoalescer::writesReady,
                     [&](const QVector<WriteOp>& ops) { writes += ops.size(); });

    coalescer.enqueue(gainOp("output-1", 0, -1.5));
    coalescer.flushNow();
    CHECK_EQ(writes, 1); // no event loop turn needed
}

void testNoWritesWhenIdle() {
    WriteCoalescer coalescer;
    int writes = 0;
    QObject::connect(&coalescer, &WriteCoalescer::writesReady,
                     [&](const QVector<WriteOp>& ops) { writes += ops.size(); });
    pump(WriteCoalescer::kFlushIntervalMs * 4);
    CHECK_EQ(writes, 0);
    CHECK(!coalescer.hasPending());
}

// ------------------------------------------------------------ edit guarding --

void testHoldBlocksThenExpires() {
    EditGuard guard;
    const EditKey key{"output-1#0", Field::Gain, -1};

    CHECK(!guard.isHeld(key));
    guard.beginEdit(key);
    CHECK(guard.isHeld(key));
    pump(EditGuard::kGraceMs + 100);
    CHECK(guard.isHeld(key)); // still actively held; the grace hasn't started

    guard.endEdit(key);
    CHECK(guard.isHeld(key)); // grace period
    pump(EditGuard::kGraceMs + 120);
    CHECK(!guard.isHeld(key));
}

// The grace period is a guess about timing; the pending count is not. A write
// that hasn't come back keeps the hold alive however long it takes.
void testPendingWriteOutlastsTheGracePeriod() {
    EditGuard guard;
    const EditKey key{"output-1#0", Field::Gain, -1};

    guard.beginEdit(key);
    guard.noteWriteSent(key);
    guard.endEdit(key);
    pump(EditGuard::kGraceMs + 200);
    CHECK(guard.isHeld(key)); // still in flight

    guard.noteWriteCompleted(key);
    CHECK(!guard.isHeld(key));
}

// Fields are independent: dragging the fader must not also suppress a remote
// mute change on the same channel.
void testFieldsAreIndependent() {
    EditGuard guard;
    const EditKey gain{"output-1#0", Field::Gain, -1};
    const EditKey mute{"output-1#0", Field::Mute, -1};

    guard.beginEdit(gain);
    CHECK(guard.isHeld(gain));
    CHECK(!guard.isHeld(mute));
}

// ...and so are band indices, so dragging band 2 doesn't freeze band 0.
void testBandIndicesAreIndependent() {
    EditGuard guard;
    guard.beginEdit(EditKey{"output-1#0", Field::EqBand, 2});
    CHECK(guard.isHeld(EditKey{"output-1#0", Field::EqBand, 2}));
    CHECK(!guard.isHeld(EditKey{"output-1#0", Field::EqBand, 0}));
}

void testExpiredKeysAreReportedOnce() {
    EditGuard guard;
    const EditKey key{"output-1#0", Field::Gain, -1};

    guard.beginEdit(key);
    guard.endEdit(key);
    CHECK(guard.takeExpiredKeys().isEmpty()); // still in the grace period

    pump(EditGuard::kGraceMs + 120);
    const QVector<QString> expired = guard.takeExpiredKeys();
    CHECK_EQ(expired.size(), 1);
    if (!expired.isEmpty()) {
        CHECK_EQ(expired.front(), key.toString());
    }
    // Reported once, then forgotten.
    CHECK(guard.takeExpiredKeys().isEmpty());
}

// -------------------------------------------------------- meter ballistics --

// Tolerance for "the meter reached this level". A tick may have elapsed
// between the value landing and the assertion, and release costs
// kReleaseDbPerSecond * kTickMs per tick, so an exact match would make these
// tests timing-flaky rather than wrong.
constexpr double kOneTickDecayDb = LevelMeters::kReleaseDbPerSecond * LevelMeters::kTickMs / 1000.0;
constexpr double kAttackToleranceDb = 2.0 * kOneTickDecayDb + 0.2;

MeterRow meterRow(const QString& id, const QVector<double>& peaksDb) {
    MeterRow row;
    row.id = id;
    row.peaksDb = peaksDb;
    return row;
}

void testAttackIsInstantAndReleaseDecays() {
    LevelMeters meters;
    meters.setActive(true);

    meters.ingest({meterRow("output-1", {-6.0})}, {});
    pump(LevelMeters::kTickMs * 2);
    CHECK_NEAR(meters.levelDb("output-1", 0), -6.0, kAttackToleranceDb);

    // Nothing further ingested: it must fall, not hold.
    pump(300);
    const double decayed = meters.levelDb("output-1", 0);
    CHECK(decayed < -6.0);
    // ...at roughly the advertised rate.
    CHECK(decayed > -6.0 - LevelMeters::kReleaseDbPerSecond * 0.6);
}

// The reason ballistics live in the store: a stalled signal must fall to
// silence rather than freezing the meters at their last value.
void testStalledSignalDecaysToSilence() {
    LevelMeters meters;
    meters.setActive(true);
    meters.ingest({meterRow("output-1", {0.0})}, {});
    pump(LevelMeters::kTickMs * 2);

    pump(9000); // ~9 s of nothing at 20 dB/s covers the whole range
    CHECK_NEAR(meters.levelDb("output-1", 0), LevelMeters::kSilenceDb, 1.0);
}

// Frames arriving faster than the repaint tick are peak-combined, so a
// transient between two paints isn't lost.
void testFramesBetweenTicksArePeakCombined() {
    LevelMeters meters;
    meters.setActive(true);

    meters.ingest({meterRow("output-1", {-30.0})}, {});
    meters.ingest({meterRow("output-1", {-9.0})}, {});
    meters.ingest({meterRow("output-1", {-24.0})}, {});
    pump(LevelMeters::kTickMs * 2);

    CHECK_NEAR(meters.levelDb("output-1", 0), -9.0, kAttackToleranceDb);
}

void testClipLatchesUntilCleared() {
    LevelMeters meters;
    meters.setActive(true);

    meters.ingest({meterRow("output-1", {0.0})}, {});
    pump(LevelMeters::kTickMs * 2);
    CHECK(meters.clipped("output-1", 0));

    // A clip that stops flashing before anyone looks is useless, so quiet
    // audio afterwards must not clear it.
    meters.ingest({meterRow("output-1", {-40.0})}, {});
    pump(LevelMeters::kTickMs * 3);
    CHECK(meters.clipped("output-1", 0));

    meters.clearClip("output-1", 0);
    CHECK(!meters.clipped("output-1", 0));
}

void testPeakHoldLingersThenFollows() {
    LevelMeters meters;
    meters.setActive(true);

    meters.ingest({meterRow("output-1", {-6.0})}, {});
    pump(LevelMeters::kTickMs * 2);
    meters.ingest({meterRow("output-1", {-40.0})}, {});
    // Long enough for the release rate to actually move the level: at
    // 20 dB/s, three ticks is only ~2 dB, which would prove nothing.
    pump(600);

    // The level has dropped but the hold marker is still up near the peak.
    CHECK(meters.levelDb("output-1", 0) < -12.0);
    CHECK_NEAR(meters.holdDb("output-1", 0), -6.0, 1.0);

    pump(LevelMeters::kHoldMs + 200);
    CHECK(meters.holdDb("output-1", 0) < -10.0);
}

void testDisarmingResetsToSilence() {
    LevelMeters meters;
    meters.setActive(true);
    meters.ingest({meterRow("output-1", {-3.0})}, {});
    pump(LevelMeters::kTickMs * 2);
    CHECK(meters.levelDb("output-1", 0) > -10.0);

    meters.setActive(false);
    // Re-arming must not briefly show a stale level from before.
    CHECK_NEAR(meters.levelDb("output-1", 0), LevelMeters::kSilenceDb, 0.001);
    CHECK(!meters.isActive());
}

void testUnknownChannelReadsAsSilence() {
    LevelMeters meters;
    CHECK_NEAR(meters.levelDb("nope", 0), LevelMeters::kSilenceDb, 0.001);
    CHECK(!meters.clipped("nope", 3));
}

// ---------------------------------------------------------------- the taper --

// The fader and the meter share this mapping so that 0 dB lands at the same
// height on both. If it stops round-tripping, one of them silently misreads the
// other's scale.
void testTaperRoundTrips() {
    for (double db : {-65.0, -50.0, -40.0, -30.0, -20.0, -12.0, -6.0, -3.0, 0.0, 3.0, 6.0, 12.0}) {
        CHECK_NEAR(taper::normToDb(taper::dbToNorm(db)), db, 0.01);
    }
    for (double norm = 0.05; norm <= 1.0; norm += 0.05) {
        CHECK_NEAR(taper::dbToNorm(taper::normToDb(norm)), norm, 0.01);
    }
}

void testTaperIsMonotonic() {
    double previous = -1.0;
    for (double db = taper::kMinDb; db <= taper::kMaxDb; db += 0.25) {
        const double norm = taper::dbToNorm(db);
        CHECK(norm >= previous);
        previous = norm;
    }
}

// The whole reason for a piecewise taper: the useful range must get most of the
// travel. Linear in dB would give -12..0 only about 17% of it.
void testTaperGivesTheUsefulRangeMostOfTheTravel() {
    const double unity = taper::dbToNorm(0.0);
    CHECK_NEAR(unity, 0.80, 0.001);

    const double topFortyDb = unity - taper::dbToNorm(-40.0);
    CHECK(topFortyDb > 0.60); // -40..0 dB occupies over 60% of the travel

    const double usefulBand = unity - taper::dbToNorm(-12.0);
    CHECK(usefulBand > 0.25); // -12..0 dB alone gets over a quarter
}

void testTaperEndsAndDetent() {
    CHECK_NEAR(taper::dbToNorm(taper::kMaxDb), 1.0, 1e-9);
    CHECK_NEAR(taper::dbToNorm(24.0), 1.0, 1e-9); // clamps rather than extrapolating

    // Bottom of the travel is a hard-off detent, not merely very quiet.
    CHECK_NEAR(taper::dbToNorm(taper::kSilenceDb), 0.0, 1e-9);
    CHECK_NEAR(taper::normToDb(0.0), taper::kSilenceDb, 1e-9);
    CHECK(taper::isSilent(taper::kMinDb));
    CHECK(taper::isSilent(-200.0));
    CHECK(!taper::isSilent(-60.0));
}

// 0.1 dB has to be resolvable by a pixel near unity, or the fader can't be set
// as precisely as the readout claims.
void testTaperResolutionNearUnity() {
    constexpr double kTravelPx = 300.0;
    const double dbPerPixel =
        (0.0 - (-1.0)) / ((taper::dbToNorm(0.0) - taper::dbToNorm(-1.0)) * kTravelPx);
    CHECK(dbPerPixel < 0.25);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    RUN(testCoalescesToLatestValue);
    RUN(testWriteRateIsBounded);
    RUN(testDistinctKeysDoNotCollapse);
    RUN(testOrderedOpsArePreservedAndNotCollapsed);
    RUN(testStructuralOpsFlushBeforeCoalescedOnes);
    RUN(testFlushNowSendsImmediately);
    RUN(testNoWritesWhenIdle);

    RUN(testHoldBlocksThenExpires);
    RUN(testPendingWriteOutlastsTheGracePeriod);
    RUN(testFieldsAreIndependent);
    RUN(testBandIndicesAreIndependent);
    RUN(testExpiredKeysAreReportedOnce);

    RUN(testAttackIsInstantAndReleaseDecays);
    RUN(testStalledSignalDecaysToSilence);
    RUN(testFramesBetweenTicksArePeakCombined);
    RUN(testClipLatchesUntilCleared);
    RUN(testPeakHoldLingersThenFollows);
    RUN(testDisarmingResetsToSilence);
    RUN(testUnknownChannelReadsAsSilence);

    RUN(testTaperRoundTrips);
    RUN(testTaperIsMonotonic);
    RUN(testTaperGivesTheUsefulRangeMostOfTheTravel);
    RUN(testTaperEndsAndDetent);
    RUN(testTaperResolutionNearUnity);

    return pipeeq::test::summary("gui_model");
}
