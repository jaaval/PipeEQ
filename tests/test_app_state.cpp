// Drives AppState end-to-end against the demo backend.
//
// This exercises the seam that a widget-level test cannot reach and that unit
// tests of the individual pieces miss entirely: the store hands work to a
// backend on another thread, and if that hand-off silently does nothing, every
// component looks correct in isolation while the feature is simply dead. A
// linking bug shipped exactly that way.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>

#include <cstdio>

#include <cmath>
#include <functional>

#include "model/app_state.h"

#include "check.h"

namespace {

using namespace pipeeq;

// Pumps the event loop until `predicate` holds or the timeout expires. The store
// is asynchronous by design, so every assertion has to wait for a round trip
// rather than assume one already happened.
bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate()) {
            return true;
        }
    }
    return predicate();
}

// Formats a channel's sends, so a failure says what was actually there instead
// of just "predicate was false".
QString sendsText(AppState& state, const QString& outputId, uint32_t channelIndex) {
    QStringList parts;
    for (const auto& [id, gainDb] : state.channelSends(outputId, channelIndex)) {
        parts << QString("%1=%2").arg(id).arg(gainDb, 0, 'f', 2);
    }
    return parts.isEmpty() ? QStringLiteral("<none>") : parts.join(" ");
}

const StripRow* findByPosition(AppState& state, const QString& outputId, const QString& position) {
    for (const StripRow& strip : state.strips()) {
        if (strip.outputId == outputId && strip.position == position) {
            return &strip;
        }
    }
    return nullptr;
}

QString groupIdOf(AppState& state, const QString& outputId, const QString& position) {
    const StripRow* strip = findByPosition(state, outputId, position);
    return strip ? strip->groupId : QString("<missing>");
}

void testStoreLoadsTheTopology() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));
    CHECK(!state.devices().isEmpty());
    CHECK(!state.inputs().isEmpty());
    // The demo topology: a 6-channel card, a 4-channel interface and an absent
    // stereo device.
    CHECK_EQ(state.strips().size(), 12);
}

// The bug this file exists for: the store accepted the request and nothing
// happened, because the cross-thread hand-off named a type moc had recorded
// differently and the call was dropped with only a warning.
void testLinkChannelsActuallyLinks() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    const QString outputId = "output-1";
    const StripRow* centre = findByPosition(state, outputId, "FC");
    const StripRow* lfe = findByPosition(state, outputId, "LFE");
    CHECK(centre != nullptr);
    CHECK(lfe != nullptr);
    if (!centre || !lfe) {
        return;
    }
    CHECK(centre->groupId.isEmpty());
    CHECK(lfe->groupId.isEmpty());

    const uint32_t centreIndex = centre->channelIndex;
    const uint32_t lfeIndex = lfe->channelIndex;
    state.linkChannels(outputId, {centreIndex, lfeIndex});

    CHECK(waitFor([&] { return !groupIdOf(state, outputId, "FC").isEmpty(); }));
    const QString groupId = groupIdOf(state, outputId, "FC");
    CHECK(!groupId.isEmpty());
    CHECK_EQ(groupIdOf(state, outputId, "LFE"), groupId);
}

void testUnlinkGroupActuallyUnlinks() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    const QString outputId = "output-1";
    // FL+FR are seeded already linked.
    const QString groupId = groupIdOf(state, outputId, "FL");
    CHECK(!groupId.isEmpty());
    CHECK_EQ(groupIdOf(state, outputId, "FR"), groupId);

    state.unlinkGroup(outputId, groupId);

    CHECK(waitFor([&] { return groupIdOf(state, outputId, "FL").isEmpty(); }));
    CHECK(groupIdOf(state, outputId, "FL").isEmpty());
    CHECK(groupIdOf(state, outputId, "FR").isEmpty());
}

