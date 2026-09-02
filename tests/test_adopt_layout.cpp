// adoptDeviceLayout: reshaping an output's channel list to whatever layout its
// device actually advertises, without losing settings it could have kept.
//
// The 4.0 -> stereo -> 4.0 round trip is the case that matters most: before
// this existed, flipping a card's profile silently deleted the rear channels'
// EQ and sends.

#include <map>
#include <string>
#include <vector>

#include "adopt_layout.h"
#include "app_config.h"

#include "check.h"

namespace {

using namespace pipeeq;
using namespace eqcore;
using Names = std::vector<std::string>;

// A channel as a post-migration config holds it: the device's own name and the
// user's routing assignment start out the same.
OutputChannelConfig channel(const std::string& position, double gainDb, const std::string& eqId,
                             std::map<std::string, double> sends = {{"input-1", 0.0}}) {
    OutputChannelConfig c;
    c.position = position;
    c.devicePosition = position;
    c.gainDb = gainDb;
    c.eqInstanceId = eqId;
    c.sendsDb = std::move(sends);
    return c;
}

Names devicePositionsOf(const OutputConfig& output) {
    Names names;
    for (const auto& c : output.channels) {
        names.push_back(c.devicePosition);
    }
    return names;
}

Names positionsOf(const OutputConfig& output) {
    Names names;
    for (const auto& c : output.channels) {
        names.push_back(c.position);
    }
    return names;
}

// A stereo-configured output meeting its real 4.0 device.
void testAppendsNewDeviceChannelsSilently() {
    OutputConfig output;
    output.channels = {channel("FL", -3.0, "eq-1"), channel("FR", -3.0, "eq-1")};

    const AdoptResult result = adoptDeviceLayout(output, {"FL", "FR", "RL", "RR"});

    CHECK_EQ(result.liveChannelCount, std::size_t{4});
    CHECK_EQ(result.appendedChannels, std::size_t{2});
    CHECK_EQ(result.retiredChannels, std::size_t{0});
    CHECK(result.changed);
    CHECK_EQ(positionsOf(output), Names({"FL", "FR", "RL", "RR"}));

    // The existing pair keeps everything.
    CHECK_NEAR(output.channels[0].gainDb, -3.0, 1e-12);
    CHECK_EQ(output.channels[0].eqInstanceId, std::string("eq-1"));

    // The new pair is present and editable but SILENT: no sends at all, so
    // upgrading doesn't suddenly start playing the mix out of outputs 3 and 4.
    CHECK_EQ(output.channels[2].sendsDb.size(), std::size_t{0});
    CHECK_EQ(output.channels[3].sendsDb.size(), std::size_t{0});
    CHECK_EQ(output.channels[2].eqInstanceId, std::string(""));
    CHECK_NEAR(output.channels[2].gainDb, 0.0, 1e-12);
}

// Shrinking must RETIRE rather than delete.
void testShrinkingRetiresRatherThanDeletes() {
    OutputConfig output;
    output.channels = {channel("FL", -1.0, "eq-1"), channel("FR", -1.0, "eq-1"),
                        channel("RL", -7.5, "eq-2"), channel("RR", -7.5, "eq-2")};

    const AdoptResult result = adoptDeviceLayout(output, {"FL", "FR"});

    CHECK_EQ(result.liveChannelCount, std::size_t{2});
    CHECK_EQ(result.retiredChannels, std::size_t{2});
    // All four entries survive; only the first two are driven.
    CHECK_EQ(output.channels.size(), std::size_t{4});
    CHECK_EQ(positionsOf(output), Names({"FL", "FR", "RL", "RR"}));
    CHECK_NEAR(output.channels[2].gainDb, -7.5, 1e-12);
    CHECK_EQ(output.channels[2].eqInstanceId, std::string("eq-2"));
}

// The headline guarantee: a full profile round trip loses nothing.
void testProfileRoundTripRestoresSettings() {
    OutputConfig output;
    output.channels = {channel("FL", -1.0, "eq-1"), channel("FR", -1.0, "eq-1"),
                        channel("RL", -7.5, "eq-2", {{"input-2", -12.0}}),
                        channel("RR", -7.5, "eq-2", {{"input-2", -12.0}})};
    const OutputConfig before = output;

    adoptDeviceLayout(output, {"FL", "FR"});          // profile switched to stereo
    adoptDeviceLayout(output, {"FL", "FR", "RL", "RR"}); // ...and back

    CHECK(output.channels == before.channels);
}

// Matching is by position NAME, so a device that reorders its channels doesn't
// scramble which strip is which.
void testMatchesByNameNotByIndex() {
    OutputConfig output;
    output.channels = {channel("FL", -2.0, "eq-1"), channel("FR", -4.0, "eq-2")};

    adoptDeviceLayout(output, {"RL", "RR", "FL", "FR"});

    CHECK_EQ(positionsOf(output), Names({"RL", "RR", "FL", "FR"}));
    // FL/FR moved to indices 2 and 3, carrying their settings.
    CHECK_NEAR(output.channels[2].gainDb, -2.0, 1e-12);
    CHECK_EQ(output.channels[2].eqInstanceId, std::string("eq-1"));
    CHECK_NEAR(output.channels[3].gainDb, -4.0, 1e-12);
    // RL/RR are brand new and silent.
    CHECK_EQ(output.channels[0].sendsDb.size(), std::size_t{0});
}

// A device renaming its channels wholesale - a Focusrite switching between a
// surround profile and "Pro Audio" - falls back to matching by index, which
// preserves the settings the user had on those physical outputs.
void testFallsBackToIndexWhenNamesAllChange() {
    OutputConfig output;
    output.channels = {channel("FL", -5.0, "eq-1"), channel("FR", -6.0, "eq-2")};

    const AdoptResult result = adoptDeviceLayout(output, {"AUX0", "AUX1"});

    CHECK_EQ(result.appendedChannels, std::size_t{0});
    CHECK_EQ(result.retiredChannels, std::size_t{0});
    // The device's names change...
    CHECK_EQ(devicePositionsOf(output), Names({"AUX0", "AUX1"}));
    // ...but the ROUTING assignment does not. "This physical output is my front
    // left speaker" is the user's statement about their room, and a profile
    // rename is no reason to discard it - otherwise a stereo input would stop
    // reaching those speakers entirely.
    CHECK_EQ(positionsOf(output), Names({"FL", "FR"}));
    CHECK_NEAR(output.channels[0].gainDb, -5.0, 1e-12);
    CHECK_EQ(output.channels[0].eqInstanceId, std::string("eq-1"));
    CHECK_NEAR(output.channels[1].gainDb, -6.0, 1e-12);
    CHECK_EQ(output.channels[1].eqInstanceId, std::string("eq-2"));
}

// The reason position and devicePosition are separate fields: a relabel has to
// survive the next connect. Previously adoptDeviceLayout overwrote `position`
// with the device's name, so setting a channel's position had no lasting effect.
void testUserRelabelSurvivesReconnect() {
    OutputConfig output;
    output.channels = {channel("FL", 0.0, "eq-1"), channel("FR", 0.0, "eq-1"),
                        channel("RL", 0.0, ""), channel("RR", 0.0, "")};

    // The user decides physical channel 2 actually feeds their centre speaker.
    output.channels[2].position = "FC";

    // Reconnecting to the very same device must not undo that.
    adoptDeviceLayout(output, {"FL", "FR", "RL", "RR"});
    CHECK_EQ(positionsOf(output), Names({"FL", "FR", "FC", "RR"}));
    CHECK_EQ(devicePositionsOf(output), Names({"FL", "FR", "RL", "RR"}));

    // ...and neither must a profile round trip.
    adoptDeviceLayout(output, {"FL", "FR"});
    adoptDeviceLayout(output, {"FL", "FR", "RL", "RR"});
    CHECK_EQ(positionsOf(output), Names({"FL", "FR", "FC", "RR"}));
}

// A config written before devicePosition existed gets it backfilled once, from
// the position it had at the time - which is what that field then meant.
void testLegacyConfigBackfillsDevicePosition() {
    OutputConfig output;
    OutputChannelConfig legacyLeft;
    legacyLeft.position = "FL";
    OutputChannelConfig legacyRight;
    legacyRight.position = "FR";
    output.channels = {legacyLeft, legacyRight};

    const AdoptResult result = adoptDeviceLayout(output, {"FL", "FR"});

    CHECK(result.changed); // the backfill is a change, once
    CHECK_EQ(devicePositionsOf(output), Names({"FL", "FR"}));
    CHECK_EQ(positionsOf(output), Names({"FL", "FR"}));

    // Idempotent from then on.
    const AdoptResult again = adoptDeviceLayout(output, {"FL", "FR"});
    CHECK(!again.changed);
}

// Mixed: some names match, the leftovers fall back to index.
void testPartialNameMatchThenIndexFallback() {
    OutputConfig output;
    output.channels = {channel("FL", -1.0, "eq-1"), channel("FR", -2.0, "eq-2"),
                        channel("RL", -3.0, "eq-3")};

    adoptDeviceLayout(output, {"FL", "FR", "SL"});

    CHECK_EQ(devicePositionsOf(output), Names({"FL", "FR", "SL"}));
    // RL had no name match, so it filled the remaining slot by index, keeping
    // its settings and its routing assignment.
    CHECK_EQ(positionsOf(output), Names({"FL", "FR", "RL"}));
    CHECK_NEAR(output.channels[2].gainDb, -3.0, 1e-12);
    CHECK_EQ(output.channels[2].eqInstanceId, std::string("eq-3"));
}

// Link groups index into the channel vector, so any reordering has to remap
// them or a "linked" pair silently stops being linked.
void testLinkGroupsAreRemappedThroughReordering() {
    OutputConfig output;
    output.channels = {channel("FL", 0.0, "eq-1"), channel("FR", 0.0, "eq-1")};
    output.linkGroups.push_back(LinkGroupConfig{"group-1", "Mains", {0, 1}});

    adoptDeviceLayout(output, {"RL", "RR", "FL", "FR"});

    CHECK_EQ(output.linkGroups.at(0).channelIndices, std::vector<uint32_t>({2, 3}));
    CHECK_EQ(output.linkedChannels(2), std::vector<std::size_t>({2, 3}));
}

// A group whose members are retired must keep pointing at them, so flipping
// back restores the grouping too.
void testLinkGroupSurvivesRetirement() {
    OutputConfig output;
    output.channels = {channel("FL", 0.0, ""), channel("FR", 0.0, ""), channel("RL", 0.0, ""),
                        channel("RR", 0.0, "")};
    output.linkGroups.push_back(LinkGroupConfig{"group-1", "Mains", {0, 1}});
    output.linkGroups.push_back(LinkGroupConfig{"group-2", "Rears", {2, 3}});

    adoptDeviceLayout(output, {"FL", "FR"});
    CHECK_EQ(output.linkGroups.at(1).channelIndices, std::vector<uint32_t>({2, 3}));

    adoptDeviceLayout(output, {"FL", "FR", "RL", "RR"});
    CHECK_EQ(output.linkGroups.at(0).channelIndices, std::vector<uint32_t>({0, 1}));
    CHECK_EQ(output.linkGroups.at(1).channelIndices, std::vector<uint32_t>({2, 3}));
}

void testIdenticalLayoutChangesNothing() {
    OutputConfig output;
    output.channels = {channel("FL", -1.0, "eq-1"), channel("FR", -1.0, "eq-1")};
    output.linkGroups.push_back(LinkGroupConfig{"group-1", "Mains", {0, 1}});
    const OutputConfig before = output;

    const AdoptResult result = adoptDeviceLayout(output, {"FL", "FR"});

    CHECK(!result.changed);
    CHECK(output == before);
}

void testFirstConnectFromEmptyConfig() {
    OutputConfig output;
    const AdoptResult result = adoptDeviceLayout(output, {"FL", "FR", "FC", "LFE", "RL", "RR"});

    CHECK_EQ(result.liveChannelCount, std::size_t{6});
    CHECK_EQ(result.appendedChannels, std::size_t{6});
    CHECK_EQ(positionsOf(output), Names({"FL", "FR", "FC", "LFE", "RL", "RR"}));
    for (const auto& c : output.channels) {
        CHECK_EQ(c.sendsDb.size(), std::size_t{0});
    }
}

void testEmptyDeviceLayoutRetiresEverything() {
    OutputConfig output;
    output.channels = {channel("FL", -1.0, "eq-1"), channel("FR", -1.0, "eq-1")};

    const AdoptResult result = adoptDeviceLayout(output, {});

    CHECK_EQ(result.liveChannelCount, std::size_t{0});
    CHECK_EQ(result.retiredChannels, std::size_t{2});
    CHECK_EQ(output.channels.size(), std::size_t{2});
}

} // namespace

int main() {
    RUN(testAppendsNewDeviceChannelsSilently);
    RUN(testShrinkingRetiresRatherThanDeletes);
    RUN(testProfileRoundTripRestoresSettings);
    RUN(testMatchesByNameNotByIndex);
    RUN(testFallsBackToIndexWhenNamesAllChange);
    RUN(testUserRelabelSurvivesReconnect);
    RUN(testLegacyConfigBackfillsDevicePosition);
    RUN(testPartialNameMatchThenIndexFallback);
    RUN(testLinkGroupsAreRemappedThroughReordering);
    RUN(testLinkGroupSurvivesRetirement);
    RUN(testIdenticalLayoutChangesNothing);
    RUN(testFirstConnectFromEmptyConfig);
    RUN(testEmptyDeviceLayoutRetiresEverything);
    return pipeeq::test::summary("adopt_layout");
}
