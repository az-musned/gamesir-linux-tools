// Deterministic tests of the pipeline with a FakeClock: chord detection,
// timing, FIFO, merging, cancellation, cleanup, config and parsing.
#include "../core/config.h"
#include "../core/processor.h"
#include "test.h"

using namespace gsm;

namespace {

GamepadState pad(uint16_t buttons, int lx = 0, int ly = 0) {
    GamepadState s;
    s.buttons = buttons;
    s.lx = (int16_t)lx;
    s.ly = (int16_t)ly;
    return s;
}

// Left: stick full left + Cross for 10 ms, then 10 ms more (20 ms total).
// Right: Circle for 5 ms, 5 ms more (10 ms total).
Config testConfig(int capacity = 2, double gapMs = 10) {
    Config c;
    c.settings.queueCapacity = capacity;
    c.settings.interMacroGapMs = gapMs;
    c.macros[0].name = "L";
    c.macros[0].actions = {Action::setAxis(AX_LX, -1), Action::press(BTN_A), Action::wait(10),
                           Action::release(BTN_A), Action::wait(10)};
    c.macros[1].name = "R";
    c.macros[1].actions = {Action::press(BTN_B), Action::wait(5), Action::release(BTN_B), Action::wait(5)};
    return c;
}

struct Rig {
    Processor p;
    Time t = ms(1000);
    explicit Rig(const Config& c = testConfig()) { p.setConfig(std::make_shared<const Config>(c)); }
    GamepadState at(Time when, const GamepadState& in, bool connected = true) { t = when; return p.process(in, connected, t); }
    GamepadState step(Time dt, const GamepadState& in, bool connected = true) { return at(t + dt, in, connected); }
};

}  // namespace

// ───────────── chord detection ─────────────

TEST("chord: Square alone passes through in the same sample") {
    Rig r;
    r.step(0, pad(0));  // first sample is the connect baseline
    GamepadState o = r.step(ms(1), pad(BTN_X));
    CHECK(o.has(BTN_X));
    CHECK(!r.p.scheduler().running());
    CHECK_EQ(r.p.chordCounters().passthrough, 1u);
    o = r.step(ms(5), pad(0));
    CHECK(!o.has(BTN_X));
}

TEST("chord: L1 held + Square starts the left macro in the same sample, L1 preserved") {
    Rig r;
    GamepadState o = r.step(0, pad(BTN_LB));
    CHECK(o.has(BTN_LB));
    o = r.step(ms(30), pad(BTN_LB | BTN_X));
    CHECK(r.p.scheduler().running());
    CHECK_EQ(r.p.scheduler().currentId(), 0);
    CHECK(o.has(BTN_LB));        // air roll still held
    CHECK(!o.has(BTN_X));        // Square withheld
    CHECK(o.has(BTN_A));         // macro output, no added sample of delay
    CHECK_EQ(o.lx, -32768);
}

TEST("chord: R1 held + Square starts the right macro") {
    Rig r;
    r.step(0, pad(BTN_RB));
    GamepadState o = r.step(ms(10), pad(BTN_RB | BTN_X));
    CHECK_EQ(r.p.scheduler().currentId(), 1);
    CHECK(o.has(BTN_RB) && o.has(BTN_B) && !o.has(BTN_X));
}

TEST("chord: bumper and Square in the same sample count as a chord") {
    Rig r;
    r.step(0, pad(0));
    r.step(ms(1), pad(BTN_LB | BTN_X));
    CHECK_EQ(r.p.scheduler().currentId(), 0);
}

TEST("chord: bumper pressed after Square does not trigger and Square stays normal") {
    Rig r;
    r.step(0, pad(BTN_X));
    GamepadState o = r.step(ms(20), pad(BTN_X | BTN_LB));
    CHECK(!r.p.scheduler().running());
    CHECK(o.has(BTN_X) && o.has(BTN_LB));
}

