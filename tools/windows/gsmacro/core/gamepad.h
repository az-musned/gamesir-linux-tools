// Controller state in XInput layout. This is the only state format the app
// handles: physical input arrives in it and the virtual Xbox 360 pad is fed in
// it, so nothing is translated on the hot path.
#pragma once
#include <cstdint>
#include <string>

namespace gsm {

// XInput wButtons bits. PlayStation names (as printed on the Tarantula's
// labels in its PlayStation-style layout) are aliases: Square = X, L1 = LB ...
enum Button : uint16_t {
    BTN_DPAD_UP = 0x0001,
    BTN_DPAD_DOWN = 0x0002,
    BTN_DPAD_LEFT = 0x0004,
    BTN_DPAD_RIGHT = 0x0008,
    BTN_START = 0x0010,
    BTN_BACK = 0x0020,
    BTN_LTHUMB = 0x0040,
    BTN_RTHUMB = 0x0080,
    BTN_LB = 0x0100,
    BTN_RB = 0x0200,
    BTN_GUIDE = 0x0400,
    BTN_A = 0x1000,
    BTN_B = 0x2000,
    BTN_X = 0x4000,
    BTN_Y = 0x8000,
};

enum Axis : uint8_t { AX_LX = 0, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT, AXIS_COUNT };

struct GamepadState {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;

    bool has(uint16_t b) const { return (buttons & b) != 0; }

    int axis(Axis a) const {
        switch (a) {
        case AX_LX: return lx;
        case AX_LY: return ly;
        case AX_RX: return rx;
        case AX_RY: return ry;
        case AX_LT: return lt;
        case AX_RT: return rt;
        default: return 0;
        }
    }
    void setAxis(Axis a, int v) {
        switch (a) {
        case AX_LX: lx = (int16_t)v; break;
        case AX_LY: ly = (int16_t)v; break;
        case AX_RX: rx = (int16_t)v; break;
        case AX_RY: ry = (int16_t)v; break;
        case AX_LT: lt = (uint8_t)v; break;
        case AX_RT: rt = (uint8_t)v; break;
        default: break;
        }
    }
    bool operator==(const GamepadState& o) const {
        return buttons == o.buttons && lt == o.lt && rt == o.rt && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry;
    }
    bool operator!=(const GamepadState& o) const { return !(*this == o); }
};

inline bool isTrigger(Axis a) { return a == AX_LT || a == AX_RT; }

// Normalised value (-1..1 sticks, 0..1 triggers) <-> raw XInput value.
int axisFromNormalized(Axis a, double v);
double axisToNormalized(Axis a, int raw);

// Name lookup. Accepts XInput names (A, X, LB, LT, LX ...) and PlayStation
// aliases (Cross, Square, L1, L2 ...), case-insensitive. Returns 0 / false on
// unknown names.
uint16_t buttonFromName(const std::string& name);
bool axisFromName(const std::string& name, Axis* out);
const char* buttonName(uint16_t singleBit);      // canonical PlayStation-style name
const char* buttonXName(uint16_t singleBit);     // XInput name
const char* axisName(Axis a);

// All single-bit buttons in display order.
extern const uint16_t kAllButtons[15];

}  // namespace gsm
