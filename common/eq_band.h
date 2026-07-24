#pragma once

#include "biquad.h"

namespace eqcore {

// One parametric EQ band as controlled from the GUI / stored in config.
struct EqBand {
    FilterType type = FilterType::Peaking;
    double freqHz = 1000.0;
    double gainDb = 0.0;
    double q = 0.707;

    BiquadCoeffs toCoeffs(double sampleRateHz) const {
        return computeBiquadCoeffs(type, freqHz, sampleRateHz, gainDb, q);
    }
};

} // namespace eqcore