TEST("chord: Square stays withheld until released, even after the macro ends") {
    Rig r;
    r.step(0, pad(BTN_LB));
    r.step(ms(1), pad(BTN_LB | BTN_X));
    GamepadState o = r.step(ms(50), pad(BTN_LB | BTN_X));  // macro finished, Square still held
    CHECK(!r.p.scheduler().running());
    CHECK(!o.has(BTN_X));
    o = r.step(ms(5), pad(BTN_LB));
    CHECK(!o.has(BTN_X));
    // L1 still held: a new Square press is a new chord.
    r.step(ms(20), pad(BTN_LB | BTN_X));
    CHECK_EQ(r.p.scheduler().counters().started, 2u);
}

TEST("chord: after release, Square alone is normal again") {
    Rig r;
    r.step(0, pad(BTN_LB));
    r.step(ms(1), pad(BTN_LB | BTN_X));
    r.step(ms(50), pad(0));
    GamepadState o = r.step(ms(5), pad(BTN_X));
    CHECK(o.has(BTN_X));
}

TEST("chord: both bumpers - latest / left / right / ignore policies") {
    for (int pol = 0; pol < 4; pol++) {
        Config c = testConfig();
        c.settings.bothBumpers = (BothBumpers)pol;
        Rig r(c);
        r.step(0, pad(BTN_LB));
        r.step(ms(10), pad(BTN_LB | BTN_RB));  // R1 pressed last
        GamepadState o = r.step(ms(10), pad(BTN_LB | BTN_RB | BTN_X));
        int expect = pol == 0 ? 1 : pol == 1 ? 0 : pol == 2 ? 1 : -1;
        CHECK_EQ(r.p.scheduler().currentId(), expect);
        if (expect < 0) CHECK(o.has(BTN_X));  // ignored chord: Square passes through
    }
}

TEST("chord: disabled side passes Square through") {
    Config c = testConfig();
    c.settings.leftEnabled = false;
    Rig r(c);
    r.step(0, pad(BTN_LB));
    GamepadState o = r.step(ms(5), pad(BTN_LB | BTN_X));
    CHECK(!r.p.scheduler().running());
    CHECK(o.has(BTN_X));
    CHECK_EQ(r.p.chordCounters().disabled, 1u);
}

TEST("chord: custom chord and modifier buttons") {
    Config c = testConfig();
    c.settings.chordButton = BTN_Y;
    c.settings.leftModifier = BTN_DPAD_LEFT;
    Rig r(c);
    r.step(0, pad(BTN_LB));
    GamepadState o = r.step(ms(5), pad(BTN_LB | BTN_X));
    CHECK(!r.p.scheduler().running() && o.has(BTN_X));
    r.step(ms(5), pad(BTN_DPAD_LEFT));
    o = r.step(ms(5), pad(BTN_DPAD_LEFT | BTN_Y));
    CHECK_EQ(r.p.scheduler().currentId(), 0);
    CHECK(!o.has(BTN_Y));
}

// ───────────── chord order: shift first (hold Square, tap bumper) ─────────────

namespace {
Config shiftFirstConfig() {
    Config c = testConfig();
    c.settings.chordOrder = ChordOrder::ShiftFirst;
    return c;
}
}  // namespace

TEST("shift-first: Square is a shift key and never reaches the game while armed") {
    Rig r(shiftFirstConfig());
    r.step(0, pad(0));
    GamepadState o = r.step(ms(1), pad(BTN_X));
    CHECK(!o.has(BTN_X));
    CHECK(!r.p.scheduler().running());
    o = r.step(ms(50), pad(BTN_X));
    CHECK(!o.has(BTN_X));
    o = r.step(ms(5), pad(0));
    CHECK(!o.has(BTN_X));
}

TEST("shift-first: Square held + R1 tap starts the right macro in the same sample, R1 withheld") {
    Rig r(shiftFirstConfig());
    r.step(0, pad(BTN_X));
    GamepadState o = r.step(ms(20), pad(BTN_X | BTN_RB));
    CHECK_EQ(r.p.scheduler().currentId(), 1);
    CHECK(o.has(BTN_B));                  // macro output, no added sample of delay
    CHECK(!o.has(BTN_RB) && !o.has(BTN_X));
    o = r.step(ms(50), pad(BTN_X | BTN_RB));  // macro over, R1 still held: still withheld
    CHECK(!r.p.scheduler().running());
    CHECK(!o.has(BTN_RB));
    o = r.step(ms(5), pad(BTN_X));        // R1 released
    CHECK(!o.has(BTN_RB));
    o = r.step(ms(5), pad(0));
    o = r.step(ms(5), pad(BTN_RB));       // R1 alone afterwards: air roll again
    CHECK(o.has(BTN_RB));
}

