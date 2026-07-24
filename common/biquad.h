#pragma once

namespace eqcore {

enum class FilterType {
    Peaking,
    LowShelf,
    HighShelf,
    LowPass,
    HighPass,
};

// Normalized biquad coefficients (a0 already divided out).
struct BiquadCoeffs {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

// RBJ Audio EQ Cookbook formulas. gainDb only affects Peaking/LowShelf/HighShelf.
BiquadCoeffs computeBiquadCoeffs(FilterType type, double freqHz, double sampleRateHz,
                                  double gainDb, double q);

// Magnitude response of a single biquad at freqHz, in dB.
double biquadResponseDb(const BiquadCoeffs& coeffs, double freqHz, double sampleRateHz);

// Per-channel filter state (history samples) for one biquad stage.
class BiquadState {
public:
    float process(const BiquadCoeffs& c, float input);
    void reset();

private:
    double x1_ = 0.0;
    double x2_ = 0.0;
    double y1_ = 0.0;
    double y2_ = 0.0;
};

} // namespace eqcore
