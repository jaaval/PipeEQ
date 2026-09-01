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
#include "fake_backend.h"
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
WriteOp sendOp(const QString& outputId, uint32_t channel, const QString& inputId, double gainDb) {
    WriteOp op;
    op.kind = WriteOp::Kind::Send;
    op.outputId = outputId;
    op.channelIndex = channel;
    op.stringArg = inputId;
    op.doubleArg = gainDb;
    return op;
}

WriteOp removeSendOp(const QString& outputId, uint32_t channel, const QString& inputId) {
    WriteOp op;
    op.kind = WriteOp::Kind::RemoveSend;
    op.outputId = outputId;
    op.channelIndex = channel;
    op.stringArg = inputId;
    return op;
}

// Ordered ops flush before coalesced ones, which is right for "set band count,
// then set band" - and wrong when the ordered op UNDOES the coalesced one.
// Switching a send off while a level write from the fader is still pending would
// otherwise emit [RemoveSend, Send] and the send would switch straight back on.
void testRemoveSendCancelsPendingLevelWrite() {
    WriteCoalescer coalescer;
    QVector<WriteOp> sent;
    QObject::connect(&coalescer, &WriteCoalescer::writesReady,
                     [&](const QVector<WriteOp>& ops) { sent.append(ops); });

    coalescer.enqueue(sendOp("output-1", 2, "input-1", -9.25));
    coalescer.enqueue(removeSendOp("output-1", 2, "input-1"));
    coalescer.flushNow();

    CHECK_EQ(sent.size(), 1);
    if (!sent.isEmpty()) {
        CHECK(sent.front().kind == WriteOp::Kind::RemoveSend);
    }
}

// ...but only for the SAME send. Removing one input's send must not cancel a
// pending level change to a different input, or to the same input on another
// channel.
void testRemoveSendOnlyCancelsItsOwnPendingWrite() {
    WriteCoalescer coalescer;
    QVector<WriteOp> sent;
    QObject::connect(&coalescer, &WriteCoalescer::writesReady,
                     [&](const QVector<WriteOp>& ops) { sent.append(ops); });

    coalescer.enqueue(sendOp("output-1", 2, "input-2", -3.0));
    coalescer.enqueue(sendOp("output-1", 5, "input-1", -4.0));
    coalescer.enqueue(removeSendOp("output-1", 2, "input-1"));
    coalescer.flushNow();

    CHECK_EQ(sent.size(), 3);
    int sends = 0;
    for (const WriteOp& op : sent) {
        if (op.kind == WriteOp::Kind::Send) {
            ++sends;
        }
    }
    CHECK_EQ(sends, 2);
}

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

// ------------------------------------------------- demo backend link semantics --

// The fake backend duplicates the daemon's linking rules, because it has no
// daemon behind it. Duplicated behaviour drifts, and UI built against a fake
// that behaves differently from the real thing is worse than no fake at all -
// so the rules are asserted here rather than assumed.

int channelIndexOf(FakeBackend& backend, const QString& outputId, const QString& position) {
    for (const StripRow& strip : backend.listStrips()) {
        if (strip.outputId == outputId && strip.position == position) {
            return static_cast<int>(strip.channelIndex);
        }
    }
    return -1;
}

StripRow stripFor(FakeBackend& backend, const QString& outputId, uint32_t channelIndex) {
    for (const StripRow& strip : backend.listStrips()) {
        if (strip.outputId == outputId && strip.channelIndex == channelIndex) {
            return strip;
        }
    }
    return {};
}

void testDemoLinkAdoptsLeaderValues() {
    FakeBackend backend;
    // The seeded 5.1 output has FC and LFE ungrouped, with different gains and
    // different curves - so linking them is a real test of what gets discarded.
    const QString outputId = "output-1";
    const int fc = channelIndexOf(backend, outputId, "FC");
    const int lfe = channelIndexOf(backend, outputId, "LFE");
    CHECK(fc >= 0);
    CHECK(lfe >= 0);
    if (fc < 0 || lfe < 0) {
        return;
    }

    const StripRow beforeFc = stripFor(backend, outputId, static_cast<uint32_t>(fc));
    const StripRow beforeLfe = stripFor(backend, outputId, static_cast<uint32_t>(lfe));
    CHECK(beforeFc.groupId.isEmpty());
    CHECK(!qFuzzyCompare(beforeFc.gainDb, beforeLfe.gainDb)); // the test needs them to differ

    const QString groupId = backend.createLinkGroup(
        outputId, {static_cast<uint32_t>(fc), static_cast<uint32_t>(lfe)}, "Test");
    CHECK(!groupId.isEmpty());

    const StripRow afterFc = stripFor(backend, outputId, static_cast<uint32_t>(fc));
    const StripRow afterLfe = stripFor(backend, outputId, static_cast<uint32_t>(lfe));
    CHECK_EQ(afterFc.groupId, groupId);
    CHECK_EQ(afterLfe.groupId, groupId);
    // The LOWER index wins, matching the daemon's leader rule.
    CHECK_NEAR(afterLfe.gainDb, beforeFc.gainDb, 1e-9);
    CHECK_NEAR(afterFc.gainDb, beforeFc.gainDb, 1e-9);
}

