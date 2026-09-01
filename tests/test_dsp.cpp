// Sanity checks for the biquad DSP math: that computed frequency responses
// match what the RBJ cookbook formulas are supposed to produce, and that a
// cascade stays stable over a real signal. No audio hardware involved, so this
// runs anywhere (including inside WSL with no PipeWire graph).
//
// Moved from the old common/selftest.cpp. The EqChain-based cases now go
// through eqcore::eqResponseDb and a bare BiquadState cascade, which is what
// the daemon and the GUI actually use since EqChain was removed.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "biquad.h"
#include "eq_band.h"
#include "eq_response.h"

#include "check.h"

namespace {

constexpr double kRate = 48000.0;

void testPeakingBandGainAtCenter() {
    constexpr double freq = 1000.0;
    constexpr double gainDb = 6.0;
    const auto coeffs =
        eqcore::computeBiquadCoeffs(eqcore::FilterType::Peaking, freq, kRate, gainDb, 1.0);

    CHECK_NEAR(eqcore::biquadResponseDb(coeffs, freq, kRate), gainDb, 0.2);
    CHECK_NEAR(eqcore::biquadResponseDb(coeffs, 20.0, kRate), 0.0, 0.5);
    CHECK_NEAR(eqcore::biquadResponseDb(coeffs, 20000.0, kRate), 0.0, 0.5);
}

void testLowShelf() {
    constexpr double gainDb = 9.0;
    const auto coeffs =
        eqcore::computeBiquadCoeffs(eqcore::FilterType::LowShelf, 1000.0, kRate, gainDb, 0.707);

    CHECK_NEAR(eqcore::biquadResponseDb(coeffs, 20.0, kRate), gainDb, 0.5);
    CHECK_NEAR(eqcore::biquadResponseDb(coeffs, 20000.0, kRate), 0.0, 1.0);
}

void testHighShelf() {
    constexpr double gainDb = -9.0;
    const auto coeffs =
        eqcore::computeBiquadCoeffs(eqcore::FilterType::HighShelf, 1000.0, kRate, gainDb, 0.707);

    CHECK_NEAR(eqcore::biquadResponseDb(coeffs, 20.0, kRate), 0.0, 1.0);
    CHECK_NEAR(eqcore::biquadResponseDb(coeffs, 20000.0, kRate), gainDb, 1.0);
}

void testLowPassAttenuatesHighs() {
    const auto coeffs =
        eqcore::computeBiquadCoeffs(eqcore::FilterType::LowPass, 1000.0, kRate, 0.0, 0.707);

    const double lowDb = eqcore::biquadResponseDb(coeffs, 100.0, kRate);
    const double highDb = eqcore::biquadResponseDb(coeffs, 10000.0, kRate);
    CHECK(lowDb > highDb + 20.0);
}

void testHighPassAttenuatesLows() {
    const auto coeffs =
        eqcore::computeBiquadCoeffs(eqcore::FilterType::HighPass, 1000.0, kRate, 0.0, 0.707);

    const double lowDb = eqcore::biquadResponseDb(coeffs, 100.0, kRate);
    const double highDb = eqcore::biquadResponseDb(coeffs, 10000.0, kRate);
    CHECK(highDb > lowDb + 20.0);
}

void testEqResponseCombinesBandsAdditively() {
    const std::vector<eqcore::EqBand> bands = {
        {eqcore::FilterType::Peaking, 200.0, 4.0, 1.0},
        {eqcore::FilterType::Peaking, 5000.0, -3.0, 1.0},
    };

    const double combined = eqcore::eqResponseDb(bands, 200.0, kRate);
    const double perBandSum = eqcore::biquadResponseDb(bands[0].toCoeffs(kRate), 200.0, kRate) +
                              eqcore::biquadResponseDb(bands[1].toCoeffs(kRate), 200.0, kRate);

    CHECK_NEAR(combined, perBandSum, 0.01);
}

void testEqResponseEmptyIsFlat() {
    CHECK_NEAR(eqcore::eqResponseDb({}, 1000.0, kRate), 0.0, 1e-12);
}

// The batched form is what the curve widget calls, so it must agree exactly
// with the scalar form rather than merely closely.
void testEqResponseCurveMatchesScalar() {
    const std::vector<eqcore::EqBand> bands = {
        {eqcore::FilterType::LowShelf, 120.0, 3.0, 0.707},
        {eqcore::FilterType::Peaking, 2200.0, -5.5, 1.4},
        {eqcore::FilterType::HighPass, 35.0, 0.0, 0.707},
    };
    const std::vector<double> freqs = {20.0, 60.0, 120.0, 480.0, 1000.0, 2200.0, 8000.0, 20000.0};

    std::vector<double> batched(freqs.size(), 0.0);
    eqcore::eqResponseCurveDb(bands, freqs, kRate, batched);

    for (std::size_t i = 0; i < freqs.size(); ++i) {
        CHECK_NEAR(batched[i], eqcore::eqResponseDb(bands, freqs[i], kRate), 1e-9);
    }
}

void testEqResponseCurveHonoursShortOutput() {
    const std::vector<eqcore::EqBand> bands = {{eqcore::FilterType::Peaking, 1000.0, 6.0, 1.0}};
    const std::vector<double> freqs = {100.0, 1000.0, 10000.0};

    // A caller passing a shorter output span must get the first N filled and
    // no write past the end - checked by the sentinel staying untouched.
    std::array<double, 2> out{{-999.0, -999.0}};
    eqcore::eqResponseCurveDb(bands, freqs, kRate, std::span<double>(out.data(), 1));
    CHECK_NEAR(out[0], eqcore::eqResponseDb(bands, 100.0, kRate), 1e-9);
    CHECK_NEAR(out[1], -999.0, 1e-12);
}

// Replaces the old EqChain::processSample finiteness check with the cascade
// shape the daemon actually runs: one BiquadState per band, in series.
void testCascadeStaysFiniteOverSineBurst() {
    const std::vector<eqcore::EqBand> bands = {
        {eqcore::FilterType::Peaking, 1000.0, 6.0, 1.0},
        {eqcore::FilterType::HighShelf, 8000.0, -4.0, 0.707},
    };
    std::vector<eqcore::BiquadCoeffs> coeffs;
    for (const auto& band : bands) {
        coeffs.push_back(band.toCoeffs(kRate));
    }
    std::vector<eqcore::BiquadState> state(bands.size());

    bool allFinite = true;
    for (int n = 0; n < 4800; ++n) {
        float sample = static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * n / kRate));
        for (std::size_t i = 0; i < coeffs.size(); ++i) {
            sample = state[i].process(coeffs[i], sample);
        }
        if (!std::isfinite(sample)) {
            allFinite = false;
            break;
        }
    }
    CHECK(allFinite);
}