TEST("shift-first: Square held + L1 tap starts the left macro") {
    Rig r(shiftFirstConfig());
    r.step(0, pad(BTN_X));
    GamepadState o = r.step(ms(5), pad(BTN_X | BTN_LB));
    CHECK_EQ(r.p.scheduler().currentId(), 0);
    CHECK(o.has(BTN_A) && !o.has(BTN_LB) && o.lx == -32768);
}

TEST("shift-first: Square and a bumper in the same sample count; bumper alone passes at once") {
    Rig r(shiftFirstConfig());
    r.step(0, pad(0));
    GamepadState o = r.step(ms(1), pad(BTN_LB));
    CHECK(o.has(BTN_LB));
    CHECK(!r.p.scheduler().running());
    r.step(ms(5), pad(0));
    r.step(ms(1), pad(BTN_X | BTN_RB));
    CHECK_EQ(r.p.scheduler().currentId(), 1);
}

TEST("shift-first: a bumper already held when Square goes down does not trigger") {
    Rig r(shiftFirstConfig());
    r.step(0, pad(BTN_LB));
    GamepadState o = r.step(ms(10), pad(BTN_LB | BTN_X));
    CHECK(!r.p.scheduler().running());
    CHECK(o.has(BTN_LB) && !o.has(BTN_X));
}

TEST("shift-first: disarmed is pure passthrough, Square included") {
    Config c = shiftFirstConfig();
    c.settings.macrosArmed = false;
    Rig r(c);
    r.step(0, pad(BTN_X));
    GamepadState o = r.step(ms(5), pad(BTN_X | BTN_RB));
    CHECK(!r.p.scheduler().running());
    CHECK(o.has(BTN_X) && o.has(BTN_RB));
    CHECK_EQ(r.p.chordCounters().disarmed, 1u);
}

TEST("shift-first: disabled side lets the bumper through") {
    Config c = shiftFirstConfig();
    c.settings.rightEnabled = false;
    Rig r(c);
    r.step(0, pad(BTN_X));
    GamepadState o = r.step(ms(5), pad(BTN_X | BTN_RB));
    CHECK(!r.p.scheduler().running());
    CHECK(o.has(BTN_RB) && !o.has(BTN_X));
    CHECK_EQ(r.p.chordCounters().disabled, 1u);
}

TEST("shift-first: repeated taps while Square stays held queue FIFO; disconnect clears withheld") {
    Rig r(shiftFirstConfig());
    r.step(0, pad(BTN_X));
    r.step(ms(1), pad(BTN_X | BTN_RB));
    r.step(ms(2), pad(BTN_X));
    r.step(ms(2), pad(BTN_X | BTN_LB));
    CHECK_EQ(r.p.scheduler().currentId(), 1);
    CHECK_EQ(r.p.scheduler().queued(), 1);
    GamepadState o = r.step(ms(1), pad(BTN_X | BTN_LB), false);
    CHECK(o == GamepadState());
    o = r.step(ms(100), pad(BTN_LB));  // reconnect with L1 held: baseline, not a tap
    CHECK(o.has(BTN_LB));
    CHECK(!r.p.scheduler().running());
}

TEST("config: chord_order round trip, default bumper_first, bad value rejected") {
    Config c = shiftFirstConfig();
    Config back;
    std::string err;
    CHECK(configFromText(configToText(c), &back, &err));
    CHECK(back.settings.chordOrder == ChordOrder::ShiftFirst);
    CHECK(configFromText("[settings]\npoll_period_us = 500\n", &back, &err));
    CHECK(back.settings.chordOrder == ChordOrder::BumperFirst);
    CHECK(!configFromText("[settings]\nchord_order = sideways\n", &back, &err));
}

