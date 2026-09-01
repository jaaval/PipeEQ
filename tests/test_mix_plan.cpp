// The position-based routing rules. This is the highest-value suite in the
// project: every "which input channel reaches which speaker" decision is made
// here, and the realtime thread just applies whatever coefficients come out.

#include <span>
#include <string>
#include <vector>

#include <spa/param/audio/raw.h>

#include "channel_layout.h"
#include "mix_plan.h"

#include "check.h"

namespace {

using namespace pipeeq;
using namespace pipeeq::mix;

std::vector<uint32_t> positions(const std::vector<std::string>& names) {
    return mix::positionValues(names);
}

ChannelTaps tapsFor(const std::string& outputPosition, std::size_t outputChannelIndex,
                     const std::vector<std::string>& inputPositions, SendSpec spec = {}) {
    const std::vector<uint32_t> in = positions(inputPositions);
    return buildTaps(layout::positionValue(outputPosition), outputChannelIndex, in, spec);
}

// ------------------------------------------------------------ direct matches --

void testStereoIntoStereoIsOneTapEach() {
    const std::vector<std::string> stereo = {"FL", "FR"};

    const ChannelTaps left = tapsFor("FL", 0, stereo);
    CHECK_EQ(left.count, uint8_t{1});
    CHECK_EQ(left.taps[0].inputChannel, uint16_t{0});
    CHECK_NEAR(left.taps[0].gain, 1.0, 1e-6);

    const ChannelTaps right = tapsFor("FR", 1, stereo);
    CHECK_EQ(right.count, uint8_t{1});
    CHECK_EQ(right.taps[0].inputChannel, uint16_t{1});
}

// The headline requirement: a stereo sink lands on FL+FR regardless of how many
// channels the hardware output has.
void testStereoIntoSurroundFeedsOnlyTheFronts() {
    const std::vector<std::string> stereo = {"FL", "FR"};
    const std::vector<std::string> surround = {"FL", "FR", "FC", "LFE", "RL", "RR"};

    for (std::size_t ch = 0; ch < surround.size(); ++ch) {
        const ChannelTaps taps = tapsFor(surround[ch], ch, stereo);
        if (surround[ch] == "FL" || surround[ch] == "FR") {
            CHECK_EQ(taps.count, uint8_t{1});
        } else {
            // FC, LFE, RL, RR get silence - not a duplicated front.
            CHECK_EQ(taps.count, uint8_t{0});
        }
    }
}

// The other headline requirement: a 5.1 sink into a stereo output passes FL/FR
// and drops the rest (until downmix exists).
void testSurroundIntoStereoPassesFrontsOnly() {
    const std::vector<std::string> surround = {"FL", "FR", "FC", "LFE", "RL", "RR"};

    const ChannelTaps left = tapsFor("FL", 0, surround);
    CHECK_EQ(left.count, uint8_t{1});
    CHECK_EQ(left.taps[0].inputChannel, uint16_t{0});

    const ChannelTaps right = tapsFor("FR", 1, surround);
    CHECK_EQ(right.count, uint8_t{1});
    CHECK_EQ(right.taps[0].inputChannel, uint16_t{1});
}

// Channel ORDER must not matter - only position. An input that lists LFE before
// FC still has to land correctly.
void testMatchingIsByPositionNotByIndex() {
    const std::vector<std::string> oddOrder = {"RR", "RL", "FR", "FL"};

    CHECK_EQ(tapsFor("FL", 0, oddOrder).taps[0].inputChannel, uint16_t{3});
    CHECK_EQ(tapsFor("FR", 1, oddOrder).taps[0].inputChannel, uint16_t{2});
    CHECK_EQ(tapsFor("RL", 2, oddOrder).taps[0].inputChannel, uint16_t{1});
    CHECK_EQ(tapsFor("RR", 3, oddOrder).taps[0].inputChannel, uint16_t{0});
}

// -------------------------------------------------------------- mono sources --

void testMonoInputFeedsFrontsAtUnity() {
    const std::vector<std::string> mono = {"MONO"};

    for (const char* front : {"FL", "FR", "FC"}) {
        const ChannelTaps taps = tapsFor(front, 0, mono);
        CHECK_EQ(taps.count, uint8_t{1});
        CHECK_EQ(taps.taps[0].inputChannel, uint16_t{0});
        // Unity, deliberately not -3 dB: a mono input at 0 dB reads 0 dB on
        // both meters, which is what setting a level implies.
        CHECK_NEAR(taps.taps[0].gain, 1.0, 1e-6);
    }

    // ...but not into the LFE or the rears.
    CHECK_EQ(tapsFor("LFE", 3, mono).count, uint8_t{0});
    CHECK_EQ(tapsFor("RL", 4, mono).count, uint8_t{0});
}

// ------------------------------------------------ positionless (AUX) devices --

// A Focusrite in its "Pro Audio" profile advertises AUX0..AUXn. There is no
// position information to match on, so fall back to index - which is exactly
// what the stereo-only engine's straight interleaved copy did.
void testAuxOutputFallsBackToIndexMatching() {
    const std::vector<std::string> stereo = {"FL", "FR"};

    const ChannelTaps first = tapsFor("AUX0", 0, stereo);
    CHECK_EQ(first.count, uint8_t{1});
    CHECK_EQ(first.taps[0].inputChannel, uint16_t{0});

    const ChannelTaps second = tapsFor("AUX1", 1, stereo);
    CHECK_EQ(second.count, uint8_t{1});
    CHECK_EQ(second.taps[0].inputChannel, uint16_t{1});

    // Index past the end of the input: silence, not a wrapped-around channel.
    CHECK_EQ(tapsFor("AUX2", 2, stereo).count, uint8_t{0});
    CHECK_EQ(tapsFor("AUX3", 3, stereo).count, uint8_t{0});
}

void testUnknownOutputPositionAlsoFallsBackToIndex() {
    const std::vector<std::string> stereo = {"FL", "FR"};
    const ChannelTaps taps = buildTaps(SPA_AUDIO_CHANNEL_UNKNOWN, 1, positions(stereo), SendSpec{});
    CHECK_EQ(taps.count, uint8_t{1});
    CHECK_EQ(taps.taps[0].inputChannel, uint16_t{1});
}

// An AUX-labelled INPUT into a positioned output has nothing to match, so it
// stays silent rather than being guessed into the fronts.
void testAuxInputIntoPositionedOutputIsSilent() {
    const std::vector<std::string> aux = {"AUX0", "AUX1"};
    CHECK_EQ(tapsFor("FL", 0, aux).count, uint8_t{0});
    CHECK_EQ(tapsFor("FR", 1, aux).count, uint8_t{0});
}

// A single positionless input channel is treated as mono - the common case of a
// 1-channel sink that never declared a layout.
void testSinglePositionlessInputBehavesAsMono() {
    const std::vector<std::string> one = {"AUX0"};
    CHECK_EQ(tapsFor("FL", 0, one).count, uint8_t{1});
    CHECK_EQ(tapsFor("FR", 1, one).count, uint8_t{1});
    CHECK_EQ(tapsFor("RL", 4, one).count, uint8_t{0});
}

// ------------------------------------------------------------- send handling --

void testSendGainIsSeparateFromTaps() {
    SendSpec spec;
    spec.gainDb = -6.0;
    const ChannelTaps taps = tapsFor("FL", 0, {"FL", "FR"}, spec);

    // The level lives in sendGain; the tap stays at the position coefficient.
    CHECK_NEAR(taps.taps[0].gain, 1.0, 1e-6);
    CHECK_NEAR(taps.sendGain, 0.5011872336272722, 1e-6);
    CHECK(taps.audible());
}

void testUnitySendGain() {
    const ChannelTaps taps = tapsFor("FL", 0, {"FL", "FR"});
    CHECK_NEAR(taps.sendGain, 1.0, 1e-6);
}

// A disabled send keeps its taps with sendGain 0, so the realtime thread can
// RAMP it out instead of hard-cutting - which is the difference between a fade
// and a click.
void testDisabledSendKeepsTapsWithZeroGain() {
    SendSpec spec;
    spec.enabled = false;
    const ChannelTaps taps = tapsFor("FL", 0, {"FL", "FR"}, spec);

    CHECK_EQ(taps.count, uint8_t{1});
    CHECK_NEAR(taps.sendGain, 0.0, 1e-12);
    CHECK(taps.routed());
    CHECK(!taps.audible());
}

// And routing must not depend on the level, or turning a send down to silence
// would change which channels it feeds when turned back up.
void testTapCountIsIndependentOfLevel() {
    for (double db : {-120.0, -60.0, 0.0, 12.0}) {
        SendSpec spec;
        spec.gainDb = db;
        CHECK_EQ(tapsFor("FL", 0, {"FL", "FR"}, spec).count, uint8_t{1});
        CHECK_EQ(tapsFor("RL", 4, {"FL", "FR"}, spec).count, uint8_t{0});
    }
}

// ------------------------------------------------------------ degenerate input --

void testEmptyInputLayoutIsSilent() {
    const ChannelTaps taps = buildTaps(SPA_AUDIO_CHANNEL_FL, 0, {}, SendSpec{});
    CHECK_EQ(taps.count, uint8_t{0});
}

// --------------------------------------------------------- whole-output form --

void testBuildOutputTapsMarksAnyTaps() {
    const std::vector<uint32_t> out = positions({"FL", "FR", "RL", "RR"});
    const std::vector<uint32_t> stereoIn = positions({"FL", "FR"});
    std::array<ChannelTaps, kMaxOutputChannels> taps{};
    bool anyTaps = false;

    const std::vector<SendSpec> sends(out.size(), SendSpec{});
    buildOutputTaps(out, stereoIn, sends, taps, anyTaps);

    CHECK(anyTaps);
    CHECK_EQ(taps[0].count, uint8_t{1});
    CHECK_EQ(taps[1].count, uint8_t{1});
    CHECK_EQ(taps[2].count, uint8_t{0});
    CHECK_EQ(taps[3].count, uint8_t{0});
}

// anyTaps false is what lets the RT thread skip the ring-buffer read entirely,
// so it must be false when nothing is audible - including when the only routed
// channels have their send disabled.
void testBuildOutputTapsAnyTapsFalseWhenNothingAudible() {
    const std::vector<uint32_t> out = positions({"RL", "RR"});
    const std::vector<uint32_t> stereoIn = positions({"FL", "FR"});
    std::array<ChannelTaps, kMaxOutputChannels> taps{};
    bool anyTaps = true;

    const std::vector<SendSpec> sends(out.size(), SendSpec{});
    buildOutputTaps(out, stereoIn, sends, taps, anyTaps);
    CHECK(!anyTaps); // no position match at all

    const std::vector<uint32_t> frontOut = positions({"FL", "FR"});
    SendSpec disabled;
    disabled.enabled = false;
    const std::vector<SendSpec> disabledSends(frontOut.size(), disabled);
    anyTaps = true;
    buildOutputTaps(frontOut, stereoIn, disabledSends, taps, anyTaps);
    CHECK(!anyTaps); // routed, but silent
    CHECK(taps[0].routed());
}

// Per-channel send levels: the same input can arrive at different levels on
// different channels of one output.
void testBuildOutputTapsHonoursPerChannelSendLevels() {
    const std::vector<uint32_t> out = positions({"FL", "FR"});
    const std::vector<uint32_t> in = positions({"FL", "FR"});
    std::vector<SendSpec> sends(2);
    sends[0].gainDb = 0.0;
    sends[1].gainDb = -6.0;

    std::array<ChannelTaps, kMaxOutputChannels> taps{};
    bool anyTaps = false;
    buildOutputTaps(out, in, sends, taps, anyTaps);

    CHECK_NEAR(taps[0].sendGain, 1.0, 1e-6);
    CHECK_NEAR(taps[1].sendGain, 0.5011872336272722, 1e-6);
}

void testBuildOutputTapsClampsToMaxChannels() {
    std::vector<uint32_t> tooMany(kMaxOutputChannels + 8, SPA_AUDIO_CHANNEL_FL);
    const std::vector<uint32_t> in = positions({"FL", "FR"});
    std::array<ChannelTaps, kMaxOutputChannels> taps{};
    bool anyTaps = false;

    // Must not write past the fixed-capacity array.
    buildOutputTaps(tooMany, in, std::vector<SendSpec>(tooMany.size()), taps, anyTaps);
    CHECK(anyTaps);
}

void testPositionValuesClampsAndConverts() {
    const std::vector<uint32_t> values = mix::positionValues({"FL", "FR", "nonsense"});
    CHECK_EQ(values.size(), std::size_t{3});
    CHECK_EQ(values[0], static_cast<uint32_t>(SPA_AUDIO_CHANNEL_FL));
    CHECK_EQ(values[2], static_cast<uint32_t>(SPA_AUDIO_CHANNEL_UNKNOWN));

    const std::vector<std::string> tooMany(kMaxOutputChannels + 5, "FL");
    CHECK_EQ(mix::positionValues(tooMany).size(), kMaxOutputChannels);
}

} // namespace

