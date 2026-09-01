#include "fader_taper.h"

#include <algorithm>
#include <array>

namespace pipeeq::taper {

namespace {

// Console-style breakpoints. Read as "0 dB sits at 80% of the travel", which is
// the property that makes the useful range usable.
struct Point {
    double db;
    double norm;
};

// Ascending in both columns, which the interpolation below relies on.
constexpr std::array<Point, 11> kCurve = {{
    {-65.0, 0.02},
    {-50.0, 0.09},
    {-40.0, 0.18},
    {-30.0, 0.28},
    {-20.0, 0.40},
    {-12.0, 0.53},
    {-6.0, 0.66},
    {0.0, 0.80},
    {6.0, 0.92},
    {12.0, 1.00},
    {12.0, 1.00}, // duplicate terminator, so the search never runs off the end
}};

} // namespace

double dbToNorm(double db) {
    if (db <= kSilenceDb || db < kMinDb) {
        return kSilenceNorm;
    }
    if (db >= kMaxDb) {
        return 1.0;
    }

    for (std::size_t i = 0; i + 1 < kCurve.size(); ++i) {
        const Point& low = kCurve[i];
        const Point& high = kCurve[i + 1];
        if (db >= low.db && db <= high.db) {
            if (high.db == low.db) {
                return high.norm;
            }
            const double t = (db - low.db) / (high.db - low.db);
            return low.norm + t * (high.norm - low.norm);
        }
    }
    return 1.0;
}

double normToDb(double norm) {
    if (norm <= kSilenceNorm) {
        return kSilenceDb;
    }
    if (norm >= 1.0) {
        return kMaxDb;
    }
    // Anything below the first breakpoint is inside the detent slice.
    if (norm < kCurve.front().norm) {
        return kSilenceDb;
    }

    for (std::size_t i = 0; i + 1 < kCurve.size(); ++i) {
        const Point& low = kCurve[i];
        const Point& high = kCurve[i + 1];
        if (norm >= low.norm && norm <= high.norm) {
            if (high.norm == low.norm) {
                return high.db;
            }
            const double t = (norm - low.norm) / (high.norm - low.norm);
            return low.db + t * (high.db - low.db);
        }
    }
    return kMaxDb;
}

bool isSilent(double db) {
    return db <= kMinDb;
}

} // namespace pipeeq::taper