// ───────────── timing ─────────────

TEST("timing: events fire exactly at their offsets") {
    Rig r;
    r.step(0, pad(BTN_LB));
    Time t0 = r.t + ms(1);
    r.at(t0, pad(BTN_LB | BTN_X));
    CHECK_EQ(r.p.nextDeadline(), t0 + ms(10));
    GamepadState o = r.at(t0 + ms(10) - 1, pad(BTN_LB));
    CHECK(o.has(BTN_A));                         // 1 ns early: still held
    o = r.at(t0 + ms(10), pad(BTN_LB));
    CHECK(!o.has(BTN_A));                        // released exactly at 10 ms
    CHECK_EQ(o.lx, -32768);                      // stick still overridden
    CHECK_EQ(r.p.nextDeadline(), t0 + ms(20));
    o = r.at(t0 + ms(20) - 1, pad(BTN_LB, 1234));
    CHECK_EQ(o.lx, -32768);
    o = r.at(t0 + ms(20), pad(BTN_LB, 1234));
    CHECK_EQ(o.lx, 1234);                        // overrides dropped at the end
    CHECK(!r.p.scheduler().running());
    CHECK_EQ(r.p.nextDeadline(), kNever);
}

TEST("timing: a late update applies all due events and does not drift") {
    Rig r;
    r.step(0, pad(BTN_LB));
    Time t0 = r.t + ms(1);
    r.at(t0, pad(BTN_LB | BTN_X));
    GamepadState o = r.at(t0 + ms(15), pad(BTN_LB));  // 5 ms late for the 10 ms event
    CHECK(!o.has(BTN_A));
    CHECK_EQ(r.p.nextDeadline(), t0 + ms(20));        // end still on the original schedule
    CHECK_EQ(r.p.scheduler().eventLateness().max, ms(5));
}

TEST("timing: fractional-millisecond waits are kept to the nanosecond") {
    Config c = testConfig();
    c.macros[0].actions = {Action::press(BTN_A), Action::wait(8.333), Action::release(BTN_A), Action::wait(0.5)};
    Rig r(c);
    r.step(0, pad(BTN_LB));
    Time t0 = r.t + ms(1);
    r.at(t0, pad(BTN_LB | BTN_X));
    CHECK_EQ(r.p.nextDeadline(), t0 + 8'333'000);
    r.at(t0 + 8'333'000, pad(BTN_LB));
    CHECK_EQ(r.p.nextDeadline(), t0 + 8'833'000);
}

// ───────────── FIFO ─────────────

TEST("fifo: rapid activations run sequentially, never overlapping, with the gap") {
    Rig r(testConfig(2, 10));
    r.step(0, pad(BTN_RB));
    Time t0 = r.t + ms(1);
    // Four taps 2 ms apart.
    for (int i = 0; i < 4; i++) {
        r.at(t0 + ms(i * 2), pad(BTN_RB | BTN_X));
        r.at(t0 + ms(i * 2 + 1), pad(BTN_RB));
    }
    const auto& c = r.p.scheduler().counters();
    CHECK_EQ(c.enqueued, 3u);   // 1 running + 2 queued
    CHECK_EQ(c.dropped, 1u);    // 4th dropped (capacity 2)
    CHECK_EQ(r.p.scheduler().queued(), 2);
    // Macro 1: t0 .. t0+10. Gap 10 ms. Macro 2 starts at t0+20.
    GamepadState o = r.at(t0 + ms(10), pad(BTN_RB));
    CHECK(!r.p.scheduler().running());
    CHECK(!o.has(BTN_B));                         // neutral during the gap
    CHECK_EQ(r.p.nextDeadline(), t0 + ms(20));
    o = r.at(t0 + ms(20) - 1, pad(BTN_RB));
    CHECK(!o.has(BTN_B));
    o = r.at(t0 + ms(20), pad(BTN_RB));
    CHECK(o.has(BTN_B));
    CHECK_EQ(r.p.scheduler().counters().started, 2u);
    // Macro 3 at t0+40.
    r.at(t0 + ms(30), pad(BTN_RB));
    o = r.at(t0 + ms(40), pad(BTN_RB));
    CHECK(o.has(BTN_B));
    r.at(t0 + ms(50), pad(BTN_RB));
    CHECK_EQ(r.p.scheduler().counters().completed, 3u);
    CHECK(!r.p.scheduler().running());
    CHECK_EQ(r.p.scheduler().queued(), 0);
}

