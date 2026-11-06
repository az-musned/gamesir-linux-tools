// DualSense USB report parsing. The base reports are real captures from a
// DualSense (054C:0CE6) on the target PC; single bytes are varied per test.
#include <cstring>
#include <vector>

#include "../core/config.h"
#include "../core/dualsense.h"
#include "../core/processor.h"
#include "test.h"

using namespace gsm;

namespace {

// Idle (nothing pressed, sticks resting), captured at 1099.872 ms.
const uint8_t kIdle[64] = {
    0x01, 0x7F, 0x80, 0x80, 0x7F, 0x00, 0x00, 0xE9, 0x08, 0x00, 0x00, 0x00, 0x8D, 0x2A, 0xE4, 0xC5,
    0x75, 0xFD, 0xEA, 0x01, 0x2B, 0x01, 0x15, 0xF2, 0xB0, 0x2E, 0x04, 0x02, 0x7D, 0x24, 0x02, 0x07,
    0xFC, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xC7, 0x3F, 0x02, 0x07, 0x29, 0x08, 0x00, 0xFA, 0xF7, 0x56, 0x38, 0x0D, 0x69, 0x0B, 0xE0};
// Finger on the touchpad (x = 1784), captured at 6279.880 ms (first 40 bytes; rest as idle).
const uint8_t kTouchHead[40] = {
    0x01, 0x7F, 0x80, 0x80, 0x7F, 0x00, 0x00, 0xF8, 0x08, 0x00, 0x00, 0x00, 0x9C, 0x2F, 0xE4, 0xC5,
    0xCD, 0x02, 0x08, 0x00, 0x4A, 0x00, 0x16, 0xFB, 0x76, 0x20, 0x99, 0xFC, 0xFF, 0x44, 0xEF, 0x07,
    0xFC, 0x01, 0xF8, 0x36, 0x23, 0x80, 0x00, 0x00};

std::vector<uint8_t> idle() { return std::vector<uint8_t>(kIdle, kIdle + 64); }
GamepadState parse(const std::vector<uint8_t>& r) {
    GamepadState s;
    CHECK(parseDualSenseUsb(r.data(), r.size(), &s));
    return s;
}

}  // namespace

TEST("dualsense: real idle report -> no buttons, sticks near centre, triggers 0") {
    GamepadState s = parse(idle());
    CHECK_EQ(s.buttons, 0);
    CHECK(s.lx > -400 && s.lx < 400 && s.ly > -400 && s.ly < 400);
    CHECK(s.rx > -400 && s.rx < 400 && s.ry > -400 && s.ry < 400);
    CHECK_EQ(s.lt, 0);
    CHECK_EQ(s.rt, 0);
}

TEST("dualsense: face buttons map to the Xbox layout (Square = X, Cross = A ...)") {
    struct { uint8_t bit; uint16_t want; } m[] = {{0x10, BTN_X}, {0x20, BTN_A}, {0x40, BTN_B}, {0x80, BTN_Y}};
    for (auto& e : m) {
        auto r = idle();
        r[8] = (uint8_t)(0x08 | e.bit);
        CHECK_EQ(parse(r).buttons, e.want);
    }
}

TEST("dualsense: D-pad hat, all 8 directions and released") {
    const uint16_t want[9] = {BTN_DPAD_UP, BTN_DPAD_UP | BTN_DPAD_RIGHT, BTN_DPAD_RIGHT, BTN_DPAD_DOWN | BTN_DPAD_RIGHT,
                              BTN_DPAD_DOWN, BTN_DPAD_DOWN | BTN_DPAD_LEFT, BTN_DPAD_LEFT, BTN_DPAD_UP | BTN_DPAD_LEFT, 0};
    for (uint8_t h = 0; h <= 8; h++) {
        auto r = idle();
        r[8] = h;
        CHECK_EQ(parse(r).buttons, want[h]);
    }
    auto r = idle();
    r[8] = 0x0F;  // out-of-range hat value: no D-pad
    CHECK_EQ(parse(r).buttons, 0);
}

