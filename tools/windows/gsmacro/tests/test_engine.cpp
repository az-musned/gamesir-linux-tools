// Engine integration tests on the real clock with mock backends. These check
// behaviour (ordering, sequencing, cleanup, threading), with loose timing
// tolerances so they don't flake on a loaded machine. They are NOT
// benchmarks.
#include <thread>

#include "../backends/mock_backend.h"
#include "../backends/portable_waiter.h"
#include "../core/engine.h"
#include "test.h"

using namespace gsm;

namespace {

Config engineConfig() {
    Config c;
    c.settings.queueCapacity = 2;
    c.settings.interMacroGapMs = 10;
    c.settings.pollPeriodUs = 500;
    c.macros[0].actions = {Action::setAxis(AX_LX, -1), Action::press(BTN_A), Action::wait(20),
                           Action::release(BTN_A), Action::wait(20)};
    c.macros[1].actions = {Action::press(BTN_B), Action::wait(15), Action::release(BTN_B), Action::wait(15)};
    return c;
}

GamepadState pad(uint16_t b) { GamepadState s; s.buttons = b; return s; }

void sleepMs(int m) { std::this_thread::sleep_for(std::chrono::milliseconds(m)); }

// Wait (up to 500 ms) for a condition instead of assuming a fixed sleep is
// long enough - keeps the state checks robust on a loaded machine.
template <class F>
bool eventually(F pred) {
    for (int i = 0; i < 500; i++) {
        if (pred()) return true;
        sleepMs(1);
    }
    return pred();
}

// Time of the first frame (at or after index `from`) matching pred; -1 if none.
template <class F>
Time firstFrame(const std::vector<RecordingSink::Frame>& f, F pred, size_t from = 0, size_t* idx = nullptr) {
    for (size_t i = from; i < f.size(); i++)
        if (pred(f[i].s)) { if (idx) *idx = i; return f[i].t; }
    return -1;
}

void runEngineMacroCheck(bool eventDriven) {
    SteadyClock clock;
    PortableWaiter waiter;
    MockInput in(eventDriven);
    RecordingSink out(clock);
    Engine e(clock, waiter);
    std::string err;
    in.set(pad(0));
    CHECK(e.start(&in, &out, std::make_shared<const Config>(engineConfig()), &err));
    sleepMs(20);
    in.set(pad(BTN_LB));
    sleepMs(20);
    in.set(pad(BTN_LB | BTN_X));
    sleepMs(30);
    in.set(pad(BTN_LB));
    sleepMs(60);
    e.stop();

    auto f = out.frames();
    size_t iPress = 0, iRel = 0, iEnd = 0;
    Time tPress = firstFrame(f, [](const GamepadState& s) { return s.has(BTN_A); }, 0, &iPress);
    Time tRel = tPress < 0 ? -1 : firstFrame(f, [](const GamepadState& s) { return !s.has(BTN_A); }, iPress, &iRel);
    Time tEnd = tRel < 0 ? -1 : firstFrame(f, [](const GamepadState& s) { return s.lx == 0; }, iRel, &iEnd);
    CHECK(tPress > 0 && tRel > 0 && tEnd > 0);
    if (tPress > 0 && tRel > 0 && tEnd > 0) {
        // Scheduled 20 ms apart; allow generous scheduler noise on CI machines.
        CHECK_NEAR(toMs(tRel - tPress), 20.0, 5.0);
        CHECK_NEAR(toMs(tEnd - tRel), 20.0, 5.0);
        CHECK(f[iPress].s.has(BTN_LB));      // L1 preserved during the macro
        CHECK(!f[iPress].s.has(BTN_X));      // Square withheld
        CHECK_EQ(f[iPress].s.lx, -32768);
    }
    CHECK(out.last() == pad(0) || out.last() == pad(BTN_LB));
    EngineSnapshot s;
    e.snapshot(&s);
    CHECK_EQ(s.sched.completed, 1u);
    CHECK(!s.running);
}

}  // namespace

TEST("engine: event-driven source runs a macro with correct ordering and spacing") { runEngineMacroCheck(true); }
TEST("engine: polled source runs a macro with correct ordering and spacing") { runEngineMacroCheck(false); }

