// Physical controller input from a Sony DualSense / DualSense Edge over USB
// (raw HID, event-driven). A reader thread blocks on the HID interface and
// wakes the engine for every new report, so there is no polling delay.
//
// Read-only: it only reads input reports and never sends output or feature
// reports (no lightbar, rumble or mode changes). It reconnects by itself if
// the controller drops off USB. Bluetooth is not supported (different report
// format); connect by cable. Not an XInput device, so it never takes an
// XInput slot: the virtual pad gets slot 0 without any slot handling.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "../../core/backend.h"

namespace gsm {

class DualSenseSource final : public IInputSource {
public:
    ~DualSenseSource() override { close(); }
    const char* name() const override { return "DualSense (USB HID, event-driven)"; }
    bool open(std::string* err) override;
    void close() override;
    void read(InputSample* out) override;
    bool eventDriven() const override { return true; }
    void setWaker(std::function<void()> waker) override;

private:
    void run();
    void publish(const GamepadState* s);  // nullptr = disconnected
    void* stop_ = nullptr;                // HANDLE (event)
    std::thread thread_;
    std::mutex m_;
    InputSample latest_;
    std::function<void()> waker_;
};

}  // namespace gsm