// The reported bug: linking asked for confirmation and then appeared to do
// nothing.
//
// The store reported it as a mere VALUE update, because the set of strip ids is
// unchanged by linking - only groupId moves. The rack groups linked channels
// into one widget keyed by group, so a value update had nothing to update and
// silently did nothing. Grouping is topology, and this asserts the store says so.
void testLinkingIsReportedAsATopologyChange() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    int topologyChanges = 0;
    QObject::connect(&state, &AppState::topologyChanged, [&] { ++topologyChanges; });

    const StripRow* centre = findByPosition(state, "output-1", "FC");
    const StripRow* lfe = findByPosition(state, "output-1", "LFE");
    CHECK(centre != nullptr);
    CHECK(lfe != nullptr);
    if (!centre || !lfe) {
        return;
    }
    const uint32_t centreIndex = centre->channelIndex;
    const uint32_t lfeIndex = lfe->channelIndex;

    state.linkChannels("output-1", {centreIndex, lfeIndex});
    CHECK(waitFor([&] { return topologyChanges > 0; }));
    CHECK(topologyChanges > 0);

    // ...and again on the way back out.
    const QString groupId = groupIdOf(state, "output-1", "FC");
    CHECK(!groupId.isEmpty());
    const int afterLink = topologyChanges;
    state.unlinkGroup("output-1", groupId);
    CHECK(waitFor([&] { return topologyChanges > afterLink; }));
    CHECK(topologyChanges > afterLink);
}

// A plain value change must NOT be reported as topology, or the rack rebuilds
// its widgets on every fader move and drops the drag in progress.
void testGainChangeIsNotATopologyChange() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    const StripRow* centre = findByPosition(state, "output-1", "FC");
    CHECK(centre != nullptr);
    if (!centre) {
        return;
    }
    const uint32_t channelIndex = centre->channelIndex;

    int topologyChanges = 0;
    QObject::connect(&state, &AppState::topologyChanged, [&] { ++topologyChanges; });

    state.setChannelGain("output-1", channelIndex, -17.0);
    CHECK(waitFor([&] {
        const StripRow* now = findByPosition(state, "output-1", "FC");
        return now && std::abs(now->gainDb - (-17.0)) < 1e-6;
    }));
    CHECK_EQ(topologyChanges, 0);
}

void testGroupChannelsReportsMembers() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    const QString outputId = "output-1";
    const QString groupId = groupIdOf(state, outputId, "FL");
    const QVector<uint32_t> members = state.groupChannels(outputId, groupId);
    CHECK_EQ(members.size(), 2);
    if (members.size() == 2) {
        CHECK(members[0] < members[1]); // channel order
    }
    CHECK(state.groupChannels(outputId, QString()).isEmpty());
}

// The other cross-thread hand-offs, for the same reason: each is a call that
// looks fine at every layer and does nothing if the hand-off is wrong.
void testChannelGainReachesTheBackend() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    const QString outputId = "output-1";
    const StripRow* centre = findByPosition(state, outputId, "FC");
    CHECK(centre != nullptr);
    if (!centre) {
        return;
    }
    state.setChannelGain(outputId, centre->channelIndex, -21.5);

    CHECK(waitFor([&] {
        const StripRow* now = findByPosition(state, outputId, "FC");
        return now && std::abs(now->gainDb - (-21.5)) < 1e-6;
    }));
}

void testChannelMuteReachesTheBackend() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    const StripRow* centre = findByPosition(state, "output-1", "FC");
    CHECK(centre != nullptr);
    if (!centre) {
        return;
    }
    CHECK(!centre->muted);
    state.setChannelMuted("output-1", centre->channelIndex, true);

    CHECK(waitFor([&] {
        const StripRow* now = findByPosition(state, "output-1", "FC");
        return now && now->muted;
    }));
}

void testChannelPositionReachesTheBackend() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    const StripRow* centre = findByPosition(state, "output-1", "FC");
    CHECK(centre != nullptr);
    if (!centre) {
        return;
    }
    state.setChannelPosition("output-1", centre->channelIndex, "SL");

    CHECK(waitFor([&] { return findByPosition(state, "output-1", "SL") != nullptr; }));
}

