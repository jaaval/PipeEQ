#pragma once

namespace pipeeq::taper {

// Maps a dB value to a 0..1 fader position and back.
//
// Not linear in dB. The old UI used a plain QSlider over -60..+12 integer dB,
// which gives the -12..0 region - where essentially all real adjustment happens
// - only 17% of the travel, in 1 dB steps. This is a console-style piecewise
// taper that gives the top 40 dB about 80% of the travel, so a fader can be set
// meaningfully by hand near unity.
//
// Pure functions in their own translation unit specifically so they can be unit
// tested and, more importantly, SHARED between the fader and the meter. A large
// part of why a mixer strip reads well is that 0 dB sits at the same height on
// both; two independent mappings drift apart the first time either is tweaked.

// Below this, a fader is at its -inf detent and the channel is silent.
inline constexpr double kMinDb = -65.0;
inline constexpr double kMaxDb = 12.0;
inline constexpr double kUnityDb = 0.0;
// The bottom slice of the travel is a hard-off detent rather than -65 dB, so
// "all the way down" means silence rather than very quiet.
inline constexpr double kSilenceDb = -144.0;
inline constexpr double kSilenceNorm = 0.0;

// dB -> 0..1. Values at or below kMinDb map to 0; above kMaxDb clamps to 1.
double dbToNorm(double db);

// 0..1 -> dB. Exactly 0 returns kSilenceDb, so the detent is reachable.
double normToDb(double norm);

// True for a level that should be treated as silent rather than merely quiet.
bool isSilent(double db);

} // namespace pipeeq::taper