TEST("fifo: mixed left/right activations keep arrival order") {
    Rig r(testConfig(4, 0));
    r.step(0, pad(BTN_LB));
    Time t0 = r.t + ms(1);
    r.at(t0, pad(BTN_LB | BTN_X));            // L
    r.at(t0 + ms(1), pad(BTN_RB));
    r.at(t0 + ms(2), pad(BTN_RB | BTN_X));    // R
    r.at(t0 + ms(3), pad(BTN_LB));
    r.at(t0 + ms(4), pad(BTN_LB | BTN_X));    // L
    CHECK_EQ(r.p.scheduler().queuedIdAt(0), 1);
    CHECK_EQ(r.p.scheduler().queuedIdAt(1), 0);
    r.at(t0 + ms(20), pad(BTN_LB));           // L ends exactly: one update with everything released
    CHECK_EQ(r.p.scheduler().currentId(), -1);
    CHECK_EQ(r.p.nextDeadline(), t0 + ms(20));  // gap 0: R is due now
    r.at(t0 + ms(20), pad(BTN_LB));           // next update, same time: R starts
    CHECK_EQ(r.p.scheduler().currentId(), 1);
    r.at(t0 + ms(30), pad(BTN_LB));
    r.at(t0 + ms(30), pad(BTN_LB));
    CHECK_EQ(r.p.scheduler().currentId(), 0);
}

TEST("fifo: capacity 0 means no queue - presses during a macro are dropped") {
    Rig r(testConfig(0, 0));
    r.step(0, pad(BTN_LB));
    r.step(ms(1), pad(BTN_LB | BTN_X));
    r.step(ms(1), pad(BTN_LB));
    r.step(ms(1), pad(BTN_LB | BTN_X));
    CHECK_EQ(r.p.scheduler().counters().dropped, 1u);
    CHECK_EQ(r.p.scheduler().queued(), 0);
}

TEST("fifo: shrinking capacity drops the newest waiting activations") {
    MacroScheduler s;
    s.setLimits(4, 0);
    Config c = testConfig();
    auto m = compileMacro(c.macros[1], 1);
    for (int i = 0; i < 5; i++) s.enqueue(m, ms(1));
    s.update(ms(1));
    CHECK_EQ(s.queued(), 4);
    s.setLimits(1, 0);
    CHECK_EQ(s.queued(), 1);
    CHECK_EQ(s.counters().dropped, 3u);
}

TEST("fifo: a queued start that is processed late is recorded and keeps internal timing") {
    Rig r(testConfig(2, 10));
    r.step(0, pad(BTN_RB));
    Time t0 = r.t + ms(1);
    r.at(t0, pad(BTN_RB | BTN_X));
    r.at(t0 + ms(1), pad(BTN_RB));
    r.at(t0 + ms(2), pad(BTN_RB | BTN_X));
    r.at(t0 + ms(10), pad(BTN_RB));                       // first ends
    r.at(t0 + ms(23), pad(BTN_RB));                       // 3 ms after allowed start
    CHECK_EQ(r.p.scheduler().startLateness().max, ms(3));
    CHECK_EQ(r.p.nextDeadline(), t0 + ms(23) + ms(5));    // relative timing preserved
}

// ───────────── merging ─────────────

TEST("merge: physical input passes through untouched when idle") {
    Rig r;
    GamepadState in;
    in.buttons = BTN_A | BTN_LB | BTN_DPAD_UP;
    in.lt = 12; in.rt = 250; in.lx = -100; in.ly = 200; in.rx = 300; in.ry = -32768;
    GamepadState o = r.step(0, in);
    CHECK(o == in);
}

