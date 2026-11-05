// Mock backends for Demo Mode and tests. No hardware, no drivers.
#pragma once
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "../core/backend.h"

namespace gsm {

// Thread-safe settable controller. Event-driven by default (wakes the engine
// on every change, like a HID read completing); can emulate a polled source.
class MockInput final : public IInputSource {
public:
    explicit MockInput(bool eventDriven = true) : eventDriven_(eventDriven) {}
    const char* name() const override { return eventDriven_ ? "Mock (event-driven)" : "Mock (polled)"; }
    void read(InputSample* out) override {
        std::lock_guard<std::mutex> l(m_);
        *out = s_;
    }
    bool eventDriven() const override { return eventDriven_; }
    void setWaker(std::function<void()> w) override {
        std::lock_guard<std::mutex> l(m_);
        waker_ = std::move(w);
    }
    void setRumble(uint8_t large, uint8_t smallMotor) override { rumbleL_ = large; rumbleS_ = smallMotor; }

    void set(const GamepadState& st, bool connected = true) {
        std::function<void()> w;
        {
            std::lock_guard<std::mutex> l(m_);
            if (s_.state == st && s_.connected == connected) return;
            s_.state = st;
            s_.connected = connected;
            s_.seq++;
            w = waker_;
        }
        if (w) w();
    }
    InputSample current() {
        std::lock_guard<std::mutex> l(m_);
        return s_;
    }
    uint8_t rumbleLarge() const { return rumbleL_; }
    uint8_t rumbleSmall() const { return rumbleS_; }

private:
    bool eventDriven_;
    std::mutex m_;
    InputSample s_;
    std::function<void()> waker_;
    std::atomic<uint8_t> rumbleL_{0}, rumbleS_{0};
};

// Records what would be sent to the virtual pad.
class RecordingSink final : public IOutputSink {
public:
    struct Frame { Time t; GamepadState s; };
    explicit RecordingSink(const Clock& c, size_t keep = 100000) : clock_(c), keep_(keep) {}
    const char* name() const override { return "Recording sink (no virtual device)"; }
    bool submit(const GamepadState& s) override {
        std::lock_guard<std::mutex> l(m_);
        last_ = s;
        count_++;
        if (frames_.size() < keep_) frames_.push_back({clock_.now(), s});
        return true;
    }
    GamepadState last() { std::lock_guard<std::mutex> l(m_); return last_; }
    std::vector<Frame> frames() { std::lock_guard<std::mutex> l(m_); return frames_; }
    uint64_t count() { std::lock_guard<std::mutex> l(m_); return count_; }
    void clear() { std::lock_guard<std::mutex> l(m_); frames_.clear(); count_ = 0; }

private:
    const Clock& clock_;
    size_t keep_;
    std::mutex m_;
    GamepadState last_;
    uint64_t count_ = 0;
    std::vector<Frame> frames_;
};

}  // namespace gsm