// Linking shares the curve; that is what makes a linked pair a pair.
void testDemoLinkSharesTheCurve() {
    FakeBackend backend;
    const QString outputId = "output-1";
    const int fc = channelIndexOf(backend, outputId, "FC");
    const int lfe = channelIndexOf(backend, outputId, "LFE");
    if (fc < 0 || lfe < 0) {
        return;
    }

    const auto fcBands = backend.getChannelEqBands(outputId, static_cast<uint32_t>(fc));
    const auto lfeBandsBefore = backend.getChannelEqBands(outputId, static_cast<uint32_t>(lfe));
    CHECK(!fcBands.empty());
    CHECK(!lfeBandsBefore.empty());
    CHECK(!(fcBands == lfeBandsBefore)); // they start different

    backend.createLinkGroup(outputId, {static_cast<uint32_t>(fc), static_cast<uint32_t>(lfe)}, "T");

    const auto lfeBandsAfter = backend.getChannelEqBands(outputId, static_cast<uint32_t>(lfe));
    CHECK(lfeBandsAfter == fcBands);
}

void testDemoUnlinkSeparatesChannels() {
    FakeBackend backend;
    const QString outputId = "output-1";
    // FL+FR are seeded already linked as group-1.
    const int fl = channelIndexOf(backend, outputId, "FL");
    const int fr = channelIndexOf(backend, outputId, "FR");
    if (fl < 0 || fr < 0) {
        return;
    }
    const StripRow linked = stripFor(backend, outputId, static_cast<uint32_t>(fl));
    CHECK(!linked.groupId.isEmpty());

    CHECK(backend.removeLinkGroup(outputId, linked.groupId));
    CHECK(stripFor(backend, outputId, static_cast<uint32_t>(fl)).groupId.isEmpty());
    CHECK(stripFor(backend, outputId, static_cast<uint32_t>(fr)).groupId.isEmpty());

    // Unlinking must not change any value - it only stops them moving together.
    CHECK_NEAR(stripFor(backend, outputId, static_cast<uint32_t>(fr)).gainDb, linked.gainDb, 1e-9);

    // ...and editing one must no longer touch the other.
    backend.setChannelEqBandCount(outputId, static_cast<uint32_t>(fl), 1);
    backend.setChannelEqBand(outputId, static_cast<uint32_t>(fl), 0, "peaking", 500.0, -8.0, 2.0);
    const auto frBands = backend.getChannelEqBands(outputId, static_cast<uint32_t>(fr));
    CHECK(frBands.size() != 1 || !qFuzzyCompare(frBands.front().freqHz, 500.0));
}

void testDemoLinkRefusesInvalidRequests() {
    FakeBackend backend;
    const QString outputId = "output-1";
    // Fewer than two channels is not a group.
    CHECK(backend.createLinkGroup(outputId, {0}, "T").isEmpty());
    CHECK(backend.createLinkGroup(outputId, {}, "T").isEmpty());
    // A channel already in a group can't join another - a channel in two groups
    // has no coherent meaning.
    CHECK(backend.createLinkGroup(outputId, {0, 2}, "T").isEmpty());
    // Out of range.
    CHECK(backend.createLinkGroup(outputId, {2, 99}, "T").isEmpty());
    // Unknown output.
    CHECK(backend.createLinkGroup("nope", {0, 1}, "T").isEmpty());
    CHECK(!backend.removeLinkGroup(outputId, "group-does-not-exist"));
}

// Gain, mute and sends move together for every member, which is what the strip
// shows as one fader.
void testDemoLinkedGainMovesAllMembers() {
    FakeBackend backend;
    const QString outputId = "output-1";
    const int fl = channelIndexOf(backend, outputId, "FL");
    const int fr = channelIndexOf(backend, outputId, "FR");
    if (fl < 0 || fr < 0) {
        return;
    }
    CHECK(backend.setChannelGain(outputId, static_cast<uint32_t>(fl), -13.5));
    CHECK_NEAR(stripFor(backend, outputId, static_cast<uint32_t>(fr)).gainDb, -13.5, 1e-9);

    CHECK(backend.setChannelMuted(outputId, static_cast<uint32_t>(fr), true));
    CHECK(stripFor(backend, outputId, static_cast<uint32_t>(fl)).muted);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    RUN(testCoalescesToLatestValue);
    RUN(testWriteRateIsBounded);
    RUN(testDistinctKeysDoNotCollapse);
    RUN(testOrderedOpsArePreservedAndNotCollapsed);
    RUN(testStructuralOpsFlushBeforeCoalescedOnes);
    RUN(testRemoveSendCancelsPendingLevelWrite);
    RUN(testRemoveSendOnlyCancelsItsOwnPendingWrite);
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

    RUN(testDemoLinkAdoptsLeaderValues);
    RUN(testDemoLinkSharesTheCurve);
    RUN(testDemoUnlinkSeparatesChannels);
    RUN(testDemoLinkRefusesInvalidRequests);
    RUN(testDemoLinkedGainMovesAllMembers);

    return pipeeq::test::summary("gui_model");
}