TEST("merge: macro overrides only what it sets") {
    Rig r;
    r.step(0, pad(BTN_LB));
    GamepadState in = pad(BTN_LB | BTN_X | BTN_Y, 500, 600);
    in.rt = 200; in.rx = 7;
    GamepadState o = r.step(ms(1), in);
    CHECK_EQ(o.lx, -32768);   // overridden
    CHECK_EQ(o.ly, 600);      // untouched
    CHECK_EQ(o.rx, 7);
    CHECK_EQ((int)o.rt, 200);
    CHECK(o.has(BTN_Y) && o.has(BTN_LB) && o.has(BTN_A) && !o.has(BTN_X));
}

TEST("merge: suppress hides a held button, release gives it back, axis-release restores the stick") {
    Config c = testConfig();
    c.macros[0].actions = {Action::suppress(BTN_LB), Action::setAxis(AX_LY, 1), Action::wait(5),
                           Action::release(BTN_LB), Action::releaseAxis(AX_LY), Action::wait(5)};
    Rig r(c);
    r.step(0, pad(BTN_LB));
    GamepadState o = r.step(ms(1), pad(BTN_LB | BTN_X, 0, -50));
    CHECK(!o.has(BTN_LB));
    CHECK_EQ(o.ly, 32767);
    o = r.step(ms(5), pad(BTN_LB, 0, -50));
    CHECK(o.has(BTN_LB));
    CHECK_EQ(o.ly, -50);
}

TEST("merge: press wins over physical-off; release-all clears everything") {
    Config c = testConfig();
    c.macros[0].actions = {Action::press(BTN_A | BTN_B), Action::setAxis(AX_RT, 1), Action::wait(5),
                           Action::releaseAll(), Action::wait(5)};
    Rig r(c);
    r.step(0, pad(BTN_LB));
    GamepadState o = r.step(ms(1), pad(BTN_LB | BTN_X));
    CHECK(o.has(BTN_A) && o.has(BTN_B));
    CHECK_EQ((int)o.rt, 255);
    o = r.step(ms(5), pad(BTN_LB));
    CHECK(!o.has(BTN_A) && !o.has(BTN_B));
    CHECK_EQ((int)o.rt, 0);
}

TEST("merge: mergeOutput composition order") {
    Overrides ov;
    ov.forceOn = BTN_A;
    ov.forceOff = BTN_B | BTN_A;  // forceOn applied last
    GamepadState o = mergeOutput(pad(BTN_B | BTN_X | BTN_Y), BTN_X, ov);
    CHECK(o.has(BTN_A) && !o.has(BTN_B) && !o.has(BTN_X) && o.has(BTN_Y));
}

// ───────────── cancellation & cleanup ─────────────

TEST("cancel: clears the running macro, the queue and all overrides at once") {
    Rig r;
    r.step(0, pad(BTN_LB));
    r.step(ms(1), pad(BTN_LB | BTN_X));
    r.step(ms(1), pad(BTN_LB));
    r.step(ms(1), pad(BTN_LB | BTN_X));
    CHECK_EQ(r.p.scheduler().queued(), 1);
    r.p.cancel(r.t);
    GamepadState o = r.step(0, pad(BTN_LB, 99));
    CHECK(!r.p.scheduler().running());
    CHECK_EQ(r.p.scheduler().queued(), 0);
    CHECK(!o.has(BTN_A));
    CHECK_EQ(o.lx, 99);
    CHECK_EQ(r.p.scheduler().counters().cancelled, 2u);
    CHECK(r.p.scheduler().overrides().empty());
}

TEST("cancel: a new activation after cancel waits for the gap") {
    Rig r(testConfig(2, 10));
    r.step(0, pad(BTN_LB));
    r.step(ms(1), pad(BTN_LB | BTN_X));
    Time tc = r.t + ms(2);
    r.p.cancel(tc);
    r.at(tc, pad(BTN_LB));
    r.at(tc + ms(1), pad(BTN_LB | BTN_X));
    CHECK(!r.p.scheduler().running());
    CHECK_EQ(r.p.nextDeadline(), tc + ms(10));
    GamepadState o = r.at(tc + ms(10), pad(BTN_LB | BTN_X));
    CHECK(r.p.scheduler().running() && o.has(BTN_A));
}

