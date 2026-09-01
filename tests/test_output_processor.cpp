// The realtime DSP core: position routing actually moving audio, fader/mute
// ramping instead of clicking, per-channel EQ, the clamp order, NaN recovery,
// and metering. No PipeWire involved - OutputProcessor deliberately contains no
// PipeWire type, which is what makes this testable at all.

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include <spa/param/audio/raw.h>

#include "channel_layout.h"
#include "mix_plan.h"
#include "output_processor.h"
#include "ring_buffer.h"

#include "check.h"

namespace {

using namespace pipeeq;

constexpr uint32_t kRate = 48000;

float dbToLinear(double db) {
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

// Builds a snapshot for `outputPositions`, with one input slot fed by `buffer`
// whose layout is `inputPositions`, all channels at `gainDb` and no EQ.
std::unique_ptr<OutputSnapshot> makeSnapshot(const std::vector<std::string>& outputPositions,
                                             const std::shared_ptr<const RingBuffer>& buffer,
                                             const std::vector<std::string>& inputPositions,
                                             double gainDb = 0.0, double sendDb = 0.0,
                                             bool sendEnabled = true, bool muted = false) {
    auto snap = std::make_unique<OutputSnapshot>();
    snap->numChannels = static_cast<uint8_t>(outputPositions.size());
    for (std::size_t ch = 0; ch < outputPositions.size(); ++ch) {
        snap->channels[ch].gainLinear = muted ? 0.0f : dbToLinear(gainDb);
    }

    if (buffer) {
        InputMixSlot& slot = snap->inputs[0];
        slot.active = true;
        slot.inputId = "input-1";
        slot.buffer = buffer;
        slot.inputChannels = static_cast<uint16_t>(buffer->numChannels());

        mix::SendSpec spec;
        spec.gainDb = sendDb;
        spec.enabled = sendEnabled;
        const std::vector<mix::SendSpec> sends(outputPositions.size(), spec);
        mix::buildOutputTaps(mix::positionValues(outputPositions), mix::positionValues(inputPositions),
                              sends, slot.perChannel, slot.anyTaps);
    }
    return snap;
}

// Fills a ring buffer with a constant per channel, e.g. {1.0, -1.0}.
//
// Capacity is deliberately larger than what is written, and the default is
// larger than any test consumes: read past the written frames and readAt()
// correctly zero-fills, which silently turns "measured the signal" into
// "measured the silence after it".
std::shared_ptr<RingBuffer> bufferWithConstants(const std::vector<float>& perChannel,
                                                 std::size_t frames = 32768) {
    auto buffer =
        std::make_shared<RingBuffer>(frames * 2 + 16, static_cast<int>(perChannel.size()));
    std::vector<float> block(frames * perChannel.size());
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t ch = 0; ch < perChannel.size(); ++ch) {
            block[f * perChannel.size() + ch] = perChannel[ch];
        }
    }
    buffer->write(block.data(), frames);
    return buffer;
}

std::shared_ptr<RingBuffer> bufferWithSine(double freqHz, int channels, std::size_t frames,
                                            float amplitude = 0.5f) {
    auto buffer = std::make_shared<RingBuffer>(frames * 2 + 16, channels);
    std::vector<float> block(frames * static_cast<std::size_t>(channels));
    for (std::size_t f = 0; f < frames; ++f) {
        const float sample =
            amplitude * static_cast<float>(std::sin(2.0 * M_PI * freqHz * static_cast<double>(f) / kRate));
        for (int ch = 0; ch < channels; ++ch) {
            block[f * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch)] = sample;
        }
    }
    buffer->write(block.data(), frames);
    return buffer;
}

float channelPeak(const std::vector<float>& interleaved, uint32_t numChannels, uint32_t channel) {
    float peak = 0.0f;
    for (std::size_t f = 0; f * numChannels + channel < interleaved.size(); ++f) {
        peak = std::max(peak, std::fabs(interleaved[f * numChannels + channel]));
    }
    return peak;
}

