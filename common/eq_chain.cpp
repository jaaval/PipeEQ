#include "eq_chain.h"

namespace eqcore {

EqChain::EqChain(int numChannels, double sampleRateHz) : sampleRateHz_(sampleRateHz) {
    setNumChannels(numChannels);
}

void EqChain::setSampleRate(double sampleRateHz) {
    sampleRateHz_ = sampleRateHz;
    for (std::size_t i = 0; i < bands_.size(); ++i) {
        rebuildCoeffs(i);
    }
    reset();
}

void EqChain::setNumChannels(int numChannels) {
    state_.assign(static_cast<std::size_t>(numChannels), std::vector<BiquadState>(bands_.size()));
}

void EqChain::setBandCount(std::size_t count) {
    bands_.resize(count);
    coeffs_.resize(count);
    for (auto& channelState : state_) {
        channelState.resize(count);
    }
    for (std::size_t i = 0; i < count; ++i) {
        rebuildCoeffs(i);
    }
}

void EqChain::setBand(std::size_t index, const EqBand& band) {
    bands_.at(index) = band;
    rebuildCoeffs(index);
}

float EqChain::processSample(int channel, float input) {
    auto& channelState = state_.at(static_cast<std::size_t>(channel));
    float sample = input;
    for (std::size_t i = 0; i < bands_.size(); ++i) {
        sample = channelState[i].process(coeffs_[i], sample);
    }
    return sample;
}

void EqChain::reset() {
    for (auto& channelState : state_) {
        for (auto& s : channelState) {
            s.reset();
        }
    }
}

double EqChain::frequencyResponseDb(double freqHz) const {
    double totalDb = 0.0;
    for (const auto& c : coeffs_) {
        totalDb += biquadResponseDb(c, freqHz, sampleRateHz_);
    }
    return totalDb;
}

void EqChain::rebuildCoeffs(std::size_t index) {
    coeffs_.at(index) = bands_.at(index).toCoeffs(sampleRateHz_);
}

} // namespace eqcore
