#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rt_limits.h"

namespace pipeeq::mix {

// How one input channel contributes to one output channel. `gain` is the
// POSITION coefficient only - 1.0 for a direct match, or a fold-down
// coefficient for a downmix leg. The user's send level is deliberately NOT
// folded in here; see ChannelTaps::sendGain.
struct MixTap {
    uint16_t inputChannel = 0;
    float gain = 0.0f;

    bool operator==(const MixTap&) const = default;
};

// How one input feeds ONE output channel.
//
// The split between `taps` (the static position mapping) and `sendGain` (the
// user's level) is what lets the realtime thread ramp a fader or a send mute in
// one multiply per frame rather than ramping every tap independently: 32x8
// ramped scalars instead of 32x8x12.
struct ChannelTaps {
    uint8_t count = 0;         // 0 => this input is not routed to this channel
    float sendGain = 0.0f;     // linear; 0 when the send is disabled or removed
    std::array<MixTap, kMaxTapsPerChannel> taps{};

    bool routed() const { return count > 0; }
    bool audible() const { return count > 0 && sendGain != 0.0f; }
};

// How an input should be folded into an output.
enum class SendMode : uint8_t {
    // Position match only. Output channels the input has no matching channel
    // for get silence - a stereo input into a 4.0 output leaves the rears
    // alone rather than inventing a rear signal.
    Direct,
    // Fold every input channel into the output's channels using standard
    // downmix coefficients. Implemented in a later phase; the enum exists now
    // so the RT structures and the D-Bus surface don't change when it lands.
    Downmix,
};

// One send's user-controlled settings.
struct SendSpec {
    double gainDb = 0.0;
    bool enabled = true;
    SendMode mode = SendMode::Direct;
    // Later phase: derive an LFE feed by summing the input's front channels.
    bool lfeFromStereo = false;
    double lfeCrossoverHz = 80.0;
};

// Builds the taps feeding ONE output channel from ONE input.
//
// This is the ONLY place channel-position logic exists. The realtime thread
// applies coefficients and knows nothing about FL, LFE or downmixing - which is
// what makes surround downmix and an LFE feed drop-in later: a new SendMode
// branch here, and not one line of output_processor.cpp changes.
//
// `outputPosition` and `inputPositions` are SPA channel position values.
// `outputChannelIndex` is only used by the unknown-position fallback below.
ChannelTaps buildTaps(uint32_t outputPosition, std::size_t outputChannelIndex,
                       std::span<const uint32_t> inputPositions, const SendSpec& spec);

// Whole-output convenience: one ChannelTaps per output channel.
// `perChannelSends` is parallel to `outputPositions`. Sets `anyTaps` to whether
// any channel ended up audible, which the RT thread uses to skip a silent
// input's ring-buffer read entirely.
void buildOutputTaps(std::span<const uint32_t> outputPositions,
                      std::span<const uint32_t> inputPositions,
                      std::span<const SendSpec> perChannelSends,
                      std::array<ChannelTaps, kMaxOutputChannels>& out, bool& anyTaps);

// Converts SPA short names to position values, clamped to kMaxOutputChannels.
std::vector<uint32_t> positionValues(const std::vector<std::string>& names);

} // namespace pipeeq::mix