// Runs the processor long enough for a 10 ms fader ramp to complete.
void runBlocks(OutputProcessor& processor, std::vector<float>& dst, uint32_t frames,
                uint32_t numChannels, int blocks) {
    for (int i = 0; i < blocks; ++i) {
        processor.process(dst.data(), frames, numChannels);
    }
}

// ------------------------------------------------------------------ routing --

void testUnpublishedProcessorEmitsSilence() {
    OutputProcessor processor(kRate);
    std::vector<float> dst(128 * 2, 7.0f);
    processor.process(dst.data(), 128, 2);
    for (float sample : dst) {
        CHECK_NEAR(sample, 0.0, 1e-12);
    }
}

// A stereo input into a 4.0 output must reach FL/FR and leave RL/RR silent.
// This is the whole point of position-based routing.
void testStereoInputIntoQuadOutputLeavesRearsSilent() {
    OutputProcessor processor(kRate);
    const std::vector<std::string> quad = {"FL", "FR", "RL", "RR"};
    auto buffer = bufferWithConstants({0.5f, -0.5f});
    processor.publish(makeSnapshot(quad, buffer, {"FL", "FR"}));

    std::vector<float> dst(256 * 4, 0.0f);
    runBlocks(processor, dst, 256, 4, 8); // let the fader ramp finish

    CHECK_NEAR(channelPeak(dst, 4, 0), 0.5, 0.01);
    CHECK_NEAR(channelPeak(dst, 4, 1), 0.5, 0.01);
    CHECK_NEAR(channelPeak(dst, 4, 2), 0.0, 1e-9);
    CHECK_NEAR(channelPeak(dst, 4, 3), 0.0, 1e-9);
}

// Positions, not indices: the input lists its channels in an odd order and the
// audio must still arrive at the right speakers.
void testRoutingFollowsPositionNotIndex() {
    OutputProcessor processor(kRate);
    auto buffer = std::make_shared<RingBuffer>(16384, 2);
    // Channel 0 is FR (value -0.5), channel 1 is FL (value 0.25).
    constexpr std::size_t kFrames = 8192;
    std::vector<float> block(kFrames * 2);
    for (std::size_t f = 0; f < kFrames; ++f) {
        block[f * 2 + 0] = -0.5f;
        block[f * 2 + 1] = 0.25f;
    }
    buffer->write(block.data(), kFrames);

    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FR", "FL"}));
    std::vector<float> dst(256 * 2, 0.0f);
    runBlocks(processor, dst, 256, 2, 8);

    CHECK_NEAR(channelPeak(dst, 2, 0), 0.25, 0.01); // FL got input channel 1
    CHECK_NEAR(channelPeak(dst, 2, 1), 0.5, 0.01);  // FR got input channel 0
}

void testMonoInputReachesBothFronts() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({0.4f});
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"MONO"}));

    std::vector<float> dst(256 * 2, 0.0f);
    runBlocks(processor, dst, 256, 2, 8);

    // Unity into both, not -3 dB.
    CHECK_NEAR(channelPeak(dst, 2, 0), 0.4, 0.01);
    CHECK_NEAR(channelPeak(dst, 2, 1), 0.4, 0.01);
}

void testSendLevelScalesTheContribution() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({0.8f, 0.8f});
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}, 0.0, -6.0));

    std::vector<float> dst(256 * 2, 0.0f);
    runBlocks(processor, dst, 256, 2, 8);

    CHECK_NEAR(channelPeak(dst, 2, 0), 0.8 * 0.5011872336, 0.01);
}

// ------------------------------------------------------------------- faders --

