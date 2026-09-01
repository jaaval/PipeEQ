#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "app_config.h"

namespace pipeeq {

struct AdoptResult {
    // How many of `channels` the device actually drives. Entries at indices at
    // or beyond this are RETIRED configs, kept so that flipping a profile back
    // restores those channels' gain/EQ/sends instead of resetting them.
    std::size_t liveChannelCount = 0;
    std::size_t appendedChannels = 0; // brand-new, and deliberately silent
    std::size_t retiredChannels = 0;
    bool changed = false; // whether anything about the channel list moved
};

// Reshapes an output's channel list to match the layout its device actually
// advertises, without losing any settings it can preserve.
//
// The matching order matters, and each step exists for a specific failure:
//
//  1. By POSITION NAME. This is what makes a profile flip
//     (4.0 -> stereo -> 4.0) restore the rear channels' EQ rather than
//     silently deleting it - the single most valuable property here.
//  2. Then by INDEX, for any device channel still unmatched. This preserves
//     settings when a device renames its channels wholesale, e.g. a Focusrite
//     switching between a surround profile (FL/FR/RL/RR) and its "Pro Audio"
//     profile (AUX0..AUX3).
//  3. Anything still unmatched becomes a fresh channel with NO SENDS AT ALL.
//     Silent on purpose: after upgrading, a 4.0 interface gains two new strips
//     rather than suddenly playing the mix out of outputs 3 and 4.
//
// Config entries that matched nothing are appended after the live ones as
// retired configs. Link group indices are remapped so grouping survives any
// reordering this does.
AdoptResult adoptDeviceLayout(eqcore::OutputConfig& output,
                               const std::vector<std::string>& devicePositions);

} // namespace pipeeq
