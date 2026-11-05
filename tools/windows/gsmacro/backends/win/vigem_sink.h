// Virtual Xbox 360 controller through ViGEmBus (already installed on the
// target PC). The ViGEmClient library is loaded at run time from
// ViGEmClient.dll next to the exe, so the build needs no SDK and the app
// still starts (in Demo Mode) when the DLL is missing.
//
// Function signatures follow ViGEmClient 1.17+ (notification callback with a
// user-data pointer). UNTESTED ON HARDWARE.
#pragma once
#include <functional>
#include <mutex>

#include "../../core/backend.h"

namespace gsm {

class ViGEmSink final : public IOutputSink {
public:
    ~ViGEmSink() override { close(); }
    const char* name() const override { return "ViGEmBus virtual Xbox 360"; }
    bool open(std::string* err) override;
    void close() override;
    bool submit(const GamepadState& s) override;
    void setRumbleHandler(std::function<void(uint8_t, uint8_t)> h) override;

    // Called from ViGEm's notification thread.
    void onRumble(uint8_t large, uint8_t small);

private:
    void* client_ = nullptr;
    void* target_ = nullptr;
    bool notifRegistered_ = false;
    std::mutex rumbleLock_;
    std::function<void(uint8_t, uint8_t)> rumble_;
};

}  // namespace gsm
