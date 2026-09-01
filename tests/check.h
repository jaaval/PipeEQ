#pragma once

// A deliberately tiny assertion harness, in the same spirit as the hand-rolled
// checks this replaces (the old common/selftest.cpp): no dependency to install,
// nothing to find_package, and it runs anywhere the project builds - including
// inside WSL with no PipeWire graph and no session bus.
//
// Each test binary is one CTest test. Usage:
//
//     void testSomething() { CHECK_EQ(2 + 2, 4); }
//     int main() { RUN(testSomething); return pipeeq::test::summary("arith"); }
//
// The macros map one-for-one onto Catch2's if a real framework ever becomes
// worth the dependency.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace pipeeq::test {

struct Registry {
    int failures = 0;
    int checks = 0;
    const char* current = "<none>";
};

inline Registry& registry() {
    static Registry r;
    return r;
}

inline std::string toStr(bool v) { return v ? "true" : "false"; }
inline std::string toStr(const std::string& v) { return "\"" + v + "\""; }
inline std::string toStr(const char* v) { return std::string("\"") + (v ? v : "(null)") + "\""; }

template <typename T>
std::string toStr(const T& v) {
    if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(v);
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else {
        return "<value>";
    }
}

template <typename T>
std::string toStr(const std::vector<T>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += toStr(v[i]);
    }
    return out + "]";
}

inline void fail(const char* file, int line, const std::string& what) {
    std::fprintf(stderr, "FAIL %s:%d in %s\n      %s\n", file, line, registry().current, what.c_str());
    ++registry().failures;
}

inline void checkTrue(bool ok, const char* expr, const char* file, int line) {
    ++registry().checks;
    if (!ok) {
        fail(file, line, std::string("expected true: ") + expr);
    }
}

template <typename A, typename B>
void checkEq(const A& a, const B& b, const char* exprA, const char* exprB, const char* file, int line) {
    ++registry().checks;
    if (!(a == b)) {
        fail(file, line, std::string(exprA) + " == " + exprB + "\n      got " + toStr(a) + ", want " + toStr(b));
    }
}

inline void checkNear(double a, double b, double tolerance, const char* exprA, const char* exprB,
                      const char* file, int line) {
    ++registry().checks;
    if (!std::isfinite(a) || std::fabs(a - b) > tolerance) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s ~= %s\n      got %.6f, want %.6f (tolerance %.6f)", exprA,
                      exprB, a, b, tolerance);
        fail(file, line, buf);
    }
}

// How long a concurrency smoke test should spin.
//
// Tests that busy-loop for a fixed wall-clock duration are hostile to
// instrumented runs: under Helgrind or DRD the same window does a fraction of
// the work while costing a hundred times the CPU, so a 200 ms loop can take
// many minutes. PIPEEQ_TEST_CONCURRENCY_MS lets those runs ask for a shorter
// window without weakening the default.
inline int concurrencyMs(int defaultMs) {
    if (const char* override = std::getenv("PIPEEQ_TEST_CONCURRENCY_MS")) {
        const int value = std::atoi(override);
        if (value > 0) {
            return value;
        }
    }
    return defaultMs;
}

inline int summary(const char* suite) {
    if (registry().failures > 0) {
        std::fprintf(stderr, "\n%s: %d of %d checks FAILED\n", suite, registry().failures,
                     registry().checks);
        return EXIT_FAILURE;
    }
    std::printf("%s: all %d checks passed.\n", suite, registry().checks);
    return EXIT_SUCCESS;
}

} // namespace pipeeq::test

#define CHECK(expr) ::pipeeq::test::checkTrue((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::pipeeq::test::checkEq((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) ::pipeeq::test::checkNear((a), (b), (tol), #a, #b, __FILE__, __LINE__)

// Names the running test in any failure message, so a bare CHECK inside a
// helper still points at which case exercised it.
#define RUN(fn)                                                                                        \
    do {                                                                                               \
        ::pipeeq::test::registry().current = #fn;                                                       \
        fn();                                                                                          \
        ::pipeeq::test::registry().current = "<none>";                                                  \
    } while (false)
