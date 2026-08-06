#pragma once

#include <spa/param/audio/raw.h>

namespace pipeeq {

// Fills in the channel positions of an audio format.
//
// Without this, a 2-channel stream is negotiated with no positions at all,
// which PipeWire reports as the channel map "aux0,aux1" and names the ports
// playback_0/playback_1: two undefined auxiliary channels rather than a
// stereo pair. Applications and volume UIs then can't tell which channel is
// left, so they show "aux0/aux1" sliders, lose balance control, and may
// remix rather than pass stereo through untouched.
//
// PipeEQ is deliberately stereo-only (mixing and EQ both assume a channel
// pair), so this only knows about mono and stereo. Anything else is left
// position-less on purpose, which is the honest thing to advertise for a
// layout the rest of the daemon doesn't actually understand.
inline void fillChannelPositions(spa_audio_info_raw& info, int numChannels) {
    if (numChannels == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (numChannels == 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
}

} // namespace pipeeq