TEST("disconnect: cancels, outputs neutral, and reconnect does not fire held chords") {
    Rig r;
    r.step(0, pad(BTN_LB));
    r.step(ms(1), pad(BTN_LB | BTN_X));
    GamepadState o = r.step(ms(2), pad(BTN_LB | BTN_X, 500), false);
    CHECK(o == GamepadState());
    CHECK(!r.p.scheduler().running());
    CHECK_EQ(r.p.scheduler().counters().cancelled, 1u);
    // Reconnect with L1 + Square already held: baseline, no edge, no macro.
    o = r.step(ms(100), pad(BTN_LB | BTN_X));
    CHECK(!r.p.scheduler().running());
    CHECK(o.has(BTN_X));   // not withheld: this press never started a macro
    // A fresh Square press after reconnect works.
    r.step(ms(5), pad(BTN_LB));
    r.step(ms(5), pad(BTN_LB | BTN_X));
    CHECK(r.p.scheduler().running());
}

TEST("cleanup: overrides a macro forgot to release are dropped at its end") {
    Config c = testConfig();
    c.macros[0].actions = {Action::press(BTN_A), Action::setAxis(AX_RY, -1), Action::suppress(BTN_LB), Action::wait(10)};
    Rig r(c);
    r.step(0, pad(BTN_LB));
    r.step(ms(1), pad(BTN_LB | BTN_X));
    GamepadState o = r.step(ms(10), pad(BTN_LB, 0, 0));
    CHECK(!o.has(BTN_A));
    CHECK(o.has(BTN_LB));
    CHECK_EQ(o.ry, 0);
}

TEST("config: editing macros while one runs affects only the next activation") {
    Rig r;
    r.step(0, pad(BTN_LB));
    Time t0 = r.t + ms(1);
    r.at(t0, pad(BTN_LB | BTN_X));
    Config c2 = testConfig();
    c2.macros[0].actions = {Action::press(BTN_Y), Action::wait(3)};
    r.p.setConfig(std::make_shared<const Config>(c2));
    GamepadState o = r.at(t0 + ms(5), pad(BTN_LB | BTN_X));
    CHECK(o.has(BTN_A) && !o.has(BTN_Y));                // old macro still running
    r.at(t0 + ms(20), pad(BTN_LB));
    o = r.at(t0 + ms(40), pad(BTN_LB | BTN_X));
    CHECK(o.has(BTN_Y));                                  // new definition
}

TEST("empty macro: chord passes Square through instead of swallowing it") {
    Config c = testConfig();
    c.macros[0].actions.clear();
    Rig r(c);
    r.step(0, pad(BTN_LB));
    GamepadState o = r.step(ms(1), pad(BTN_LB | BTN_X));
    CHECK(o.has(BTN_X));
    CHECK(!r.p.scheduler().running());
}

// ───────────── histogram ─────────────

TEST("histogram: buckets, min/max/mean and percentile upper bound") {
    Histogram h;
    for (int i = 0; i < 99; i++) h.add(us(3));   // bucket <5us
    h.add(ms(3));                                 // bucket <5ms
    CHECK_EQ(h.n, 100u);
    CHECK_EQ(h.min, us(3));
    CHECK_EQ(h.max, ms(3));
    CHECK_EQ(h.percentileUpperBound(50), us(5));
    CHECK_EQ(h.percentileUpperBound(99.5), ms(3));  // clamped to max
    CHECK_NEAR(h.meanNs(), (99 * 3000.0 + 3e6) / 100, 1);
}

// ───────────── text formats ─────────────

