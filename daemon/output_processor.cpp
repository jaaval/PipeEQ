#include "output_processor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pipeeq {

OutputProcessor::OutputProcessor(uint32_t sampleRateHz)
    : sampleRateHz_(sampleRateHz == 0 ? 48000 : sampleRateHz),
      mixScratch_(kScratchCapacityFrames * kMaxInputChannels, 0.0f) {
    setSampleRate(sampleRateHz_);
}

OutputProcessor::~OutputProcessor() {
    // The owner destroys its pw_stream before this runs, which guarantees no
    // further process() call, so everything retired can go now.
    const OutputSnapshot* live = snapshot_.exchange(nullptr, std::memory_order_acquire);
    delete live;
    retired_.clear();
}

void OutputProcessor::setSampleRate(uint32_t sampleRateHz) {
    sampleRateHz_ = sampleRateHz == 0 ? 48000 : sampleRateHz;
    gainSlewPerSample_ =
        static_cast<float>(1.0 / (kGainSlewSeconds * static_cast<double>(sampleRateHz_)));
}

void OutputProcessor::publish(std::unique_ptr<const OutputSnapshot> next) {
    const OutputSnapshot* previous = snapshot_.exchange(next.release(), std::memory_order_release);

    // The generation MUST be sampled after the exchange, not before.
    //
    // Sampled before, a block that both started and finished between the sample
    // and the exchange would still have observed the outgoing pointer while
    // consuming the retire headroom - so a third block could be mid-process()
    // on that pointer when the counter reached the threshold, and it would be
    // freed underneath it. Sampled after, every reader of the outgoing pointer
    // provably started before the exchange, and since process() is serialized
    // on one realtime thread at most one such reader can be in flight: one
    // observed increment is then sufficient proof it has finished.
    const uint64_t generation = rtGeneration_.load(std::memory_order_acquire);

    if (previous) {
        retired_.emplace_back(generation, std::unique_ptr<const OutputSnapshot>(previous));
    }
    drainRetired();

    // Growth is bounded only while the stream is actually running. PipeWire
    // suspends idle nodes, so a connected-but-silent output stops calling
    // process() and the generation stops advancing - a fader drag then retains
    // every snapshot until the stream resumes or the output is torn down.
    // Retaining is still the only safe choice, since freeing a snapshot the RT
    // thread might be reading is exactly the bug this scheme exists to avoid,
    // so this warns rather than dropping anything.
    if (retired_.size() > kMaxRetained && !retainWarned_) {
        retainWarned_ = true;
        std::fprintf(stderr,
                     "pipeeq: %zu output snapshots awaiting release; the stream doesn't appear to be "
                     "running, so they'll be freed when it is torn down\n",
                     retired_.size());
    }
}

void OutputProcessor::drainRetired() {
    const uint64_t now = rtGeneration_.load(std::memory_order_acquire);
    // One observed increment past the post-exchange sample is proof: see
    // publish(). Comparing with subtraction rather than addition so the test is
    // still correct if the counter ever wraps.
    std::erase_if(retired_, [&](const auto& entry) { return now - entry.first >= 1; });
}

