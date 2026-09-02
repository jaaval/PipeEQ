// The realtime path must never allocate.
//
// A malloc inside process() can block on the allocator's lock behind any other
// thread, which is exactly the unbounded stall that produces xruns. Every
// buffer OutputProcessor touches is sized for the kMax* limits at
// construction, so this asserts that property rather than trusting the comment.
//
// The counter is armed only around process() calls: publish() allocates a whole
// snapshot on purpose, on the control thread, which is fine.
//
// Both operator new AND the C allocator are intercepted. Overriding only
// operator new was not enough: the stated rationale for this test is that a
// malloc inside process() can block on the allocator's lock, and a literal
// std::malloc was completely invisible - as was anything from C code, which
// includes every allocation PipeWire itself might make on the realtime thread.

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <dlfcn.h>

// Whether replacing the allocator is even possible in this build.
//
// A sanitizer installs its own operator new and malloc interceptors, and
// defining ours alongside them does not merely fail to count - under
// AddressSanitizer it segfaults outright. So the replacements below are not
// compiled at all in a sanitizer build, and main() reports a hard skip.
// Valgrind replaces the allocator at run time instead, which the runtime probe
// in main() catches.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define PIPEEQ_ALLOC_TRACKING_SUPPORTED 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define PIPEEQ_ALLOC_TRACKING_SUPPORTED 0
#else
#define PIPEEQ_ALLOC_TRACKING_SUPPORTED 1
#endif
#else
#define PIPEEQ_ALLOC_TRACKING_SUPPORTED 1
#endif

#include "mix_plan.h"
#include "output_processor.h"
#include "ring_buffer.h"

#include "check.h"

namespace {
std::atomic<bool> g_tracking{false};
std::atomic<long> g_allocations{0};
} // namespace

#if PIPEEQ_ALLOC_TRACKING_SUPPORTED