// A mute must slew, not step: a hard multiply-by-zero mid-waveform is a
// full-scale discontinuity and audibly a click.
void testMuteRampsRatherThanStepping() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({1.0f, 1.0f});

    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}));
    std::vector<float> dst(512 * 2, 0.0f);
    runBlocks(processor, dst, 512, 2, 8); // settle at unity

    // Now mute, and watch the very next block.
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}, 0.0, 0.0, true, /*muted=*/true));
    processor.process(dst.data(), 512, 2);

    // The step between consecutive samples must never exceed the slew rate.
    const float maxStep = 1.0f / (static_cast<float>(OutputProcessor::kGainSlewSeconds) * kRate);
    float worstStep = 0.0f;
    for (std::size_t f = 1; f < 512; ++f) {
        worstStep = std::max(worstStep, std::fabs(dst[f * 2] - dst[(f - 1) * 2]));
    }
    CHECK(worstStep <= maxStep * 1.5f);
    // And it must be on its way down rather than still at full level.
    CHECK(std::fabs(dst[511 * 2]) < std::fabs(dst[0]));
}

// ...and it must actually get there, within the advertised 10 ms.
void testMuteReachesSilenceWithinSlewWindow() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({1.0f, 1.0f});
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}));
    std::vector<float> dst(480 * 2, 0.0f); // 10 ms per block at 48 kHz
    runBlocks(processor, dst, 480, 2, 8);

    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}, 0.0, 0.0, true, true));
    processor.process(dst.data(), 480, 2); // one 10 ms block: the whole ramp
    processor.process(dst.data(), 480, 2); // and now fully silent

    CHECK_NEAR(channelPeak(dst, 2, 0), 0.0, 1e-6);
}

// A fresh stream starts at zero gain and fades in, so connecting an output
// mid-waveform isn't a click either.
void testFreshProcessorFadesIn() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({1.0f, 1.0f});
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}));

    std::vector<float> dst(64 * 2, 0.0f);
    processor.process(dst.data(), 64, 2);

    CHECK(std::fabs(dst[0]) < 0.05f);              // starts near silence
    CHECK(std::fabs(dst[63 * 2]) > std::fabs(dst[0])); // and is rising
}

// Turning a send off keeps its taps so the level can be ramped out; the audio
// must fade rather than cut.
void testDisabledSendFadesOut() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({1.0f, 1.0f});
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}));
    std::vector<float> dst(480 * 2, 0.0f);
    runBlocks(processor, dst, 480, 2, 8);

    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}, 0.0, 0.0, /*sendEnabled=*/false));
    processor.process(dst.data(), 480, 2);

    // Within this block it should have descended most of the way, and the first
    // sample must still be close to where the previous block left off.
    CHECK(std::fabs(dst[0]) > 0.5f);
    CHECK(std::fabs(dst[479 * 2]) < 0.1f);

    // The block after that is fully silent, and the slot is then skipped.
    processor.process(dst.data(), 480, 2);
    CHECK_NEAR(channelPeak(dst, 2, 0), 0.0, 1e-6);
}

// ----------------------------------------------------------------- EQ paths --

// Different EQ instances on different channels is the headline per-channel-EQ
// requirement: a 6 dB boost on FL must not touch FR.
void testPerChannelEqIsIndependent() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithSine(1000.0, 2, 48000, 0.25f);

    auto snap = makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"});
    auto boost = std::make_shared<EqCoeffBlock>();
    boost->bandCount = 1;
    boost->coeffs[0] = eqcore::EqBand{eqcore::FilterType::Peaking, 1000.0, 6.0, 1.0}.toCoeffs(kRate);
    snap->channels[0].eq = boost; // FL only
    processor.publish(std::move(snap));

    std::vector<float> dst(4800 * 2, 0.0f);
    runBlocks(processor, dst, 4800, 2, 4); // settle the filter and the fader

    const float leftPeak = channelPeak(dst, 2, 0);
    const float rightPeak = channelPeak(dst, 2, 1);
    CHECK_NEAR(20.0 * std::log10(leftPeak / rightPeak), 6.0, 0.5);
    CHECK_NEAR(rightPeak, 0.25, 0.01);
}

