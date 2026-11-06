// controller = auto: GameSir + DualSense merged into one controller.
#include "../backends/mock_backend.h"
#include "../core/merged_source.h"
#include "test.h"

using namespace gsm;

namespace {
InputSample sample(bool connected, uint16_t buttons = 0, int16_t lx = 0, int16_t ly = 0, uint8_t rt = 0) {
    InputSample s;
    s.connected = connected;
    s.state.buttons = buttons;
    s.state.lx = lx;
    s.state.ly = ly;
    s.state.rt = rt;
    return s;
}
}  // namespace

TEST("merge: only one pad connected -> its state exactly, either side") {
    InputSample o;
    mergeInputSamples(sample(true, BTN_A, 1000, -2000, 50), sample(false, BTN_X, 30000), &o);
    CHECK(o.connected);
    CHECK_EQ(o.state.buttons, BTN_A);
    CHECK_EQ(o.state.lx, 1000);
    CHECK_EQ(o.state.rt, 50);
    mergeInputSamples(sample(false, BTN_A), sample(true, BTN_X, -500), &o);
    CHECK(o.connected);
    CHECK_EQ(o.state.buttons, BTN_X);
    CHECK_EQ(o.state.lx, -500);
    mergeInputSamples(sample(false, BTN_A, 9000), sample(false, BTN_X), &o);
    CHECK(!o.connected);
    CHECK_EQ(o.state.buttons, 0);
    CHECK_EQ(o.state.lx, 0);
}

TEST("merge: both connected -> buttons OR-ed, stick pair from the pad pushed further, max trigger") {
    InputSample o;
    mergeInputSamples(sample(true, BTN_A, 300, 200, 10), sample(true, BTN_X | BTN_RB, -20000, 5, 200), &o);
    CHECK_EQ(o.state.buttons, BTN_A | BTN_X | BTN_RB);
    CHECK_EQ(o.state.lx, -20000);
    CHECK_EQ(o.state.ly, 5);  // Y comes with X from the same pad
    CHECK_EQ(o.state.rt, 200);
}

TEST("merged source: seq changes on any change, shift chord across pads, survives a pad dropping") {
    auto* a = new MockInput(false);
    auto* b = new MockInput(true);
    MergedSource m{std::unique_ptr<IInputSource>(a), std::unique_ptr<IInputSource>(b)};
    std::string err;
    CHECK(m.open(&err));
    int wakes = 0;
    m.setWaker([&] { wakes++; });
    InputSample o;
    m.read(&o);
    CHECK(!o.connected);
    uint64_t s0 = o.seq;
    a->set(GamepadState{}, true);
    m.read(&o);
    CHECK(o.connected && o.seq != s0);
    CHECK(m.firstConnected() && !m.secondConnected());
    uint64_t s1 = o.seq;
    m.read(&o);
    CHECK_EQ(o.seq, s1);  // nothing changed
    GamepadState sq;
    sq.buttons = BTN_X;
    int before = wakes;
    b->set(sq, true);  // DualSense report wakes the engine
    CHECK_EQ(wakes, before + 1);
    m.read(&o);
    CHECK(o.seq != s1);
    CHECK_EQ(o.state.buttons, BTN_X);
    a->set(GamepadState{}, false);  // GameSir unplugged: DualSense keeps working
    m.read(&o);
    CHECK(o.connected);
    CHECK_EQ(o.state.buttons, BTN_X);
    CHECK(!m.firstConnected() && m.secondConnected());
    m.close();
}