uint32_t OutputProcessor::process(float* dst, uint32_t frames, uint32_t numChannels) {
    const OutputSnapshot* snap = snapshot_.load(std::memory_order_acquire);

    numChannels = std::min<uint32_t>(numChannels, kMaxOutputChannels);
    // Clamped, and the clamped value is RETURNED. The caller sizes the buffer
    // chunk from it; reporting the unclamped count handed the device whatever
    // was previously in the mapped buffer for the difference - stale audio at
    // full scale rather than silence.
    frames = std::min<uint32_t>(frames, kScratchCapacityFrames);
    const uint32_t sampleCount = frames * numChannels;

    if (!snap || numChannels == 0) {
        // Nothing published yet, or not negotiated: emit silence, but still
        // advance the generation so publish() can retire snapshots.
        std::fill(dst, dst + sampleCount, 0.0f);
        rtGeneration_.fetch_add(1, std::memory_order_release);
        return frames;
    }

    std::fill(dst, dst + sampleCount, 0.0f);

    // ------------------------------------------------------------------ mix --
    for (std::size_t slotIndex = 0; slotIndex < kMaxInputs; ++slotIndex) {
        const InputMixSlot& slot = snap->inputs[slotIndex];
        if (!slot.active || !slot.buffer || slot.inputChannels == 0) {
            continue;
        }

        // Nothing routed here AND every send already faded to zero: skip the
        // whole slot, ring-buffer read included. The cursor going stale is
        // fine - RingBuffer::readAt resyncs a lagging reader, and the audible
        // jump that costs lands on a channel that was silent anyway. What we
        // must NOT do is skip while a send is still ramping down, which is
        // exactly what slotSilent_ tracks.
        if (!slot.anyTaps && slotSilent_[slotIndex]) {
            continue;
        }

        // Slots are positional, but which input occupies a slot changes whenever
        // the set of routed inputs does. Without this check a new occupant
        // inherits the previous one's read cursor - and RingBuffer::readAt only
        // resyncs a reader that is BEHIND, never one that is ahead, so an input
        // whose own write index is lower would read silence until it caught up.
        // At an hour of uptime that is an hour of silence.
        if (slotIdentityCur_[slotIndex] != slot.identity) {
            slotIdentityCur_[slotIndex] = slot.identity;
            readCursors_[slotIndex] = slot.buffer->currentWriteIndex();
            for (uint32_t ch = 0; ch < kMaxOutputChannels; ++ch) {
                sendGainCur_[slotIndex][ch] = 0.0f; // fade the new input in
            }
            slotSilent_[slotIndex] = false;
        }

        const std::size_t inputChannels = slot.inputChannels;
        float* const src = mixScratch_.data();
        slot.buffer->readAt(readCursors_[slotIndex], src, frames);

        bool anyAudible = false;
        for (uint32_t ch = 0; ch < numChannels; ++ch) {
            const mix::ChannelTaps& taps = slot.perChannel[ch];
            float gain = sendGainCur_[slotIndex][ch];

            if (taps.count == 0) {
                // Not routed at all. There are no taps to fade, so this is a
                // hard stop - acceptable because losing a position match is a
                // structural change (a channel reassigned to a different
                // position), not something a fader does.
                sendGainCur_[slotIndex][ch] = 0.0f;
                continue;
            }

            const float target = taps.sendGain;
            if (gain == 0.0f && target == 0.0f) {
                continue;
            }
            anyAudible = anyAudible || target != 0.0f;

            // One linear ramp across the block, so a send fader move or a send
            // being switched off lands as a fade rather than a step. Per block
            // rather than per sample is enough here: the result is summed and
            // then passes through the per-sample-slewed channel fader.
            const float step = (target - gain) / static_cast<float>(frames);

            for (uint32_t f = 0; f < frames; ++f) {
                const float* frame = src + f * inputChannels;
                float accumulated = 0.0f;
                for (uint8_t t = 0; t < taps.count; ++t) {
                    accumulated += frame[taps.taps[t].inputChannel] * taps.taps[t].gain;
                }
                dst[f * numChannels + ch] += accumulated * gain;
                gain += step;
            }
            sendGainCur_[slotIndex][ch] = target;
        }
        slotSilent_[slotIndex] = !anyAudible;
    }

    // ------------------------------ per-channel EQ, fader, clamp, metering --
    // Channel-outer / frame-inner on purpose: one channel's 16 BiquadStates
    // (~512 B) stay in L1 for the whole pass, and the biquad recurrence is
    // serial per channel anyway. The stride-numChannels walk through dst costs
    // less than cycling every channel's filter state once per frame would.
    for (uint32_t ch = 0; ch < numChannels; ++ch) {
        const ChannelSnapshot& channel = snap->channels[ch];
        // Raw pointer taken ONCE outside the loop: copying a shared_ptr per
        // sample would be an atomic refcount bump per sample.
        const EqCoeffBlock* const eq = channel.eq.get();
        const std::size_t bandCount = eq ? eq->bandCount : 0;
        auto& state = bandState_[ch];

        float gain = gainCur_[ch];
        const float target = channel.gainLinear; // already 0 when muted
        const float slew = gainSlewPerSample_;
        float peak = 0.0f;
        // A running sum purely so a NaN is DETECTABLE. std::max(peak, NaN)
        // returns peak - a comparison against NaN is always false - so the peak
        // alone can never reveal one. Addition does propagate it. Every value
        // added here has already been clamped into [-1, 1], so over the
        // 8192-frame maximum block this cannot itself overflow to infinity.
        float magnitudeSum = 0.0f;

        for (uint32_t f = 0; f < frames; ++f) {
            float sample = dst[f * numChannels + ch];
            for (std::size_t band = 0; band < bandCount; ++band) {
                sample = state[band].process(eq->coeffs[band], sample);
            }
            gain += std::clamp(target - gain, -slew, slew);
            sample *= gain;
            // ONE clamp, at the end. The old engine clamped the summed mix
            // BEFORE the EQ, which applies a nonlinearity to the signal the
            // filters then see; sum -> EQ -> fader -> clamp is the correct
            // order and still protects the hardware.
            sample = std::clamp(sample, -1.0f, 1.0f);
            dst[f * numChannels + ch] = sample;
            const float magnitude = std::fabs(sample);
            peak = std::max(peak, magnitude);
            magnitudeSum += magnitude;
        }

        // A NaN that reached this channel would otherwise live in its IIR state
        // for the lifetime of the process, silencing it until a restart. One
        // check per block bounds the damage to a single block.
        if (!std::isfinite(magnitudeSum)) {
            for (auto& biquad : state) {
                biquad.reset();
            }
            gain = 0.0f;
            for (uint32_t f = 0; f < frames; ++f) {
                dst[f * numChannels + ch] = 0.0f;
            }
            peak = 0.0f;
        }

        gainCur_[ch] = gain;
        meters_.accumulate(ch, peak); // one lock-free max per channel per block
    }

    rtGeneration_.fetch_add(1, std::memory_order_release);
    return frames;
}

} // namespace pipeeq
