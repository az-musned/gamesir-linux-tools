// Virtual Xbox 360 controller through ViGEmBus (already installed on the
// target PC). The ViGEmClient library is loaded at run time from
// ViGEmClient.dll next to the exe, so the build needs no SDK and the app
// still starts (in Demo Mode) when the DLL is missing.
//
// Declarations checked against the official ViGEmClient v1.21.222.0 headers
// (include/ViGEm/Client.h, Common.h). UNTESTED ON HARDWARE.
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
    void onRumble(uint8_t large, uint8_t smallMotor);
    // XInput slot Windows assigned to the virtual pad, or -1 (not yet known).
    int userIndex();

private:
    void* client_ = nullptr;
    void* target_ = nullptr;
    int userIndex_ = -1;
    bool notifRegistered_ = false;
    std::mutex rumbleLock_;
    std::function<void(uint8_t, uint8_t)> rumble_;
};

// Read-only availability check: loads ViGEmClient.dll and opens (then closes)
// a bus connection without creating any virtual device.
struct ViGEmProbe {
    bool dllLoaded = false, busConnected = false;
    std::string text;
};
ViGEmProbe probeViGEm();

}  // namespace gsm