TEST("engine: Square alone reaches the sink and macros never overlap under rapid presses") {
    SteadyClock clock;
    PortableWaiter waiter;
    MockInput in(true);
    RecordingSink out(clock);
    Engine e(clock, waiter);
    std::string err;
    CHECK(e.start(&in, &out, std::make_shared<const Config>(engineConfig()), &err));
    in.set(pad(BTN_X));
    CHECK(eventually([&] { return out.last().has(BTN_X); }));
    in.set(pad(0));
    sleepMs(10);
    in.set(pad(BTN_RB));
    for (int i = 0; i < 3; i++) {
        sleepMs(4);
        in.set(pad(BTN_RB | BTN_X));
        sleepMs(4);
        in.set(pad(BTN_RB));
    }
    sleepMs(200);
    e.stop();
    // Count B press edges: 3 macros, each separated by a B-off frame.
    auto f = out.frames();
    int edges = 0;
    bool prevB = false;
    for (auto& fr : f) {
        bool b = fr.s.has(BTN_B);
        if (b && !prevB) edges++;
        prevB = b;
    }
    CHECK_EQ(edges, 3);
    EngineSnapshot s;
    e.snapshot(&s);
    CHECK_EQ(s.sched.completed, 3u);
    CHECK_EQ(s.sched.dropped, 0u);
}

TEST("engine: cancel request, disconnect and stop leave a neutral output") {
    SteadyClock clock;
    PortableWaiter waiter;
    MockInput in(true);
    RecordingSink out(clock);
    Engine e(clock, waiter);
    std::string err;
    CHECK(e.start(&in, &out, std::make_shared<const Config>(engineConfig()), &err));
    in.set(pad(BTN_LB));
    sleepMs(5);
    in.set(pad(BTN_LB | BTN_X));
    CHECK(eventually([&] { return out.last().has(BTN_A); }));
    e.cancelMacros();
    CHECK(eventually([&] { return !out.last().has(BTN_A) && out.last().lx == 0; }));
    // Disconnect mid-macro.
    in.set(pad(BTN_LB));
    sleepMs(15);
    in.set(pad(BTN_LB | BTN_X));
    sleepMs(5);
    in.set(pad(BTN_LB | BTN_X), false);
    CHECK(eventually([&] { return out.last() == GamepadState(); }));
    e.stop();
    // Restart works and picks up a config change.
    Config c2 = engineConfig();
    c2.macros[0].actions = {Action::press(BTN_Y), Action::wait(10)};
    in.set(pad(0), true);
    CHECK(e.start(&in, &out, std::make_shared<const Config>(engineConfig()), &err));
    e.setConfig(std::make_shared<const Config>(c2));
    sleepMs(5);
    in.set(pad(BTN_LB));
    sleepMs(5);
    in.set(pad(BTN_LB | BTN_X));
    CHECK(eventually([&] { return out.last().has(BTN_Y); }));
    e.stop();
}

TEST("engine: rumble from the sink is forwarded to the input source") {
    struct RumbleSink : IOutputSink {
        std::function<void(uint8_t, uint8_t)> h;
        const char* name() const override { return "rumble"; }
        bool submit(const GamepadState&) override { return true; }
        void setRumbleHandler(std::function<void(uint8_t, uint8_t)> f) override { h = std::move(f); }
    } sink;
    SteadyClock clock;
    PortableWaiter waiter;
    MockInput in(true);
    Engine e(clock, waiter);
    std::string err;
    CHECK(e.start(&in, &sink, std::make_shared<const Config>(engineConfig()), &err));
    CHECK((bool)sink.h);
    if (sink.h) sink.h(200, 50);
    CHECK_EQ((int)in.rumbleLarge(), 200);
    CHECK_EQ((int)in.rumbleSmall(), 50);
    e.stop();
    CHECK(!sink.h);
}

TEST("engine: timeline records input and output changes") {
    SteadyClock clock;
    PortableWaiter waiter;
    MockInput in(true);
    RecordingSink out(clock);
    Engine e(clock, waiter);
    std::string err;
    CHECK(e.start(&in, &out, std::make_shared<const Config>(engineConfig()), &err));
    in.set(pad(BTN_LB));
    sleepMs(5);
    in.set(pad(BTN_LB | BTN_X));
    sleepMs(60);
    std::vector<TimelineSample> tl;
    e.drainTimeline(&tl);
    e.stop();
    bool sawMacro = false, sawA = false;
    for (auto& s : tl) { if (s.macro == 0) sawMacro = true; if (s.out.has(BTN_A)) sawA = true; }
    CHECK(sawMacro && sawA);
    CHECK(tl.size() >= 4);
}