// Two channels sharing one coefficient block must be identical sample for
// sample - that shared-by-pointer arrangement is what makes drift impossible.
void testSharedEqInstanceGivesIdenticalChannels() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithSine(500.0, 2, 48000, 0.3f);

    auto snap = makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"});
    auto shared = std::make_shared<EqCoeffBlock>();
    shared->bandCount = 2;
    shared->coeffs[0] = eqcore::EqBand{eqcore::FilterType::LowShelf, 200.0, 4.0, 0.707}.toCoeffs(kRate);
    shared->coeffs[1] = eqcore::EqBand{eqcore::FilterType::Peaking, 3000.0, -5.0, 1.5}.toCoeffs(kRate);
    snap->channels[0].eq = shared;
    snap->channels[1].eq = shared;
    processor.publish(std::move(snap));

    std::vector<float> dst(2048 * 2, 0.0f);
    runBlocks(processor, dst, 2048, 2, 4);

    for (std::size_t f = 0; f < 2048; ++f) {
        CHECK_NEAR(dst[f * 2 + 0], dst[f * 2 + 1], 1e-7);
    }
}

// The clamp must come AFTER the EQ and the fader, so an EQ boost can't be
// pre-limited by a clamp applied to the raw mix.
void testOutputIsClampedToUnity() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({1.0f, 1.0f});
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}, /*gainDb=*/12.0));

    std::vector<float> dst(512 * 2, 0.0f);
    runBlocks(processor, dst, 512, 2, 8);

    for (float sample : dst) {
        CHECK(sample <= 1.0f);
        CHECK(sample >= -1.0f);
    }
    CHECK_NEAR(channelPeak(dst, 2, 0), 1.0, 1e-6);
}

// ------------------------------------------------------- NaN recovery, meters --

// Without the per-block guard, one NaN would sit in the biquad state forever
// and that channel would be silent until the daemon restarted.
void testNanDoesNotPersistPastOneBlock() {
    OutputProcessor processor(kRate);
    auto buffer = std::make_shared<RingBuffer>(4096, 2);

    auto snap = makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"});
    auto eq = std::make_shared<EqCoeffBlock>();
    eq->bandCount = 1;
    eq->coeffs[0] = eqcore::EqBand{eqcore::FilterType::Peaking, 1000.0, 6.0, 1.0}.toCoeffs(kRate);
    snap->channels[0].eq = eq;
    processor.publish(std::move(snap));

    // Feed one block containing a NaN.
    std::vector<float> poisoned(256 * 2, 0.25f);
    poisoned[10] = std::numeric_limits<float>::quiet_NaN();
    buffer->write(poisoned.data(), 256);

    std::vector<float> dst(256 * 2, 0.0f);
    processor.process(dst.data(), 256, 2);

    // Then clean audio.
    std::vector<float> clean(256 * 2, 0.25f);
    for (int i = 0; i < 8; ++i) {
        buffer->write(clean.data(), 256);
        processor.process(dst.data(), 256, 2);
    }

    for (float sample : dst) {
        CHECK(std::isfinite(sample));
    }
    // And the channel is carrying signal again, not stuck at silence.
    CHECK(channelPeak(dst, 2, 0) > 0.05f);
}

void testMeterTracksSignalThenResets() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({0.5f, 0.25f});
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}));

    std::vector<float> dst(512 * 2, 0.0f);
    runBlocks(processor, dst, 512, 2, 8);

    CHECK_NEAR(processor.takeChannelPeak(0), 0.5, 0.02);
    CHECK_NEAR(processor.takeChannelPeak(1), 0.25, 0.02);
    // Draining resets, so a stopped signal decays instead of holding.
    CHECK_NEAR(processor.takeChannelPeak(0), 0.0, 1e-9);
}