TEST("dualsense: shoulder, menu, stick-click, PS and touchpad buttons") {
    struct { int byte; uint8_t bit; uint16_t want; } m[] = {
        {9, 0x01, BTN_LB}, {9, 0x02, BTN_RB}, {9, 0x10, BTN_BACK}, {9, 0x20, BTN_START},
        {9, 0x40, BTN_LTHUMB}, {9, 0x80, BTN_RTHUMB}, {10, 0x01, BTN_GUIDE}, {10, 0x02, kDualSenseTouchpadButton},
        {9, 0x04, 0}, {9, 0x08, 0}, {10, 0x04, 0},  // L2/R2 click bits and Mute: no button (triggers are analog)
    };
    for (auto& e : m) {
        auto r = idle();
        r[e.byte] = e.bit;
        CHECK_EQ(parse(r).buttons, e.want);
    }
    CHECK_EQ(kDualSenseTouchpadButton, BTN_BACK);  // Rocket League scoreboard
}

TEST("dualsense: stick range and direction (Y flipped to XInput up = +)") {
    CHECK_EQ(dualSenseAxis(0), -32768);
    CHECK_EQ(dualSenseAxis(128), 0);
    CHECK_EQ(dualSenseAxis(255), 32767);
    auto r = idle();
    r[1] = 0; r[2] = 0;      // left stick full left + full up
    r[3] = 255; r[4] = 255;  // right stick full right + full down
    GamepadState s = parse(r);
    CHECK_EQ(s.lx, -32768);
    CHECK_EQ(s.ly, 32767);
    CHECK_EQ(s.rx, 32767);
    CHECK_EQ(s.ry, -32768);
}

TEST("dualsense: analog triggers pass 0..255 unchanged") {
    auto r = idle();
    r[5] = 255; r[6] = 77;
    GamepadState s = parse(r);
    CHECK_EQ(s.lt, 255);
    CHECK_EQ(s.rt, 77);
}

TEST("dualsense: real touch report -> touching, x = 1784; idle -> not touching") {
    auto r = idle();
    std::memcpy(r.data(), kTouchHead, sizeof kTouchHead);
    GamepadState s;
    DualSenseExtra ex;
    CHECK(parseDualSenseUsb(r.data(), r.size(), &s, &ex));
    CHECK(ex.touching);
    CHECK_EQ(ex.touchX, 1784);
    CHECK_EQ(s.buttons, 0);  // a touch alone is not a click
    auto i = idle();
    CHECK(parseDualSenseUsb(i.data(), i.size(), &s, &ex));
    CHECK(!ex.touching);
}

TEST("dualsense: rejects non-USB / short reports and leaves the state alone") {
    GamepadState s;
    s.buttons = BTN_Y;
    auto r = idle();
    r[0] = 0x31;  // Bluetooth full report id
    CHECK(!parseDualSenseUsb(r.data(), r.size(), &s));
    CHECK(!parseDualSenseUsb(kIdle, 10, &s));
    CHECK(!parseDualSenseUsb(nullptr, 64, &s));
    CHECK_EQ(s.buttons, BTN_Y);
}

TEST("dualsense: the shift-first wall-dash chord works from DualSense input") {
    Config c;
    c.settings.chordOrder = ChordOrder::ShiftFirst;
    c.settings.controller = ControllerType::DualSense;
    c.macros[1].actions = {Action::press(BTN_A), Action::wait(10), Action::release(BTN_A), Action::wait(5)};
    Processor p;
    p.setConfig(std::make_shared<const Config>(c));
    auto r = idle();
    Time t = ms(1000);
    p.process(parse(r), true, t);
    r[8] = 0x08 | 0x10;  // hold Square
    GamepadState o = p.process(parse(r), true, t += ms(10));
    CHECK(!o.has(BTN_X));  // shift key hidden while armed
    r[9] = 0x02;         // tap R1
    o = p.process(parse(r), true, t += ms(10));
    CHECK_EQ(p.scheduler().currentId(), 1);
    CHECK(o.has(BTN_A) && !o.has(BTN_RB) && !o.has(BTN_X));
}

TEST("config: controller round trip, default gamesir, ds5 alias, bad value rejected") {
    Config c;
    c.settings.controller = ControllerType::DualSense;
    Config back;
    std::string err;
    CHECK(configFromText(configToText(c), &back, &err));
    CHECK(back.settings.controller == ControllerType::DualSense);
    CHECK(configFromText("[settings]\npoll_period_us = 500\n", &back, &err));
    CHECK(back.settings.controller == ControllerType::GameSir);
    CHECK(configFromText("[settings]\ncontroller = DS5\n", &back, &err));
    CHECK(back.settings.controller == ControllerType::DualSense);
    CHECK(!configFromText("[settings]\ncontroller = switchpro\n", &back, &err));
}
