#include "mix_plan.h"

#include <algorithm>
#include <cmath>

#include <spa/param/audio/raw.h>

#include "channel_layout.h"

namespace pipeeq::mix {

namespace {

// Below this a tap contributes nothing anyone can hear, and keeping it would
// just cost a multiply-add per frame forever.
constexpr float kNegligibleGain = 1.0e-6f; // -120 dB

float dbToLinear(double db) {
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

bool isFront(uint32_t position) {
    return position == SPA_AUDIO_CHANNEL_FL || position == SPA_AUDIO_CHANNEL_FR ||
           position == SPA_AUDIO_CHANNEL_FC || position == SPA_AUDIO_CHANNEL_MONO;
}

// A position we can't reason about positionally: either genuinely unknown, or
// an AUX channel, which by definition carries no layout meaning.
bool isPositionless(uint32_t position) {
    return position == SPA_AUDIO_CHANNEL_UNKNOWN || SPA_AUDIO_CHANNEL_IS_AUX(position);
}

void addTap(ChannelTaps& taps, std::size_t inputChannel, float gain) {
    if (std::fabs(gain) < kNegligibleGain) {
        return;
    }
    if (taps.count >= kMaxTapsPerChannel) {
        return; // caller-visible via count; deliberately silent per-tap
    }
    taps.taps[taps.count] = MixTap{static_cast<uint16_t>(inputChannel), gain};
    ++taps.count;
}

} // namespace

ChannelTaps buildTaps(uint32_t outputPosition, std::size_t outputChannelIndex,
                       std::span<const uint32_t> inputPositions, const SendSpec& spec) {
    ChannelTaps taps;
    taps.sendGain = spec.enabled ? dbToLinear(spec.gainDb) : 0.0f;

    if (inputPositions.empty()) {
        return taps;
    }

    // 1. Exact position match. The overwhelmingly common case: a stereo input's
    //    FL feeds the output's FL, whatever else either side has.
    for (std::size_t i = 0; i < inputPositions.size(); ++i) {
        if (inputPositions[i] == outputPosition && !isPositionless(outputPosition)) {
            addTap(taps, i, 1.0f);
            return taps;
        }
    }

    // 2. A mono input feeds every front position at UNITY.
    //    Not -3 dB: a mono input at 0 dB should read 0 dB on both meters, which
    //    is what a user setting a level expects. Energy-preserving fold-down is
    //    a downmix concern, not a mono-source one.
    if (inputPositions.size() == 1 && (inputPositions[0] == SPA_AUDIO_CHANNEL_MONO ||
                                        isPositionless(inputPositions[0]))) {
        if (isFront(outputPosition)) {
            addTap(taps, 0, 1.0f);
        }
        return taps;
    }

    // 3. Positionless output channel: fall back to matching by index. This
    //    reproduces exactly what the stereo-only engine did - a straight
    //    interleaved sample-index copy - so a device whose layout PipeEQ can't
    //    read keeps sounding the same as it always did.
    if (isPositionless(outputPosition)) {
        if (outputChannelIndex < inputPositions.size()) {
            addTap(taps, outputChannelIndex, 1.0f);
        }
        return taps;
    }

    // 4. No match under SendMode::Direct: silence. A 5.1 input into a stereo
    //    output feeds FL/FR only; a stereo input into a 4.0 output leaves
    //    RL/RR silent rather than duplicating the fronts into them.
    if (spec.mode == SendMode::Direct) {
        return taps;
    }

    // 5. SendMode::Downmix - a later phase. Until then it behaves as Direct,
    //    which is the safe reading of "we don't fold anything yet".
    return taps;
}

void buildOutputTaps(std::span<const uint32_t> outputPositions,
                      std::span<const uint32_t> inputPositions,
                      std::span<const SendSpec> perChannelSends,
                      std::array<ChannelTaps, kMaxOutputChannels>& out, bool& anyTaps) {
    out = {};
    anyTaps = false;

    const std::size_t channels = std::min(outputPositions.size(), kMaxOutputChannels);
    for (std::size_t ch = 0; ch < channels; ++ch) {
        const SendSpec spec = ch < perChannelSends.size() ? perChannelSends[ch] : SendSpec{};
        out[ch] = buildTaps(outputPositions[ch], ch, inputPositions, spec);
        anyTaps = anyTaps || out[ch].audible();
    }
}

std::vector<uint32_t> positionValues(const std::vector<std::string>& names) {
    std::vector<uint32_t> values;
    const std::size_t count = std::min(names.size(), kMaxOutputChannels);
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        values.push_back(layout::positionValue(names[i]));
    }
    return values;
}

} // namespace pipeeq::mix