TEST("macro text: round trip and aliases") {
    Macro m;
    std::string err;
    bool ok = macroFromText("axis LX -1\npress Cross+Square  # jump\nwait 16.7\nrelease A+X\n"
                            "suppress L1\naxis R2 0.5\naxis-release lx\nrelease-all\nwait 2\n", &m, &err);
    CHECK(ok);
    CHECK_EQ(m.actions.size(), 9u);
    CHECK(m.actions[1].op == Op::Press && m.actions[1].buttons == (BTN_A | BTN_X));
    CHECK(m.actions[3].buttons == (BTN_A | BTN_X));
    CHECK(m.actions[5].axis == AX_RT);
    Macro m2;
    CHECK(macroFromText(macroToText(m), &m2, &err));
    CHECK_EQ(m2.actions.size(), m.actions.size());
    CHECK(macroToText(m2) == macroToText(m));
}

TEST("macro text: errors carry line numbers") {
    Macro m;
    std::string err;
    CHECK(!macroFromText("press A\n\nwait abc\n", &m, &err));
    CHECK(err.rfind("line 3:", 0) == 0);
    CHECK(!macroFromText("press Banana\n", &m, &err));
    CHECK(err.find("Banana") != std::string::npos);
    CHECK(!macroFromText("axis LX 2\n", &m, &err));
    CHECK(!macroFromText("axis L2 -0.5\n", &m, &err));
    CHECK(!macroFromText("jump\n", &m, &err));
}

TEST("config text: round trip of settings and both macros") {
    Config c = testConfig(3, 12.5);
    c.settings.pollPeriodUs = 250;
    c.settings.spinTailUs = 50;
    c.settings.bothBumpers = BothBumpers::Ignore;
    c.settings.rightEnabled = false;
    c.settings.chordButton = BTN_Y;
    Config d;
    std::string err;
    CHECK(configFromText(configToText(c), &d, &err));
    CHECK_EQ(d.settings.queueCapacity, 3);
    CHECK_NEAR(d.settings.interMacroGapMs, 12.5, 1e-9);
    CHECK_NEAR(d.settings.pollPeriodUs, 250, 1e-9);
    CHECK(d.settings.bothBumpers == BothBumpers::Ignore);
    CHECK(!d.settings.rightEnabled);
    CHECK_EQ(d.settings.chordButton, BTN_Y);
    CHECK(d.macros[0].name == "L" && d.macros[1].name == "R");
    CHECK(configToText(d) == configToText(c));
}

TEST("config text: bad files are rejected with a reason") {
    Config d;
    std::string err;
    CHECK(!configFromText("[settings]\npoll_period_us = fast\n", &d, &err));
    CHECK(err.rfind("line 2:", 0) == 0);
    CHECK(!configFromText("[bogus]\n", &d, &err));
    CHECK(!configFromText("[settings]\nchord_button = L1\n", &d, &err));  // collides with left modifier
    CHECK(configFromText("[settings]\npoll_period_us = 5\n", &d, &err)); // clamped, not rejected
    CHECK_NEAR(d.settings.pollPeriodUs, 100, 1e-9);
}

TEST("validation: flags zero-length presses and missing waits") {
    Macro m;
    m.actions = {Action::press(BTN_A), Action::release(BTN_A), Action::wait(5)};
    auto w = validateMacro(m);
    CHECK(!w.empty());
    m.actions = {Action::press(BTN_A), Action::wait(5), Action::release(BTN_A), Action::wait(5)};
    CHECK(validateMacro(m).empty());
    m.actions = {Action::wait(5), Action::press(BTN_A)};
    CHECK(!validateMacro(m).empty());  // last press dropped immediately
    CHECK(validateMacro(defaultConfig().macros[0]).empty());
}

TEST("axis conversion: full range both ways") {
    CHECK_EQ(axisFromNormalized(AX_LX, -1), -32768);
    CHECK_EQ(axisFromNormalized(AX_LX, 1), 32767);
    CHECK_EQ(axisFromNormalized(AX_LX, 0), 0);
    CHECK_EQ(axisFromNormalized(AX_LT, 1), 255);
    CHECK_EQ(axisFromNormalized(AX_RT, 2), 255);
    CHECK_NEAR(axisToNormalized(AX_LX, -32768), -1, 1e-12);
}
