// Physical controller input through XInput (polled).
//
// Picks the XInput slot whose VID is 0x3537 (GameSir), using the
// undocumented XInputGetCapabilitiesEx (ordinal 108) that SDL also uses. It
// never picks a pad by slot number alone, so it can't read back our own
// virtual Xbox 360 pad (045E:028E) and form a feedback loop. Reads use
// XInputGetStateEx (ordinal 100) so the Guide button is included.
//
// The physical pad stays visible to this process when HidHide hides it from
// games, as long as gsmacro.exe is on HidHide's allow list.
//
// UNTESTED ON HARDWARE. gsprobe's measurements decide whether an
// event-driven (HID-wake) variant should replace or supplement this one.
#pragma once
#include <atomic>

#include "../../core/backend.h"

namespace gsm {

class XInputSource final : public IInputSource {
public:
    explicit XInputSource(uint16_t vid = 0x3537) : vid_(vid) {}
    const char* name() const override { return "XInput (polled, GameSir VID 3537)"; }
    bool open(std::string* err) override;
    void close() override;
    void read(InputSample* out) override;
    void setRumble(uint8_t large, uint8_t smallMotor) override;
    int slot() const { return slot_.load(); }

private:
    int findSlot();
    uint16_t vid_;
    std::atomic<int> slot_{-1};
    Time lastScan_ = 0;
    SteadyClock clock_;
};

}  // namespace gsm
