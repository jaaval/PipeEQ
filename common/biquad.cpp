#include "biquad.h"

#include <cmath>
#include <complex>

namespace eqcore {

BiquadCoeffs computeBiquadCoeffs(FilterType type, double freqHz, double sampleRateHz,
                                  double gainDb, double q) {
    const double w0 = 2.0 * M_PI * freqHz / sampleRateHz;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double alpha = sinW0 / (2.0 * q);
    const double a = std::pow(10.0, gainDb / 40.0);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (type) {
        case FilterType::Peaking:
            b0 = 1.0 + alpha * a;
            b1 = -2.0 * cosW0;
            b2 = 1.0 - alpha * a;
            a0 = 1.0 + alpha / a;
            a1 = -2.0 * cosW0;
            a2 = 1.0 - alpha / a;
            break;

        case FilterType::LowShelf: {
            const double sqrtA = std::sqrt(a);
            b0 = a * ((a + 1.0) - (a - 1.0) * cosW0 + 2.0 * sqrtA * alpha);
            b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosW0);
            b2 = a * ((a + 1.0) - (a - 1.0) * cosW0 - 2.0 * sqrtA * alpha);
            a0 = (a + 1.0) + (a - 1.0) * cosW0 + 2.0 * sqrtA * alpha;
            a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosW0);
            a2 = (a + 1.0) + (a - 1.0) * cosW0 - 2.0 * sqrtA * alpha;
            break;
        }

        case FilterType::HighShelf: {
            const double sqrtA = std::sqrt(a);
            b0 = a * ((a + 1.0) + (a - 1.0) * cosW0 + 2.0 * sqrtA * alpha);
            b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW0);
            b2 = a * ((a + 1.0) + (a - 1.0) * cosW0 - 2.0 * sqrtA * alpha);
            a0 = (a + 1.0) - (a - 1.0) * cosW0 + 2.0 * sqrtA * alpha;
            a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosW0);
            a2 = (a + 1.0) - (a - 1.0) * cosW0 - 2.0 * sqrtA * alpha;
            break;
        }

        case FilterType::LowPass:
            b0 = (1.0 - cosW0) / 2.0;
            b1 = 1.0 - cosW0;
            b2 = (1.0 - cosW0) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosW0;
            a2 = 1.0 - alpha;
            break;

        case FilterType::HighPass:
            b0 = (1.0 + cosW0) / 2.0;
            b1 = -(1.0 + cosW0);
            b2 = (1.0 + cosW0) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosW0;
            a2 = 1.0 - alpha;
            break;
    }

    BiquadCoeffs out;
    out.b0 = b0 / a0;
    out.b1 = b1 / a0;
    out.b2 = b2 / a0;
    out.a1 = a1 / a0;
    out.a2 = a2 / a0;
    return out;
}

double biquadResponseDb(const BiquadCoeffs& coeffs, double freqHz, double sampleRateHz) {
    const double w = 2.0 * M_PI * freqHz / sampleRateHz;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;

    const std::complex<double> num = coeffs.b0 + coeffs.b1 * z1 + coeffs.b2 * z2;
    const std::complex<double> den = 1.0 + coeffs.a1 * z1 + coeffs.a2 * z2;
    const double magnitude = std::abs(num / den);

    return 20.0 * std::log10(magnitude);
}

float BiquadState::process(const BiquadCoeffs& c, float input) {
    const double out = c.b0 * input + c.b1 * x1_ + c.b2 * x2_ - c.a1 * y1_ - c.a2 * y2_;
    x2_ = x1_;
    x1_ = input;
    y2_ = y1_;
    y1_ = out;
    return static_cast<float>(out);
}

void BiquadState::reset() {
    x1_ = x2_ = y1_ = y2_ = 0.0;
}

} // namespace eqcore
