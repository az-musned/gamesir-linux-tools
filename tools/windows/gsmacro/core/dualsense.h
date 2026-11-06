// Sony DualSense (054C:0CE6) / DualSense Edge (054C:0DF2) USB input report
// -> GamepadState (XInput layout). Pure and portable, so it is unit-tested.
//
// USB input report 0x01, 64 bytes (layout verified on a real DualSense on the
// target PC, matching Linux hid-playstation's dualsense_input_report):
//   [1] LX  [2] LY  [3] RX  [4] RY   0..255, centre ~128, Y: 0 = up
//   [5] L2  [6] R2                  0..255 analog
//   [8]  low nibble: D-pad hat (0 N, 1 NE, 2 E, ... 7 NW, 8 released)
//        0x10 Square  0x20 Cross  0x40 Circle  0x80 Triangle
//   [9]  0x01 L1  0x02 R1  0x04 L2 click  0x08 R2 click
//        0x10 Create  0x20 Options  0x40 L3  0x80 R3
//   [10] 0x01 PS  0x02 touchpad click  0x04 mute
//   [33] touch point 1: bit 7 = 0 while touching; [34..36] x/y (12 bit each)
// Bluetooth uses report 0x31 with a different offset and is not handled.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gamepad.h"

namespace gsm {

constexpr uint16_t kSonyVid = 0x054C;
constexpr uint16_t kDualSensePid = 0x0CE6;
constexpr uint16_t kDualSenseEdgePid = 0x0DF2;
constexpr size_t kDualSenseUsbReportSize = 64;

// What the touchpad click becomes on the virtual Xbox 360 pad (which has no
// touchpad). Back/View = Rocket League's scoreboard, as on a DualSense.
constexpr uint16_t kDualSenseTouchpadButton = BTN_BACK;

struct DualSenseExtra {
    bool touching = false;   // finger 1 on the touchpad
    uint16_t touchX = 0, touchY = 0;
    bool mute = false;
};

// Returns false (and leaves *out alone) if this is not a USB 0x01 report.
bool parseDualSenseUsb(const uint8_t* report, size_t size, GamepadState* out, DualSenseExtra* extra = nullptr);

// 0..255 (centre 128) -> -32768..32767, full range reachable at both ends.
int16_t dualSenseAxis(uint8_t v);

}  // namespace gsm
