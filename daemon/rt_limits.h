#pragma once

#include <cstddef>

#include "app_config.h"

namespace pipeeq {

// Fixed capacities for every realtime-owned buffer.
//
// Everything on the RT path is sized by these at construction and NEVER by a
// negotiated channel count. That is precisely what makes it safe for
// param_changed() to report a different layout than we asked for: there is
// nothing left to resize on the realtime path, so a surprise format is a
// clamped loop bound rather than a reallocation in process().
inline constexpr std::size_t kMaxOutputChannels = 32; // covers an 18i20; 32 x 16 biquads is ~16 KB
inline constexpr std::size_t kMaxInputChannels = 12;  // 7.1.4 is the realistic ceiling for a sink
inline constexpr std::size_t kMaxInputs = 8;          // sends per output; unchanged from v1
inline constexpr std::size_t kMaxBands = eqcore::kMaxBands;
// One tap per input channel is the worst case: a full N->1 downmix fold.
inline constexpr std::size_t kMaxTapsPerChannel = kMaxInputChannels;
// Generous enough that process() never needs to grow anything; frames per
// callback is clamped to this as a safety net rather than ever reallocating.
inline constexpr std::size_t kScratchCapacityFrames = 8192;

} // namespace pipeeq
