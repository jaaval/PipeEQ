#include "eq_response.h"

#include <algorithm>

#include "biquad.h"

namespace eqcore {

double eqResponseDb(std::span<const EqBand> bands, double freqHz, double sampleRateHz) {
    double totalDb = 0.0;
    for (const EqBand& band : bands) {
        totalDb += biquadResponseDb(band.toCoeffs(sampleRateHz), freqHz, sampleRateHz);
    }
    return totalDb;
}

void eqResponseCurveDb(std::span<const EqBand> bands, std::span<const double> freqsHz,
                       double sampleRateHz, std::span<double> outDb) {
    const std::size_t count = std::min(freqsHz.size(), outDb.size());
    std::fill(outDb.begin(), outDb.begin() + static_cast<std::ptrdiff_t>(count), 0.0);

    // Band-outer / point-inner: the coefficients are computed once per band.
    for (const EqBand& band : bands) {
        const BiquadCoeffs coeffs = band.toCoeffs(sampleRateHz);
        for (std::size_t i = 0; i < count; ++i) {
            outDb[i] += biquadResponseDb(coeffs, freqsHz[i], sampleRateHz);
        }
    }
}

} // namespace eqcore