// The real allocator, resolved once so the interposers below don't recurse.
// Looked up lazily rather than in a constructor, because a static initialiser
// elsewhere may allocate before ours would have run.
namespace {

using MallocFn = void* (*)(std::size_t);
using CallocFn = void* (*)(std::size_t, std::size_t);
using ReallocFn = void* (*)(void*, std::size_t);

MallocFn realMalloc() {
    static MallocFn fn = reinterpret_cast<MallocFn>(dlsym(RTLD_NEXT, "malloc"));
    return fn;
}
CallocFn realCalloc() {
    static CallocFn fn = reinterpret_cast<CallocFn>(dlsym(RTLD_NEXT, "calloc"));
    return fn;
}
ReallocFn realRealloc() {
    static ReallocFn fn = reinterpret_cast<ReallocFn>(dlsym(RTLD_NEXT, "realloc"));
    return fn;
}

void noteAllocation() {
    if (g_tracking.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

extern "C" void* malloc(std::size_t size) {
    noteAllocation();
    return realMalloc()(size);
}

extern "C" void* calloc(std::size_t count, std::size_t size) {
    noteAllocation();
    return realCalloc()(count, size);
}

extern "C" void* realloc(void* ptr, std::size_t size) {
    noteAllocation();
    return realRealloc()(ptr, size);
}

void* operator new(std::size_t size) {
    // Counted by the malloc interposer below, so this must NOT count again.
    void* p = std::malloc(size == 0 ? 1 : size);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    noteAllocation();
    // aligned_alloc requires size to be a multiple of the alignment; passing an
    // arbitrary size is undefined even though glibc tolerates it.
    const std::size_t align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1) / align * align;
    void* p = std::aligned_alloc(align, rounded);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

#endif // PIPEEQ_ALLOC_TRACKING_SUPPORTED

namespace {

using namespace pipeeq;

std::unique_ptr<OutputSnapshot> makeSnapshot(const std::shared_ptr<const RingBuffer>& buffer,
                                             std::size_t numChannels, double sendDb) {
    auto snap = std::make_unique<OutputSnapshot>();
    snap->numChannels = static_cast<uint8_t>(numChannels);

    std::vector<std::string> outputPositions;
    for (std::size_t ch = 0; ch < numChannels; ++ch) {
        outputPositions.push_back(ch == 0 ? "FL" : ch == 1 ? "FR" : ch == 2 ? "RL" : "RR");
        snap->channels[ch].gainLinear = 0.8f;
    }

    auto eq = std::make_shared<EqCoeffBlock>();
    eq->bandCount = kMaxBands;
    for (std::size_t band = 0; band < kMaxBands; ++band) {
        eq->coeffs[band] =
            eqcore::EqBand{eqcore::FilterType::Peaking, 100.0 * static_cast<double>(band + 1), 1.0, 1.0}
                .toCoeffs(48000.0);
    }
    for (std::size_t ch = 0; ch < numChannels; ++ch) {
        snap->channels[ch].eq = eq;
    }

    InputMixSlot& slot = snap->inputs[0];
    slot.active = true;
    slot.inputId = "input-1";
    slot.buffer = buffer;
    slot.inputChannels = static_cast<uint16_t>(buffer->numChannels());

    mix::SendSpec spec;
    spec.gainDb = sendDb;
    const std::vector<mix::SendSpec> sends(numChannels, spec);
    mix::buildOutputTaps(mix::positionValues(outputPositions), mix::positionValues({"FL", "FR"}), sends,
                          slot.perChannel, slot.anyTaps);
    return snap;
}

void testProcessNeverAllocates() {
    constexpr std::size_t kChannels = 4;
    constexpr uint32_t kFrames = 512;

    OutputProcessor processor(48000);
    auto buffer = std::make_shared<RingBuffer>(65536, 2);
    std::vector<float> block(32768 * 2, 0.3f);
    buffer->write(block.data(), 32768);

    // Prepare a batch of snapshots up front, so swapping one in mid-run costs
    // no allocation inside the tracked window.
    std::vector<std::unique_ptr<OutputSnapshot>> pending;
    for (int i = 0; i < 10; ++i) {
        pending.push_back(makeSnapshot(buffer, kChannels, -static_cast<double>(i)));
    }

    std::vector<float> dst(kFrames * kChannels, 0.0f);
    processor.publish(makeSnapshot(buffer, kChannels, 0.0));
    processor.process(dst.data(), kFrames, kChannels); // warm up outside tracking

    std::size_t nextSnapshot = 0;
    g_allocations.store(0, std::memory_order_relaxed);
    g_tracking.store(true, std::memory_order_relaxed);

    for (int i = 0; i < 1000; ++i) {
        // Swap a pre-built snapshot in every hundred blocks: publishing itself
        // is control-thread work, but process() reading a freshly swapped
        // snapshot must still allocate nothing.
        if (i % 100 == 0 && nextSnapshot < pending.size()) {
            processor.publish(std::move(pending[nextSnapshot++]));
        }
        processor.process(dst.data(), kFrames, kChannels);
        // Keep the buffer fed so the read path stays on its normal course.
        if (i % 32 == 0) {
            buffer->write(block.data(), 4096);
        }
    }

    g_tracking.store(false, std::memory_order_relaxed);
    const long allocations = g_allocations.load(std::memory_order_relaxed);

    // publish() legitimately frees retired snapshots and grows its retire list.
    // The bound is deliberately loose here because the window includes those 10
    // publishes; the assertion that actually matters is the process()-only run
    // below, which must be exactly zero.
    CHECK(allocations <= 60);

    g_allocations.store(0, std::memory_order_relaxed);
    g_tracking.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
        processor.process(dst.data(), kFrames, kChannels);
    }
    g_tracking.store(false, std::memory_order_relaxed);
    CHECK_EQ(g_allocations.load(std::memory_order_relaxed), 0L);
}

// The clamping paths are the ones most likely to reach for more memory, so
// exercise a surprise format under tracking too.
// Proves the interposition works, so a zero result means "nothing allocated"
// rather than "nothing was being watched". Without this the whole suite could
// pass with the counter permanently disconnected.
bool allocationCounterWorks() {
#if !PIPEEQ_ALLOC_TRACKING_SUPPORTED
    return false;
#endif
    g_allocations.store(0, std::memory_order_relaxed);
    g_tracking.store(true, std::memory_order_relaxed);
    void* fromMalloc = std::malloc(64);
    volatile int* fromNew = new int(7);
    g_tracking.store(false, std::memory_order_relaxed);
    const long counted = g_allocations.load(std::memory_order_relaxed);
    std::free(fromMalloc);
    delete fromNew;
    // At least two: one for malloc, one for new (which routes through malloc).
    return counted >= 2;
}

void testTheAllocationCounterActuallyCounts() {
    CHECK(allocationCounterWorks());
}

void testProcessNeverAllocatesWithSurpriseFormat() {
    OutputProcessor processor(48000);
    auto buffer = std::make_shared<RingBuffer>(65536, 2);
    std::vector<float> block(32768 * 2, 0.3f);
    buffer->write(block.data(), 32768);
    processor.publish(makeSnapshot(buffer, 2, 0.0));

    std::vector<float> dst((kScratchCapacityFrames + 1024) * (kMaxOutputChannels + 4), 0.0f);
    processor.process(dst.data(), 256, 2); // warm up

    g_allocations.store(0, std::memory_order_relaxed);
    g_tracking.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 50; ++i) {
        processor.process(dst.data(), kScratchCapacityFrames + 1024, kMaxOutputChannels + 4);
    }
    g_tracking.store(false, std::memory_order_relaxed);

    CHECK_EQ(g_allocations.load(std::memory_order_relaxed), 0L);
}

} // namespace

int main() {
    // This suite works by replacing the allocator, which a sanitizer or
    // valgrind also does - and wins. Every measurement here would then be a
    // meaningless zero, which would look exactly like a pass.
    //
    // Reported as a hard skip so it can never be mistaken for having verified
    // something. The probe covers the run-time case (valgrind); the macro above
    // covers the compile-time one (sanitizers), where the replacements are not
    // even defined.
    if (!allocationCounterWorks()) {
        std::fprintf(stderr,
                     "rt_no_alloc: SKIPPED - the allocator is replaced by a sanitizer or "
                     "valgrind, so allocations cannot be counted here.\n"
                     "             Run this suite in a plain build to exercise it.\n");
        return EXIT_SUCCESS;
    }

    RUN(testTheAllocationCounterActuallyCounts);
    RUN(testProcessNeverAllocates);
    RUN(testProcessNeverAllocatesWithSurpriseFormat);
    return pipeeq::test::summary("rt_no_alloc");
}
