// Self-contained sanity checks for the biquad/EqChain DSP math. No audio
// hardware involved - this only checks that computed frequency responses
// match what the RBJ cookbook formulas are supposed to produce, so it can
// run anywhere (including inside WSL with no real PipeWire graph).

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "biquad.h"
#include "eq_chain.h"

namespace {

int g_failures = 0;

void expectNear(double actual, double expected, double tolerance, const char* what) {
    if (std::abs(actual - expected) > tolerance) {
        std::fprintf(stderr, "FAIL: %s - expected %.3f, got %.3f (tolerance %.3f)\n", what, expected,
                     actual, tolerance);
        ++g_failures;
    } else {
        std::printf("ok: %s (%.3f ~= %.3f)\n", what, actual, expected);
    }
}

void testPeakingBandGainAtCenter() {
    constexpr double sampleRate = 48000.0;
    constexpr double freq = 1000.0;
    constexpr double gainDb = 6.0;

    auto coeffs = eqcore::computeBiquadCoeffs(eqcore::FilterType::Peaking, freq, sampleRate, gainDb, 1.0);

    expectNear(eqcore::biquadResponseDb(coeffs, freq, sampleRate), gainDb, 0.2,
               "peaking band: response at center frequency equals configured gain");
    expectNear(eqcore::biquadResponseDb(coeffs, 20.0, sampleRate), 0.0, 0.5,
               "peaking band: negligible effect far below center frequency");
    expectNear(eqcore::biquadResponseDb(coeffs, 20000.0, sampleRate), 0.0, 0.5,
               "peaking band: negligible effect far above center frequency");
}

void testLowShelf() {
    constexpr double sampleRate = 48000.0;
    constexpr double freq = 1000.0;
    constexpr double gainDb = 9.0;

    auto coeffs = eqcore::computeBiquadCoeffs(eqcore::FilterType::LowShelf, freq, sampleRate, gainDb, 0.707);

    expectNear(eqcore::biquadResponseDb(coeffs, 20.0, sampleRate), gainDb, 0.5,
               "low shelf: near-DC gain matches shelf gain");
    expectNear(eqcore::biquadResponseDb(coeffs, 20000.0, sampleRate), 0.0, 1.0,
               "low shelf: high frequencies unaffected");
}

void testHighShelf() {
    constexpr double sampleRate = 48000.0;
    constexpr double freq = 1000.0;
    constexpr double gainDb = -9.0;

    auto coeffs =
        eqcore::computeBiquadCoeffs(eqcore::FilterType::HighShelf, freq, sampleRate, gainDb, 0.707);

    expectNear(eqcore::biquadResponseDb(coeffs, 20.0, sampleRate), 0.0, 1.0,
               "high shelf: low frequencies unaffected");
    expectNear(eqcore::biquadResponseDb(coeffs, 20000.0, sampleRate), gainDb, 1.0,
               "high shelf: near-Nyquist gain matches shelf gain");
}

void testLowPassAttenuatesHighs() {
    constexpr double sampleRate = 48000.0;
    auto coeffs = eqcore::computeBiquadCoeffs(eqcore::FilterType::LowPass, 1000.0, sampleRate, 0.0, 0.707);

    const double lowDb = eqcore::biquadResponseDb(coeffs, 100.0, sampleRate);
    const double highDb = eqcore::biquadResponseDb(coeffs, 10000.0, sampleRate);
    if (!(lowDb > highDb + 20.0)) {
        std::fprintf(stderr, "FAIL: low pass should attenuate 10kHz much more than 100Hz (got %.1f vs %.1f)\n",
                     lowDb, highDb);
        ++g_failures;
    } else {
        std::printf("ok: low pass attenuates highs relative to lows (%.1f dB vs %.1f dB)\n", lowDb, highDb);
    }
}

void testEqChainCombinesBandsAdditively() {
    eqcore::EqChain chain(2, 48000.0);
    chain.setBandCount(2);
    chain.setBand(0, eqcore::EqBand{eqcore::FilterType::Peaking, 200.0, 4.0, 1.0});
    chain.setBand(1, eqcore::EqBand{eqcore::FilterType::Peaking, 5000.0, -3.0, 1.0});

    const double combined = chain.frequencyResponseDb(200.0);
    const double bandAlone =
        eqcore::biquadResponseDb(chain.band(0).toCoeffs(48000.0), 200.0, 48000.0) +
        eqcore::biquadResponseDb(chain.band(1).toCoeffs(48000.0), 200.0, 48000.0);

    expectNear(combined, bandAlone, 0.01, "EqChain: combined response is the sum of per-band responses");
}

void testProcessSampleProducesFiniteOutput() {
    eqcore::EqChain chain(2, 48000.0);
    chain.setBandCount(1);
    chain.setBand(0, eqcore::EqBand{eqcore::FilterType::Peaking, 1000.0, 6.0, 1.0});

    bool allFinite = true;
    for (int n = 0; n < 4800; ++n) {
        const float in = static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * n / 48000.0));
        const float out = chain.processSample(0, in);
        if (!std::isfinite(out)) {
            allFinite = false;
            break;
        }
    }

    if (!allFinite) {
        std::fprintf(stderr, "FAIL: EqChain::processSample produced a non-finite sample\n");
        ++g_failures;
    } else {
        std::printf("ok: EqChain::processSample stays finite over a 0.1s sine burst\n");
    }
}

} // namespace

int main() {
    testPeakingBandGainAtCenter();
    testLowShelf();
    testHighShelf();
    testLowPassAttenuatesHighs();
    testEqChainCombinesBandsAdditively();
    testProcessSampleProducesFiniteOutput();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d self-test(s) FAILED\n", g_failures);
        return EXIT_FAILURE;
    }

    std::printf("\nAll eqcore self-tests passed.\n");
    return EXIT_SUCCESS;
}
