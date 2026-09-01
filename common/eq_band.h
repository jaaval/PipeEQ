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

    // Exact comparison on purpose: this is used to check that a config
    // survives a save/load round trip bit-for-bit, where "close enough" would
    // hide a lossy serialization.
    bool operator==(const EqBand&) const = default;
};

} // namespace eqcore
