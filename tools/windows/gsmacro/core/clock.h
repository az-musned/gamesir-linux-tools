// Time is int64 nanoseconds on a monotonic clock. On Windows (MSVC and
// MinGW) std::chrono::steady_clock is QueryPerformanceCounter-based.
#pragma once
#include <chrono>
#include <cstdint>
#include <limits>

namespace gsm {

using Time = int64_t;  // nanoseconds
constexpr Time kNever = std::numeric_limits<Time>::max();

constexpr Time us(double v) { return (Time)(v * 1e3); }
constexpr Time ms(double v) { return (Time)(v * 1e6); }
constexpr double toMs(Time t) { return (double)t / 1e6; }
constexpr double toUs(Time t) { return (double)t / 1e3; }

class Clock {
public:
    virtual ~Clock() = default;
    virtual Time now() const = 0;
};

class SteadyClock final : public Clock {
public:
    Time now() const override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};

// Manually advanced clock for deterministic tests.
class FakeClock final : public Clock {
public:
    explicit FakeClock(Time start = ms(1000)) : t_(start) {}
    Time now() const override { return t_; }
    void set(Time t) { t_ = t; }
    void advance(Time d) { t_ += d; }
private:
    Time t_;
};

}  // namespace gsm
