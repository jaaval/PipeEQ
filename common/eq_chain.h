#pragma once

#include <cstddef>
#include <vector>

#include "eq_band.h"

namespace eqcore {

// A cascade of EqBands applied identically to every channel of a route.
// Coefficients are shared across channels; filter history (state) is per-channel
// so channels don't bleed into each other.
class EqChain {
public:
    explicit EqChain(int numChannels = 2, double sampleRateHz = 48000.0);

    void setSampleRate(double sampleRateHz);
    void setNumChannels(int numChannels);

    void setBandCount(std::size_t count);
    std::size_t bandCount() const { return bands_.size(); }

    void setBand(std::size_t index, const EqBand& band);
    const EqBand& band(std::size_t index) const { return bands_.at(index); }

    // Runs one sample for the given channel through the full cascade.
    float processSample(int channel, float input);

    // Clears filter history (e.g. after a discontinuity) without changing bands.
    void reset();

    // Combined magnitude response of the whole cascade at freqHz, in dB.
    double frequencyResponseDb(double freqHz) const;

private:
    void rebuildCoeffs(std::size_t index);

    double sampleRateHz_;
    std::vector<EqBand> bands_;
    std::vector<BiquadCoeffs> coeffs_;
    std::vector<std::vector<BiquadState>> state_; // state_[channel][band]
};

} // namespace eqcore
