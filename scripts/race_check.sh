#!/usr/bin/env bash
# Runs the realtime-path suites under both race detectors.
#
# Two detectors rather than one on purpose: they disagree about what they can
# see. ThreadSanitizer models C++11 atomics properly and is the primary tool
# here; Helgrind derives happens-before from lock and atomic operations by a
# completely different mechanism, so agreement between them is worth more than
# either alone - particularly for the ring buffer, whose payload is deliberately
# accessed with relaxed atomics.
#
# The --fair-sched=yes is NOT optional. Helgrind serialises threads by default,
# and the concurrency tests spin on an atomic flag without blocking, so the main
# thread never gets scheduled to set the stop flag: the run hangs indefinitely
# rather than failing. It cost an hour to work that out once; hence this script.
# PIPEEQ_TEST_CONCURRENCY_MS shortens the spin windows, since an instrumented run
# does a fraction of the work per millisecond of wall time.
set -uo pipefail
cd "$(dirname "$0")/.."

TSAN_DIR="${TSAN_DIR:-build-tsan}"
DBG_DIR="${DBG_DIR:-build-dbg}"
SUITES=(test_ring_buffer test_peak_meter test_output_processor test_rt_no_alloc)

failures=0

echo "=== ThreadSanitizer ==="
cmake -S . -B "$TSAN_DIR" -DPIPEEQ_SANITIZE=thread -DPIPEEQ_BUILD_GUI=OFF \
    -DCMAKE_BUILD_TYPE=Debug > /dev/null || exit 1
cmake --build "$TSAN_DIR" -j"$(nproc)" --target "${SUITES[@]}" > /dev/null || exit 1
for suite in "${SUITES[@]}"; do
    if "$TSAN_DIR/tests/$suite" > /tmp/race_tsan_$suite.log 2>&1; then
        echo "ok:   $suite"
    else
        echo "FAIL: $suite (see /tmp/race_tsan_$suite.log)" >&2
        failures=$((failures + 1))
    fi
done

echo
echo "=== Helgrind ==="
if ! command -v valgrind > /dev/null; then
    echo "skip: valgrind not installed"
else
    cmake -S . -B "$DBG_DIR" -DCMAKE_BUILD_TYPE=Debug -DPIPEEQ_BUILD_GUI=OFF > /dev/null || exit 1
    cmake --build "$DBG_DIR" -j"$(nproc)" --target "${SUITES[@]}" > /dev/null || exit 1
    for suite in "${SUITES[@]}"; do
        if PIPEEQ_TEST_CONCURRENCY_MS="${PIPEEQ_TEST_CONCURRENCY_MS:-40}" \
            timeout 300 valgrind --tool=helgrind --fair-sched=yes --error-exitcode=99 --quiet \
            "$DBG_DIR/tests/$suite" > /tmp/race_hg_$suite.log 2>&1; then
            echo "ok:   $suite"
        else
            echo "FAIL: $suite exit=$? (see /tmp/race_hg_$suite.log)" >&2
            failures=$((failures + 1))
        fi
    done
fi

echo
if [[ "$failures" -gt 0 ]]; then
    echo "$failures race check(s) FAILED"
    exit 1
fi
echo "All race checks passed under both detectors."
