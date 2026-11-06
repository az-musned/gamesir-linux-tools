// controller = auto: reads the GameSir and the DualSense at the same time and
// merges them into one controller, so whichever is plugged in just works (and
// both at once also works: buttons are OR-ed, each stick and trigger comes
// from the pad that is pushing it further). Either pad may be absent.
//
// Polled (for the XInput side); the DualSense still wakes the engine at once
// for every report through the forwarded waker.
#pragma once
#include <atomic>
#include <memory>

#include "backend.h"

namespace gsm {

// Pure merge of two samples' state and connected flag (testable). seq is
// left to the caller.
void mergeInputSamples(const InputSample& a, const InputSample& b, InputSample* out);

class MergedSource final : public IInputSource {
public:
    MergedSource(std::unique_ptr<IInputSource> a, std::unique_ptr<IInputSource> b) : a_(std::move(a)), b_(std::move(b)) {}
    ~MergedSource() override { close(); }
    const char* name() const override { return "Auto: GameSir (XInput) + DualSense (USB HID)"; }
    bool open(std::string* err) override;
    void close() override;
    void read(InputSample* out) override;
    bool eventDriven() const override { return false; }
    void setWaker(std::function<void()> waker) override;
    void setRumble(uint8_t large, uint8_t smallMotor) override;
    IInputSource* first() const { return a_.get(); }
    IInputSource* second() const { return b_.get(); }
    // Which pads delivered the last read (for the GUI).
    bool firstConnected() const { return aConn_.load(std::memory_order_relaxed); }
    bool secondConnected() const { return bConn_.load(std::memory_order_relaxed); }

private:
    std::unique_ptr<IInputSource> a_, b_;
    bool aOpen_ = false, bOpen_ = false;
    std::atomic<bool> aConn_{false}, bConn_{false};
    InputSample lastA_, lastB_;
    uint64_t seq_ = 0;  // own counter: XInput's packet number is not monotonic
};

}  // namespace gsm