// A 6 dB peaking band should measurably raise a sine at its center frequency by
// ~6 dB. This ties the frequency-response math to the time-domain filter, which
// nothing did before: a sign error in BiquadState would pass every response
// check above.
void testCascadeAmplitudeMatchesPredictedResponse() {
    const eqcore::EqBand band{eqcore::FilterType::Peaking, 1000.0, 6.0, 1.0};
    const eqcore::BiquadCoeffs coeffs = band.toCoeffs(kRate);
    eqcore::BiquadState state;

    double peak = 0.0;
    const int total = 9600;
    for (int n = 0; n < total; ++n) {
        const float in = static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * n / kRate));
        const float out = state.process(coeffs, in);
        if (n > total / 2) { // let the filter settle before measuring
            peak = std::max(peak, static_cast<double>(std::fabs(out)));
        }
    }

    const double measuredDb = 20.0 * std::log10(peak);
    CHECK_NEAR(measuredDb, 6.0, 0.3);
}

} // namespace

int main() {
    RUN(testPeakingBandGainAtCenter);
    RUN(testLowShelf);
    RUN(testHighShelf);
    RUN(testLowPassAttenuatesHighs);
    RUN(testHighPassAttenuatesLows);
    RUN(testEqResponseCombinesBandsAdditively);
    RUN(testEqResponseEmptyIsFlat);
    RUN(testEqResponseCurveMatchesScalar);
    RUN(testEqResponseCurveHonoursShortOutput);
    RUN(testCascadeStaysFiniteOverSineBurst);
    RUN(testCascadeAmplitudeMatchesPredictedResponse);
    return pipeeq::test::summary("dsp");
}
