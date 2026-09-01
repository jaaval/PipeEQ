#pragma once

#include <span>

#include "eq_band.h"

namespace eqcore {

// Combined magnitude response of a band list, in dB. Free functions rather than
// a class because the response is a pure function of the bands: nothing here
// owns filter state, so the GUI can draw any number of curves (a channel's own
// EQ plus its siblings as ghosts) without constructing anything per curve.
//
// This is deliberately the same math the daemon applies - both go through
// EqBand::toCoeffs() and biquadResponseDb() - so a drawn curve is always the
// curve actually being applied, provided the caller passes the sample rate the
// daemon negotiated rather than assuming one.
double eqResponseDb(std::span<const EqBand> bands, double freqHz, double sampleRateHz);

// Batched form: one coefficient computation per band instead of one per
// (band, point). `outDb` must be at least as long as `freqsHz`; only the first
// freqsHz.size() entries are written. This is what the curve widget wants -
// a wide plot is ~500 points and recomputing coefficients at each would be
// ~500x more work than it needs to be.
void eqResponseCurveDb(std::span<const EqBand> bands, std::span<const double> freqsHz,
                       double sampleRateHz, std::span<double> outDb);

} // namespace eqcore
