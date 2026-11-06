#include "dualsense.h"

namespace gsm {

int16_t dualSenseAxis(uint8_t v) {
    int d = (int)v - 128;  // -128..127
    if (d < 0) return (int16_t)(d * 256);                 // 0 -> -32768
    return (int16_t)((d * 32767 + 63) / 127);             // 255 -> 32767
}

bool parseDualSenseUsb(const uint8_t* r, size_t n, GamepadState* out, DualSenseExtra* extra) {
    if (!r || n < 11 || r[0] != 0x01) return false;
    GamepadState s;
    s.lx = dualSenseAxis(r[1]);
    s.ly = dualSenseAxis((uint8_t)(255 - r[2]));  // DualSense: 0 = up; XInput: + = up
    s.rx = dualSenseAxis(r[3]);
    s.ry = dualSenseAxis((uint8_t)(255 - r[4]));
    s.lt = r[5];
    s.rt = r[6];

    uint16_t b = 0;
    static const uint16_t hat[8] = {
        BTN_DPAD_UP, BTN_DPAD_UP | BTN_DPAD_RIGHT, BTN_DPAD_RIGHT, BTN_DPAD_DOWN | BTN_DPAD_RIGHT,
        BTN_DPAD_DOWN, BTN_DPAD_DOWN | BTN_DPAD_LEFT, BTN_DPAD_LEFT, BTN_DPAD_UP | BTN_DPAD_LEFT,
    };
    uint8_t h = r[8] & 0x0F;
    if (h < 8) b |= hat[h];
    if (r[8] & 0x10) b |= BTN_X;  // Square
    if (r[8] & 0x20) b |= BTN_A;  // Cross
    if (r[8] & 0x40) b |= BTN_B;  // Circle
    if (r[8] & 0x80) b |= BTN_Y;  // Triangle
    if (r[9] & 0x01) b |= BTN_LB;
    if (r[9] & 0x02) b |= BTN_RB;
    if (r[9] & 0x10) b |= BTN_BACK;    // Create
    if (r[9] & 0x20) b |= BTN_START;   // Options
    if (r[9] & 0x40) b |= BTN_LTHUMB;  // L3
    if (r[9] & 0x80) b |= BTN_RTHUMB;  // R3
    if (r[10] & 0x01) b |= BTN_GUIDE;  // PS
    if (r[10] & 0x02) b |= kDualSenseTouchpadButton;
    s.buttons = b;
    *out = s;

    if (extra) {
        *extra = DualSenseExtra();
        extra->mute = (r[10] & 0x04) != 0;
        if (n >= 37 && !(r[33] & 0x80)) {
            extra->touching = true;
            extra->touchX = (uint16_t)(r[34] | ((r[35] & 0x0F) << 8));
            extra->touchY = (uint16_t)((r[35] >> 4) | (r[36] << 4));
        }
    }
    return true;
}

}  // namespace gsm
