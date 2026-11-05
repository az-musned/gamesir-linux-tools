// Fixed-bucket latency histogram: O(1) add, no allocation, cheap to copy.
#pragma once
#include <algorithm>
#include <cstdint>

#include "clock.h"

namespace gsm {

struct Histogram {
    static constexpr int kBuckets = 16;
    // Upper bucket edges in ns; the last bucket is open-ended.
    static constexpr Time kEdges[kBuckets] = {
        1'000, 2'000, 5'000, 10'000, 20'000, 50'000, 100'000, 200'000, 500'000,
        1'000'000, 2'000'000, 5'000'000, 10'000'000, 20'000'000, 50'000'000, kNever};

    uint64_t counts[kBuckets] = {};
    uint64_t n = 0;
    double sum = 0;
    Time min = kNever, max = 0;

    void add(Time v) {
        if (v < 0) v = 0;
        int b = 0;
        while (b < kBuckets - 1 && v >= kEdges[b]) b++;
        counts[b]++;
        n++;
        sum += (double)v;
        if (v < min) min = v;
        if (v > max) max = v;
    }
    void reset() { *this = Histogram(); }
    double meanNs() const { return n ? sum / (double)n : 0; }
    // Upper edge of the bucket holding the p-th percentile (an upper bound),
    // clamped to the observed maximum.
    Time percentileUpperBound(double p) const {
        if (!n) return 0;
        uint64_t target = (uint64_t)((p / 100.0) * (double)n);
        if (target >= n) target = n - 1;
        uint64_t acc = 0;
        for (int b = 0; b < kBuckets; b++) {
            acc += counts[b];
            if (acc > target) return std::min(kEdges[b], max);
        }
        return max;
    }
};

}  // namespace gsm