void testSendsReachTheBackend() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    const StripRow* centre = findByPosition(state, "output-1", "FC");
    CHECK(centre != nullptr);
    if (!centre) {
        return;
    }
    const QString outputId = centre->outputId;
    const uint32_t channelIndex = centre->channelIndex;

    state.requestOutputSends(outputId);
    CHECK(waitFor([&] { return !state.channelSends(outputId, channelIndex).isEmpty(); }));

    const QString inputId = state.inputs().front().id;
    state.setSend(outputId, channelIndex, inputId, -9.25);
    state.requestOutputSends(outputId);

    CHECK(waitFor([&] {
        for (const auto& [id, gainDb] : state.channelSends(outputId, channelIndex)) {
            if (id == inputId && std::abs(gainDb - (-9.25)) < 1e-6) {
                return true;
            }
        }
        return false;
    }));

    state.removeSend(outputId, channelIndex, inputId);
    state.requestOutputSends(outputId);
    const bool removed = waitFor([&] {
        for (const auto& [id, gainDb] : state.channelSends(outputId, channelIndex)) {
            if (id == inputId) {
                return false;
            }
        }
        return true;
    });
    if (!removed) {
        std::fprintf(stderr, "      removing %s left: %s\n", inputId.toUtf8().constData(),
                     sendsText(state, outputId, channelIndex).toUtf8().constData());
    }
    CHECK(removed);
}

void testEqBandsReachTheBackend() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    const StripRow* centre = findByPosition(state, "output-1", "FC");
    CHECK(centre != nullptr);
    if (!centre) {
        return;
    }
    const QString outputId = centre->outputId;
    const uint32_t channelIndex = centre->channelIndex;

    state.setChannelEqBandCount(outputId, channelIndex, 2);
    CHECK(waitFor([&] { return state.channelBands(outputId, channelIndex).size() == 2; }));

    eqcore::EqBand band;
    band.type = eqcore::FilterType::HighShelf;
    band.freqHz = 6500.0;
    band.gainDb = -7.25;
    band.q = 1.4;
    state.setChannelEqBand(outputId, channelIndex, 1, band);
    state.requestChannelDetail(outputId, channelIndex);

    CHECK(waitFor([&] {
        const QVector<eqcore::EqBand> bands = state.channelBands(outputId, channelIndex);
        return bands.size() == 2 && std::abs(bands[1].freqHz - 6500.0) < 1e-6 &&
               bands[1].type == eqcore::FilterType::HighShelf;
    }));
}

void testTopologyOperationsReachTheBackend() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    const int inputsBefore = state.inputs().size();
    state.addInput("Scripted");
    CHECK(waitFor([&] { return state.inputs().size() == inputsBefore + 1; }));

    QString addedId;
    for (const InputRow& input : state.inputs()) {
        if (input.displayName == "Scripted") {
            addedId = input.id;
        }
    }
    CHECK(!addedId.isEmpty());
    state.removeInput(addedId);
    CHECK(waitFor([&] { return state.inputs().size() == inputsBefore; }));

    // The absent HD650 device is not in the device list, so add an output on a
    // device that is - the third demo device is unclaimed.
    const int stripsBefore = state.strips().size();
    state.addOutput(state.devices().back().nodeName, "Scripted output");
    CHECK(waitFor([&] { return state.strips().size() > stripsBefore; }));
}

void testAutoConnectReachesTheBackend() {
    AppState state(/*demo=*/true);
    CHECK(waitFor([&] { return !state.strips().isEmpty(); }));

    const StripRow* strip = findByPosition(state, "output-1", "FC");
    CHECK(strip != nullptr);
    if (!strip) {
        return;
    }
    CHECK(strip->autoConnect);
    state.setOutputAutoConnect("output-1", false);

    CHECK(waitFor([&] {
        const StripRow* now = findByPosition(state, "output-1", "FC");
        return now && !now->autoConnect;
    }));
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    RUN(testStoreLoadsTheTopology);
    RUN(testLinkChannelsActuallyLinks);
    RUN(testUnlinkGroupActuallyUnlinks);
    RUN(testLinkingIsReportedAsATopologyChange);
    RUN(testGainChangeIsNotATopologyChange);
    RUN(testGroupChannelsReportsMembers);
    RUN(testChannelGainReachesTheBackend);
    RUN(testChannelMuteReachesTheBackend);
    RUN(testChannelPositionReachesTheBackend);
    RUN(testSendsReachTheBackend);
    RUN(testEqBandsReachTheBackend);
    RUN(testTopologyOperationsReachTheBackend);
    RUN(testAutoConnectReachesTheBackend);

    return pipeeq::test::summary("app_state");
}