int main() {
    RUN(testStereoIntoStereoIsOneTapEach);
    RUN(testStereoIntoSurroundFeedsOnlyTheFronts);
    RUN(testSurroundIntoStereoPassesFrontsOnly);
    RUN(testMatchingIsByPositionNotByIndex);

    RUN(testMonoInputFeedsFrontsAtUnity);

    RUN(testAuxOutputFallsBackToIndexMatching);
    RUN(testUnknownOutputPositionAlsoFallsBackToIndex);
    RUN(testAuxInputIntoPositionedOutputIsSilent);
    RUN(testSinglePositionlessInputBehavesAsMono);

    RUN(testSendGainIsSeparateFromTaps);
    RUN(testUnitySendGain);
    RUN(testDisabledSendKeepsTapsWithZeroGain);
    RUN(testTapCountIsIndependentOfLevel);

    RUN(testEmptyInputLayoutIsSilent);

    RUN(testBuildOutputTapsMarksAnyTaps);
    RUN(testBuildOutputTapsAnyTapsFalseWhenNothingAudible);
    RUN(testBuildOutputTapsHonoursPerChannelSendLevels);
    RUN(testBuildOutputTapsClampsToMaxChannels);
    RUN(testPositionValuesClampsAndConverts);

    return pipeeq::test::summary("mix_plan");
}
