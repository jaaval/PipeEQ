#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <spa/param/audio/raw.h>

#include "channel_layout.h"

namespace pipeeq {

// Fills in the channel positions of an audio format from a list of SPA channel
// short names.
//
// Without positions, a stream is negotiated with the channel map "aux0,aux1..."
// and ports named playback_0/playback_1: undefined auxiliary channels rather
// than a known layout. Applications and volume UIs then can't tell which
// channel is which, so they show aux sliders, lose balance control, and may
// remix rather than pass the signal through untouched.
//
// Any name SPA doesn't recognize becomes SPA_AUDIO_CHANNEL_UNKNOWN, which is
// the honest thing to advertise for a channel we can't identify - better than
// guessing a position the hardware may not have.
inline void applyChannelPositions(spa_audio_info_raw& info,
                                   const std::vector<std::string>& positions) {
    const std::size_t count =
        std::min<std::size_t>(positions.size(), SPA_AUDIO_MAX_CHANNELS);
    for (std::size_t i = 0; i < count; ++i) {
        info.position[i] = layout::positionValue(positions[i]);
    }
}

// Convenience for a stream whose layout is just "the conventional one for this
// channel count" - a virtual sink being created, or a device that advertises no
// layout of its own.
inline void applyDefaultChannelPositions(spa_audio_info_raw& info, int numChannels) {
    applyChannelPositions(info, layout::defaultPositionsFor(numChannels));
}

} // namespace pipeeq
