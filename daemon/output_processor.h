#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "biquad.h"
#include "eq_band.h"
#include "mix_plan.h"
#include "peak_meter.h"
#include "ring_buffer.h"
#include "rt_limits.h"

namespace pipeeq {

// Coefficients for one EQ instance, computed on the control thread when that
// instance's bands (or the stream's negotiated sample rate) change, and shared
// BY POINTER with every channel that references the instance. Immutable once
// published, so FL and FR sharing one instance genuinely share one coefficient
// block rather than two copies that could drift.
struct EqCoeffBlock {
    std::size_t bandCount = 0;
    std::array<eqcore::BiquadCoeffs, kMaxBands> coeffs{};
};

// One output's mix contribution from a single input.
//
// Constructed and destroyed only on the control thread, and only ever READ
// (never mutated in place) by the realtime thread.
struct InputMixSlot {
    bool active = false;
    std::string inputId;
    std::shared_ptr<const RingBuffer> buffer;
    // Cached so the RT thread never has to dereference `buffer` to learn its
    // width before it starts reading.
    uint16_t inputChannels = 0;
    // OR over perChannel[]: is anything routed here AND audible? False lets
    // the RT thread skip this slot's ring-buffer read entirely.
    bool anyTaps = false;
    std::array<mix::ChannelTaps, kMaxOutputChannels> perChannel{};
};

// Everything one output CHANNEL's post-mix processing needs.
struct ChannelSnapshot {
    // ALREADY zero when the channel is muted, so the RT path has no mute
    // branch and a mute is just another fader target to slew towards.
    float gainLinear = 1.0f;
    std::shared_ptr<const EqCoeffBlock> eq; // null = no EQ on this channel
};

// Immutable, atomically-published view of everything OutputProcessor::process()
// needs. Rebuilt wholesale (copy-on-write) by the control thread on every
// mutation and read once at the top of each realtime block - never mutated in
// place, so there are no torn reads and no lock on the RT side.
//
// Filter history, gain/send ramp state and per-input read cursors are
// deliberately NOT here (see OutputProcessor): they must persist ACROSS
// snapshot swaps rather than reset with them.
struct OutputSnapshot {
    uint8_t numChannels = 0;
    std::array<ChannelSnapshot, kMaxOutputChannels> channels{};
    std::array<InputMixSlot, kMaxInputs> inputs{};
};

// The realtime core of one output, with the PipeWire stream deliberately
// factored out (see OutputStream) so this class contains no PipeWire type and
// is therefore directly unit-testable.
//
// Everything here is either read from an immutable published snapshot or owned
// exclusively by the realtime thread. There is no lock, no allocation and no
// branch on control-plane state anywhere in process(). Every buffer is sized
// for the kMax* limits at construction and never for the negotiated channel
// count, so a param_changed() reporting a different layout than we asked for
// can never cause a resize on the realtime path.
class OutputProcessor {
public:
    explicit OutputProcessor(uint32_t sampleRateHz);
    ~OutputProcessor();

    OutputProcessor(const OutputProcessor&) = delete;
    OutputProcessor& operator=(const OutputProcessor&) = delete;

    // Control thread only. Publishes an immutable snapshot; the previous one is
    // retired and freed once the realtime thread has demonstrably moved past
    // it.
    void publish(std::unique_ptr<const OutputSnapshot> next);

    // Realtime thread only. `dst` is interleaved and `numChannels` wide, and is
    // both the mix accumulator and the output buffer.
    void process(float* dst, uint32_t frames, uint32_t numChannels);

    // Control thread. The peak magnitude since the previous call, then resets.
    float takeChannelPeak(std::size_t channel) { return meters_.takeAndReset(channel); }

    // Control thread, and only while no stream is running: changing the rate
    // changes the fader slew, and every EQ coefficient has to be rebuilt for
    // the new rate by the caller.
    void setSampleRate(uint32_t sampleRateHz);
    uint32_t sampleRateHz() const { return sampleRateHz_; }

    // Test/diagnostic hook: how many retired snapshots are still waiting to be
    // freed. Should return to 0 while a stream is running.
    std::size_t retiredSnapshotCount() const { return retired_.size(); }

    // A fader or mute reaches its target in this long, independent of block
    // size. Multiplying by zero mid-waveform is a full-scale step and audibly a
    // click; a fader drag arriving as tens of discrete updates a second is
    // textbook zipper noise. 10 ms fixes both and costs one add plus one clamp
    // per sample per channel.
    static constexpr double kGainSlewSeconds = 0.010;

private:
    void drainRetired();

    uint32_t sampleRateHz_;
    float gainSlewPerSample_;

    // ---- realtime-thread-owned; must persist across snapshot swaps ----
    std::array<std::array<eqcore::BiquadState, kMaxBands>, kMaxOutputChannels> bandState_{};
    std::array<float, kMaxOutputChannels> gainCur_{};
    std::array<std::array<float, kMaxOutputChannels>, kMaxInputs> sendGainCur_{};
    // True once every send of a slot has actually reached zero, which is what
    // lets an unrouted slot be skipped without cutting a still-fading one.
    std::array<bool, kMaxInputs> slotSilent_{};
    std::array<std::size_t, kMaxInputs> readCursors_{};
    std::vector<float> mixScratch_;
    PeakMeterBank<kMaxOutputChannels> meters_;

    // A genuinely lock-free publication, NOT std::atomic<std::shared_ptr<>>:
    // libstdc++ implements that with a spinlock in the control block, which
    // would let the realtime thread spin waiting on the control thread - the
    // exact thing this whole design exists to avoid.
    //
    // The RT thread bumps rtGeneration_ at the end of every block, so once the
    // control thread observes it advance far enough, no block that could still
    // hold a retired snapshot is in flight and it is safe to free.
    std::atomic<const OutputSnapshot*> snapshot_{nullptr};
    std::atomic<uint64_t> rtGeneration_{0};
    std::vector<std::pair<uint64_t, std::unique_ptr<const OutputSnapshot>>> retired_;

    // Purely a diagnostic threshold: past this many retained snapshots the
    // stream evidently isn't advancing, which is worth saying out loud once.
    // Nothing is ever dropped - freeing a snapshot the RT thread might still be
    // reading is the one thing this scheme must never do.
    static constexpr std::size_t kMaxRetained = 64;
    bool retainWarned_ = false;
};

} // namespace pipeeq
