// Channel-layout helpers: parsing what PipeWire reports, naming positions in
// both directions, defaulting a layout for a device that advertises none, and
// deciding when a layout change forces a stream renegotiation.

#include <string>
#include <vector>

#include <spa/param/audio/raw.h>

#include "channel_layout.h"

#include "check.h"

namespace {

using namespace pipeeq::layout;
using Names = std::vector<std::string>;

void testParseBracketedList() {
    // The exact shape PipeWire reports audio.position in for a 4.0 sink.
    CHECK_EQ(parseChannelPositions("[ FL, FR, RL, RR ]"), Names({"FL", "FR", "RL", "RR"}));
    CHECK_EQ(parseChannelPositions("FL,FR"), Names({"FL", "FR"}));
    CHECK_EQ(parseChannelPositions("[\"FL\",\"FR\"]"), Names({"FL", "FR"}));
    CHECK_EQ(parseChannelPositions("[ MONO ]"), Names({"MONO"}));
}

void testParseDegenerateInput() {
    CHECK_EQ(parseChannelPositions(nullptr), Names({}));
    CHECK_EQ(parseChannelPositions(""), Names({}));
    CHECK_EQ(parseChannelPositions("[]"), Names({}));
    CHECK_EQ(parseChannelPositions("   "), Names({}));
    // Garbage is passed through as names rather than dropped: positionValue()
    // is what decides whether a name is meaningful, and it reports UNKNOWN.
    CHECK_EQ(parseChannelPositions("[ FL, NOPE ]"), Names({"FL", "NOPE"}));
    CHECK_EQ(positionValue("NOPE"), static_cast<uint32_t>(SPA_AUDIO_CHANNEL_UNKNOWN));
}

void testPositionValueAndName() {
    CHECK_EQ(positionValue("FL"), static_cast<uint32_t>(SPA_AUDIO_CHANNEL_FL));
    CHECK_EQ(positionValue("FR"), static_cast<uint32_t>(SPA_AUDIO_CHANNEL_FR));
    CHECK_EQ(positionValue("LFE"), static_cast<uint32_t>(SPA_AUDIO_CHANNEL_LFE));
    CHECK_EQ(positionValue("MONO"), static_cast<uint32_t>(SPA_AUDIO_CHANNEL_MONO));
    CHECK_EQ(positionValue(""), static_cast<uint32_t>(SPA_AUDIO_CHANNEL_UNKNOWN));

    CHECK_EQ(positionName(SPA_AUDIO_CHANNEL_FL), std::string("FL"));
    CHECK_EQ(positionName(SPA_AUDIO_CHANNEL_LFE), std::string("LFE"));
    CHECK_EQ(positionName(SPA_AUDIO_CHANNEL_UNKNOWN), std::string("UNK"));
}

// The AUX range matters: a Focusrite in its "Pro Audio" profile advertises
// AUX0..AUXn rather than FL/FR, and those names have to survive a round trip
// or an output pinned to one would silently lose its channel.
void testAuxRoundTrip() {
    CHECK_EQ(positionValue("AUX0"), static_cast<uint32_t>(SPA_AUDIO_CHANNEL_AUX0));
    CHECK_EQ(positionValue("AUX3"), static_cast<uint32_t>(SPA_AUDIO_CHANNEL_AUX0 + 3));
    CHECK_EQ(positionName(SPA_AUDIO_CHANNEL_AUX0), std::string("AUX0"));
    CHECK_EQ(positionName(SPA_AUDIO_CHANNEL_AUX0 + 17), std::string("AUX17"));

    for (const char* name : {"FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR", "AUX0", "AUX7"}) {
        CHECK_EQ(positionName(positionValue(name)), std::string(name));
    }
}

void testDefaultPositionsForKnownCounts() {
    CHECK_EQ(defaultPositionsFor(1), Names({"MONO"}));
    CHECK_EQ(defaultPositionsFor(2), Names({"FL", "FR"}));
    CHECK_EQ(defaultPositionsFor(4), Names({"FL", "FR", "RL", "RR"}));
    CHECK_EQ(defaultPositionsFor(6), Names({"FL", "FR", "FC", "LFE", "RL", "RR"}));
    CHECK_EQ(defaultPositionsFor(8), Names({"FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"}));
    CHECK_EQ(defaultPositionsFor(12).size(), std::size_t{12});
    CHECK_EQ(defaultPositionsFor(12).back(), std::string("TRR"));

    // Every default must be a name SPA actually knows, or the stream format
    // built from it would carry UNKNOWN positions.
    for (int count : {1, 2, 3, 4, 5, 6, 8, 12}) {
        for (const std::string& name : defaultPositionsFor(count)) {
            CHECK(positionValue(name) != static_cast<uint32_t>(SPA_AUDIO_CHANNEL_UNKNOWN));
        }
    }
}

void testDefaultPositionsFallsBackToAux() {
    CHECK_EQ(defaultPositionsFor(7), Names({"AUX0", "AUX1", "AUX2", "AUX3", "AUX4", "AUX5", "AUX6"}));
    CHECK_EQ(defaultPositionsFor(10).front(), std::string("AUX0"));
    CHECK_EQ(defaultPositionsFor(10).back(), std::string("AUX9"));
    CHECK_EQ(defaultPositionsFor(0), Names({}));
    CHECK_EQ(defaultPositionsFor(-1), Names({}));
}

void testNeedsRenegotiation() {
    // A property refresh reporting the same layout must NOT reconnect.
    CHECK(!needsRenegotiation({"FL", "FR"}, {"FL", "FR"}));
    CHECK(!needsRenegotiation({}, {}));
    // Channel count change (profile switch 4.0 <-> stereo).
    CHECK(needsRenegotiation({"FL", "FR"}, {"FL", "FR", "RL", "RR"}));
    CHECK(needsRenegotiation({"FL", "FR", "RL", "RR"}, {"FL", "FR"}));
    // Same count, different positions (surround profile <-> Pro Audio).
    CHECK(needsRenegotiation({"FL", "FR"}, {"AUX0", "AUX1"}));
    // Same set, different order still renegotiates: index is the wire channel.
    CHECK(needsRenegotiation({"FL", "FR"}, {"FR", "FL"}));
}

void testIsFrontPosition() {
    CHECK(isFrontPosition("FL"));
    CHECK(isFrontPosition("FR"));
    CHECK(isFrontPosition("FC"));
    CHECK(isFrontPosition("MONO"));
    CHECK(!isFrontPosition("LFE"));
    CHECK(!isFrontPosition("RL"));
    CHECK(!isFrontPosition("AUX0"));
    CHECK(!isFrontPosition(""));
}

} // namespace

int main() {
    RUN(testParseBracketedList);
    RUN(testParseDegenerateInput);
    RUN(testPositionValueAndName);
    RUN(testAuxRoundTrip);
    RUN(testDefaultPositionsForKnownCounts);
    RUN(testDefaultPositionsFallsBackToAux);
    RUN(testNeedsRenegotiation);
    RUN(testIsFrontPosition);
    return pipeeq::test::summary("channel_layout");
}