void testMuteReadsSilenceOnTheMeter() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({1.0f, 1.0f});
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}, 0.0, 0.0, true, /*muted=*/true));

    std::vector<float> dst(512 * 2, 0.0f);
    runBlocks(processor, dst, 512, 2, 8);
    CHECK_NEAR(processor.takeChannelPeak(0), 0.0, 1e-6);
}

// ------------------------------------------------- format-surprise tolerance --

// param_changed can report a different channel count than we asked for. Every
// RT buffer is sized for kMaxOutputChannels, so this must clamp rather than
// overrun - and must not resize anything.
void testChannelCountAboveLimitIsClamped() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({0.5f, 0.5f});
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}));

    std::vector<float> dst(64 * (kMaxOutputChannels + 8), 0.0f);
    processor.process(dst.data(), 64, kMaxOutputChannels + 8);
    // Nothing crashed and the samples it did write are finite.
    for (std::size_t i = 0; i < 64 * kMaxOutputChannels; ++i) {
        CHECK(std::isfinite(dst[i]));
    }
}

void testFrameCountAboveScratchIsClamped() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({0.5f, 0.5f});
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}));

    std::vector<float> dst((kScratchCapacityFrames + 512) * 2, 0.0f);
    processor.process(dst.data(), kScratchCapacityFrames + 512, 2);
    for (std::size_t i = 0; i < kScratchCapacityFrames * 2; ++i) {
        CHECK(std::isfinite(dst[i]));
    }
}

// --------------------------------------------------- snapshot publication --

// Retired snapshots must actually be freed once the RT thread has moved past
// them, or a fader drag would accumulate them for the life of the stream.
void testRetiredSnapshotsDrain() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({0.5f, 0.5f});
    std::vector<float> dst(128 * 2, 0.0f);

    for (int i = 0; i < 50; ++i) {
        processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}, -static_cast<double>(i) * 0.1));
        processor.process(dst.data(), 128, 2);
        processor.process(dst.data(), 128, 2);
    }
    // One more publish to drain the last of them.
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}));
    CHECK(processor.retiredSnapshotCount() <= 2);
}

// A snapshot swap between blocks must not reset filter history or ramp state -
// they live outside the snapshot precisely so a gain tweak doesn't restart the
// filters.
void testSnapshotSwapPreservesRampState() {
    OutputProcessor processor(kRate);
    auto buffer = bufferWithConstants({1.0f, 1.0f});
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}));
    std::vector<float> dst(480 * 2, 0.0f);
    runBlocks(processor, dst, 480, 2, 8); // settled at unity

    // Republishing the identical configuration must not audibly restart.
    processor.publish(makeSnapshot({"FL", "FR"}, buffer, {"FL", "FR"}));
    processor.process(dst.data(), 480, 2);
    CHECK_NEAR(std::fabs(dst[0]), 1.0, 0.01); // no fade-in from zero
}

} // namespace

int main() {
    RUN(testUnpublishedProcessorEmitsSilence);
    RUN(testStereoInputIntoQuadOutputLeavesRearsSilent);
    RUN(testRoutingFollowsPositionNotIndex);
    RUN(testMonoInputReachesBothFronts);
    RUN(testSendLevelScalesTheContribution);

    RUN(testMuteRampsRatherThanStepping);
    RUN(testMuteReachesSilenceWithinSlewWindow);
    RUN(testFreshProcessorFadesIn);
    RUN(testDisabledSendFadesOut);

    RUN(testPerChannelEqIsIndependent);
    RUN(testSharedEqInstanceGivesIdenticalChannels);
    RUN(testOutputIsClampedToUnity);

    RUN(testNanDoesNotPersistPastOneBlock);
    RUN(testMeterTracksSignalThenResets);
    RUN(testMuteReadsSilenceOnTheMeter);

    RUN(testChannelCountAboveLimitIsClamped);
    RUN(testFrameCountAboveScratchIsClamped);

    RUN(testRetiredSnapshotsDrain);
    RUN(testSnapshotSwapPreservesRampState);

    return pipeeq::test::summary("output_processor");
}
