#include "gamepad.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace gsm {

const uint16_t kAllButtons[15] = {
    BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LB, BTN_RB, BTN_LTHUMB, BTN_RTHUMB,
    BTN_BACK, BTN_START, BTN_GUIDE, BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
};

int axisFromNormalized(Axis a, double v) {
    if (isTrigger(a)) {
        v = std::clamp(v, 0.0, 1.0);
        return (int)std::lround(v * 255.0);
    }
    v = std::clamp(v, -1.0, 1.0);
    // -1 maps to -32768 so a full deflection is reachable in both directions.
    return v < 0 ? (int)std::lround(v * 32768.0) : (int)std::lround(v * 32767.0);
}

double axisToNormalized(Axis a, int raw) {
    if (isTrigger(a)) return raw / 255.0;
    return raw < 0 ? raw / 32768.0 : raw / 32767.0;
}

namespace {
struct Name { const char* name; uint16_t bit; };
// First entry per bit is the canonical PlayStation-style name.
const Name kNames[] = {
    {"Cross", BTN_A}, {"A", BTN_A},
    {"Circle", BTN_B}, {"B", BTN_B},
    {"Square", BTN_X}, {"X", BTN_X},
    {"Triangle", BTN_Y}, {"Y", BTN_Y},
    {"L1", BTN_LB}, {"LB", BTN_LB},
    {"R1", BTN_RB}, {"RB", BTN_RB},
    {"L3", BTN_LTHUMB}, {"LS", BTN_LTHUMB}, {"LThumb", BTN_LTHUMB},
    {"R3", BTN_RTHUMB}, {"RS", BTN_RTHUMB}, {"RThumb", BTN_RTHUMB},
    {"Select", BTN_BACK}, {"Back", BTN_BACK}, {"View", BTN_BACK}, {"Share", BTN_BACK},
    {"Start", BTN_START}, {"Menu", BTN_START}, {"Options", BTN_START},
    {"Home", BTN_GUIDE}, {"Guide", BTN_GUIDE},
    {"Up", BTN_DPAD_UP}, {"DpadUp", BTN_DPAD_UP},
    {"Down", BTN_DPAD_DOWN}, {"DpadDown", BTN_DPAD_DOWN},
    {"Left", BTN_DPAD_LEFT}, {"DpadLeft", BTN_DPAD_LEFT},
    {"Right", BTN_DPAD_RIGHT}, {"DpadRight", BTN_DPAD_RIGHT},
};
const char* kXNames[] = {"Up", "Down", "Left", "Right", "Start", "Back", "LS", "RS",
                         "LB", "RB", "Guide", "?", "A", "B", "X", "Y"};

bool ieq(const std::string& a, const char* b) {
    size_t n = a.size();
    for (size_t i = 0; i < n; i++) {
        if (!b[i] || std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return b[n] == '\0';
}
}  // namespace

uint16_t buttonFromName(const std::string& n) {
    for (const auto& e : kNames)
        if (ieq(n, e.name)) return e.bit;
    return 0;
}

const char* buttonName(uint16_t bit) {
    for (const auto& e : kNames)
        if (e.bit == bit) return e.name;
    return "?";
}

const char* buttonXName(uint16_t bit) {
    for (int i = 0; i < 16; i++)
        if (bit == (1u << i)) return kXNames[i];
    return "?";
}

bool axisFromName(const std::string& n, Axis* out) {
    static const struct { const char* n; Axis a; } t[] = {
        {"LX", AX_LX}, {"LY", AX_LY}, {"RX", AX_RX}, {"RY", AX_RY},
        {"LT", AX_LT}, {"L2", AX_LT}, {"RT", AX_RT}, {"R2", AX_RT},
    };
    for (const auto& e : t)
        if (ieq(n, e.n)) { *out = e.a; return true; }
    return false;
}

const char* axisName(Axis a) {
    static const char* n[] = {"LX", "LY", "RX", "RY", "L2", "R2"};
    return a < AXIS_COUNT ? n[a] : "?";
}

}  // namespace gsm
