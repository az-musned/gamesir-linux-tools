// Edge cases: rapid activation, FIFO order, zero-gap transitions,
// simultaneous bumpers, disconnect/reconnect, Square suppression,
// cancellation, the armed switch, manual triggers and config validation.
#include <cstdio>
#include <fstream>
#include <vector>

#include "../core/config.h"
#include "../core/processor.h"
#include "test.h"

using namespace gsm;

namespace {

GamepadState pad(uint16_t buttons, int lx = 0) {
    GamepadState s;
    s.buttons = buttons;
    s.lx = (int16_t)lx;
    return s;
}

// Left: Cross held 10 ms (ends with nothing held). Right: Circle + stick
// right held for the whole 10 ms (ends while still asserting).
Config cfg(int capacity, double gapMs) {
    Config c;
    c.settings.queueCapacity = capacity;
    c.settings.interMacroGapMs = gapMs;
    c.macros[0].name = "L";
    c.macros[0].actions = {Action::press(BTN_A), Action::wait(10), Action::release(BTN_A), Action::wait(5)};
    c.macros[1].name = "R";
    c.macros[1].actions = {Action::press(BTN_B), Action::setAxis(AX_LX, 1), Action::wait(10)};
    return c;
}

// Drives the processor like the engine does: whenever a deadline is due,
// process again at that deadline (several times if needed) and record every
// output.
struct Sim {
    Processor p;
    std::vector<std::pair<Time, GamepadState>> out;
    GamepadState phys;
    bool connected = true;
    explicit Sim(const Config& c) { p.setConfig(std::make_shared<const Config>(c)); }
    void at(Time t) {
        out.push_back({t, p.process(phys, connected, t)});
        for (int guard = 0; guard < 16 && p.nextDeadline() <= t; guard++) out.push_back({t, p.process(phys, connected, t)});
    }
    // Advance to `until`, stopping at every due deadline.
    void runUntil(Time until) {
        for (int guard = 0; guard < 10000; guard++) {
            Time d = p.nextDeadline();
            if (d > until) break;
            at(d);
        }
    }
    int risingEdges(uint16_t bit) const {
        int n = 0;
        bool prev = false;
        for (auto& o : out) {
            bool on = o.second.has(bit);
            if (on && !prev) n++;
            prev = on;
        }
        return n;
    }
};

}  // namespace

// ───────────── zero-gap transitions ─────────────

TEST("zero gap: back-to-back macros still produce a release sample between them") {
    Sim s(cfg(4, 0));
    Time t = ms(1000);
    s.phys = pad(BTN_RB);
    s.at(t);
    for (int i = 0; i < 3; i++) {  // three activations queued at once
        s.phys = pad(BTN_RB | BTN_X); s.at(t + ms(i * 2 + 1));
        s.phys = pad(BTN_RB);         s.at(t + ms(i * 2 + 2));
    }
    s.runUntil(t + ms(100));
    CHECK_EQ(s.risingEdges(BTN_B), 3);           // three distinct presses, never merged
    CHECK_EQ(s.p.scheduler().counters().completed, 3u);
    CHECK_EQ(s.p.scheduler().startLateness().n, 0u);  // each started exactly on schedule
    // Starts at t+1, t+11, t+21 (macro is 10 ms, gap 0).
    std::vector<Time> starts;
    bool prev = false;
    for (auto& o : s.out) { bool on = o.second.has(BTN_B); if (on && !prev) starts.push_back(o.first); prev = on; }
    CHECK(starts.size() == 3);
    if (starts.size() == 3) {
        CHECK_EQ(starts[0], t + ms(1));
        CHECK_EQ(starts[1], t + ms(11));
        CHECK_EQ(starts[2], t + ms(21));
    }
}

TEST("zero gap: the stick override is released between macros too") {
    Sim s(cfg(4, 0));
    Time t = ms(1000);
    s.phys = pad(BTN_RB, -500);
    s.at(t);
    s.phys = pad(BTN_RB | BTN_X, -500); s.at(t + ms(1));
    s.phys = pad(BTN_RB, -500);         s.at(t + ms(2));
    s.phys = pad(BTN_RB | BTN_X, -500); s.at(t + ms(3));
    s.runUntil(t + ms(50));
    // Somewhere between the two macros the physical stick (-500) must show.
    bool sawPhysicalBetween = false, seenFirst = false;
    for (auto& o : s.out) {
        if (o.second.lx == 32767) seenFirst = true;
        else if (seenFirst && o.second.lx == -500) { sawPhysicalBetween = true; break; }
    }
    CHECK(sawPhysicalBetween);
}

TEST("gap: configured gap is honoured exactly, including fractional ms") {
    Sim s(cfg(4, 2.5));
    Time t = ms(1000);
    s.phys = pad(BTN_LB); s.at(t);
    s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(1));
    s.phys = pad(BTN_LB); s.at(t + ms(2));
    s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(3));
    s.runUntil(t + ms(100));
    std::vector<Time> starts;
    bool prev = false;
    for (auto& o : s.out) { bool on = o.second.has(BTN_A); if (on && !prev) starts.push_back(o.first); prev = on; }
    CHECK(starts.size() == 2);
    if (starts.size() == 2) CHECK_EQ(starts[1] - starts[0], ms(15) + us(2500));
}

// ───────────── rapid activation & FIFO ─────────────

TEST("rapid: 20 presses in 20 samples with capacity 8 run 9, drop 11, in order, never overlapping") {
    Sim s(cfg(8, 0));
    Time t = ms(1000);
    s.phys = pad(BTN_LB);
    s.at(t);
    for (int i = 0; i < 20; i++) {
        // alternate sides so ordering is observable
        uint16_t mod = (i % 2) ? BTN_RB : BTN_LB;
        s.phys = pad(mod); s.at(t + us(100 * (2 * i + 1)));
        s.phys = pad(mod | BTN_X); s.at(t + us(100 * (2 * i + 2)));
    }
    s.phys = pad(0);
    const auto& c = s.p.scheduler().counters();
    CHECK_EQ(c.enqueued, 9u);
    CHECK_EQ(c.dropped, 11u);
    // Queue holds activations 2..9 in arrival order: R L R L R L R L.
    for (int i = 0; i < 8; i++) CHECK_EQ(s.p.scheduler().queuedIdAt(i), (i % 2) ? 0 : 1);
    s.runUntil(t + ms(1000));
    CHECK_EQ(s.p.scheduler().counters().completed, 9u);
    // Never overlapping: Cross (left) and Circle (right) are never on together.
    for (auto& o : s.out) CHECK(!(o.second.has(BTN_A) && o.second.has(BTN_B)));
}

TEST("rapid: holding Square does not repeat; each new press is one activation") {
    Sim s(cfg(8, 0));
    Time t = ms(1000);
    s.phys = pad(BTN_LB); s.at(t);
    s.phys = pad(BTN_LB | BTN_X);
    for (int i = 1; i < 50; i++) s.at(t + ms(i));  // held 49 ms
    CHECK_EQ(s.p.scheduler().counters().enqueued, 1u);
}

// ───────────── bumpers ─────────────

TEST("bumpers: L1 and R1 in the same sample, latest policy tie -> left; R1 release -> left still works") {
    Sim s(cfg(2, 0));
    Time t = ms(1000);
    s.phys = pad(0); s.at(t);
    s.phys = pad(BTN_LB | BTN_RB); s.at(t + ms(1));
    s.phys = pad(BTN_LB | BTN_RB | BTN_X); s.at(t + ms(2));
    CHECK_EQ(s.p.scheduler().currentId(), 0);
    s.phys = pad(BTN_LB); s.at(t + ms(30));
    s.runUntil(t + ms(60));
    s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(61));
    CHECK_EQ(s.p.scheduler().currentId(), 0);
}

TEST("bumpers: L1/R1 always reach the output during macros (air roll preserved)") {
    Sim s(cfg(4, 0));
    Time t = ms(1000);
    s.phys = pad(BTN_LB); s.at(t);
    s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(1));
    s.phys = pad(BTN_LB | BTN_RB); s.at(t + ms(3));  // roll switches mid-macro
    s.runUntil(t + ms(40));
    for (auto& o : s.out) {
        CHECK(o.second.has(BTN_LB));
    }
    CHECK(s.out.back().second.has(BTN_RB));
}

// ───────────── Square suppression ─────────────

TEST("square: only the press that triggers a macro is withheld") {
    Sim s(cfg(2, 0));
    Time t = ms(1000);
    s.phys = pad(0); s.at(t);
    s.phys = pad(BTN_X); s.at(t + ms(1));
    CHECK(s.out.back().second.has(BTN_X));               // alone: passes
    s.phys = pad(BTN_X | BTN_LB); s.at(t + ms(2));
    CHECK(s.out.back().second.has(BTN_X));               // bumper after: still passes
    s.phys = pad(BTN_LB); s.at(t + ms(3));
    s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(4));
    CHECK(!s.out.back().second.has(BTN_X));              // chord: withheld
    s.phys = pad(0); s.at(t + ms(5));
    s.runUntil(t + ms(40));
    s.phys = pad(BTN_X); s.at(t + ms(41));
    CHECK(s.out.back().second.has(BTN_X));               // alone again: passes
}

TEST("square: a dropped activation (queue full) still withholds Square") {
    Sim s(cfg(0, 0));
    Time t = ms(1000);
    s.phys = pad(BTN_LB); s.at(t);
    s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(1));
    s.phys = pad(BTN_LB); s.at(t + ms(2));
    s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(3));       // dropped: capacity 0
    CHECK_EQ(s.p.scheduler().counters().dropped, 1u);
    CHECK(!s.out.back().second.has(BTN_X));
}

TEST("square: a macro can press Square itself even while the physical Square is withheld") {
    Config c = cfg(2, 0);
    c.macros[0].actions = {Action::press(BTN_X), Action::wait(5), Action::release(BTN_X), Action::wait(5)};
    Sim s(c);
    Time t = ms(1000);
    s.phys = pad(BTN_LB); s.at(t);
    s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(1));
    CHECK(s.out.back().second.has(BTN_X));
    s.at(t + ms(6));
    CHECK(!s.out.back().second.has(BTN_X));              // macro released it; physical still withheld
}

// ───────────── armed switch & manual trigger ─────────────

TEST("armed: disarmed means pure passthrough - Square passes, no macro") {
    Config c = cfg(2, 0);
    c.settings.macrosArmed = false;
    Sim s(c);
    Time t = ms(1000);
    s.phys = pad(BTN_LB); s.at(t);
    s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(1));
    CHECK(!s.p.scheduler().running());
    CHECK(s.out.back().second.has(BTN_X));
    CHECK_EQ(s.p.chordCounters().disarmed, 1u);
}

TEST("trigger: manual trigger runs FIFO, works while disarmed, and never withholds Square") {
    Config c = cfg(1, 0);
    c.settings.macrosArmed = false;
    Sim s(c);
    Time t = ms(1000);
    s.phys = pad(BTN_X); s.at(t);
    CHECK(s.p.trigger(1, t + ms(1)));
    CHECK(s.p.trigger(0, t + ms(1)));
    CHECK(!s.p.trigger(0, t + ms(1)));                   // queue (capacity 1) full
    s.at(t + ms(1));
    CHECK_EQ(s.p.scheduler().currentId(), 1);
    CHECK(s.out.back().second.has(BTN_X));               // physical Square untouched
    s.runUntil(t + ms(60));
    CHECK_EQ(s.p.scheduler().counters().completed, 2u);
    CHECK(!s.p.trigger(5, t));
}

// ───────────── cancellation ─────────────

TEST("cancel mid-macro: forced buttons and axes release in the very next output, L1 stays") {
    Sim s(cfg(4, 0));
    Time t = ms(1000);
    s.phys = pad(BTN_RB, 123); s.at(t);
    s.phys = pad(BTN_RB | BTN_X, 123); s.at(t + ms(1));
    CHECK(s.out.back().second.has(BTN_B));
    CHECK_EQ(s.out.back().second.lx, 32767);
    s.p.cancel(t + ms(3));
    s.phys = pad(BTN_RB, 123); s.at(t + ms(3));
    const GamepadState& o = s.out.back().second;
    CHECK(!o.has(BTN_B));
    CHECK_EQ(o.lx, 123);
    CHECK(o.has(BTN_RB));
    s.runUntil(t + ms(100));
    for (size_t i = s.out.size() - 1; i < s.out.size(); i++) CHECK(!s.out[i].second.has(BTN_B));
}

TEST("cancel while Square is still held keeps it withheld until released (no leak)") {
    Sim s(cfg(4, 0));
    Time t = ms(1000);
    s.phys = pad(BTN_LB); s.at(t);
    s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(1));
    s.p.cancel(t + ms(2));
    s.at(t + ms(2));
    CHECK(!s.out.back().second.has(BTN_X));
    s.phys = pad(BTN_LB); s.at(t + ms(3));
    s.phys = pad(BTN_X); s.at(t + ms(4));
    CHECK(s.out.back().second.has(BTN_X));
}

// ───────────── disconnect / reconnect ─────────────

TEST("disconnect with a queue: everything cancelled, neutral output, nothing resumes on reconnect") {
    Sim s(cfg(4, 0));
    Time t = ms(1000);
    s.phys = pad(BTN_LB); s.at(t);
    for (int i = 0; i < 3; i++) {
        s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(1 + 2 * i));
        s.phys = pad(BTN_LB); s.at(t + ms(2 + 2 * i));
    }
    CHECK_EQ(s.p.scheduler().queued(), 2);
    s.connected = false;
    s.at(t + ms(8));
    CHECK(s.out.back().second == GamepadState());
    CHECK_EQ(s.p.scheduler().counters().cancelled, 3u);
    s.runUntil(t + ms(200));
    s.connected = true;
    s.phys = pad(0);
    s.at(t + ms(300));
    s.runUntil(t + ms(400));
    CHECK_EQ(s.p.scheduler().counters().started, 1u);   // nothing resumed
    CHECK(s.out.back().second == GamepadState());
}

TEST("reconnect with L1 already held: a fresh Square press triggers the macro") {
    Sim s(cfg(4, 0));
    Time t = ms(1000);
    s.phys = pad(0); s.at(t);
    s.connected = false; s.at(t + ms(1));
    s.connected = true; s.phys = pad(BTN_LB); s.at(t + ms(50));   // baseline: L1 held
    s.phys = pad(BTN_LB | BTN_X); s.at(t + ms(51));
    CHECK_EQ(s.p.scheduler().currentId(), 0);
}

TEST("repeated disconnects never leave overrides behind") {
    Sim s(cfg(4, 0));
    Time t = ms(1000);
    for (int i = 0; i < 20; i++) {
        Time b = t + ms(20 * i);
        s.connected = true;
        s.phys = pad(BTN_RB); s.at(b);
        s.phys = pad(BTN_RB | BTN_X); s.at(b + ms(1));
        s.connected = false; s.at(b + ms(2 + (i % 5)));
        CHECK(s.p.scheduler().overrides().empty());
        CHECK(s.out.back().second == GamepadState());
    }
}

// ───────────── config validation ─────────────

TEST("config: duplicate macro sections are rejected; armed flag round-trips") {
    Config d;
    std::string err;
    CHECK(!configFromText("[macro left]\npress A\nwait 5\n[macro left]\nwait 5\n", &d, &err));
    CHECK(err.find("duplicate") != std::string::npos);
    Config c = cfg(2, 0);
    c.settings.macrosArmed = false;
    CHECK(configFromText(configToText(c), &d, &err));
    CHECK(!d.settings.macrosArmed);
    CHECK_NEAR(d.settings.interMacroGapMs, 0, 1e-12);
}

TEST("config: out-of-range values are clamped, garbage is rejected") {
    Config d;
    std::string err;
    CHECK(configFromText("[settings]\nqueue_capacity = 99\ninter_macro_gap_ms = -4\nspin_tail_us = 1e9\n", &d, &err));
    CHECK_EQ(d.settings.queueCapacity, 8);
    CHECK_NEAR(d.settings.interMacroGapMs, 0, 1e-12);
    CHECK_NEAR(d.settings.spinTailUs, 2000, 1e-9);
    CHECK(!configFromText("[settings]\nmacros_armed = maybe\n", &d, &err));
    CHECK(!configFromText("[settings]\nwhat = 1\n", &d, &err));
    CHECK(!configFromText("press A\n", &d, &err));       // outside a section
    CHECK(!configFromText("[macro right]\nwait -1\n", &d, &err));
    CHECK(!configFromText("[macro right]\nwait 99999\n", &d, &err));
    CHECK(!configFromText("[settings]\nleft_modifier = R1\n", &d, &err));  // collides with right modifier
}

TEST("validation: re-pressing a button at the instant it was released is flagged") {
    Macro m;
    m.actions = {Action::press(BTN_A), Action::wait(5), Action::release(BTN_A), Action::press(BTN_A), Action::wait(5), Action::release(BTN_A), Action::wait(5)};
    bool found = false;
    for (auto& w : validateMacro(m)) if (w.find("pressed again") != std::string::npos) found = true;
    CHECK(found);
}

TEST("examples: built-in templates and every shipped example config parse and validate") {
    for (int i = 0; i < 2; i++) CHECK(validateMacro(defaultConfig().macros[i]).empty());
    Config d;
    std::string err;
    CHECK(configFromText(configToText(defaultConfig()), &d, &err));
}

#ifdef GSM_SOURCE_DIR
TEST("examples: every shipped config/examples/*.cfg loads and its macros validate cleanly") {
    const char* files[] = {"01-examples.cfg", "02-passthrough-only.cfg", "03-zero-gap-fifo.cfg", "04-tick-aligned-template.cfg"};
    // Source tree when run from a build; ./examples when run from the release folder.
    std::string dir;
    for (std::string d : {std::string(GSM_SOURCE_DIR) + "/config/examples", std::string("examples"), std::string("../examples")})
        if (std::ifstream(d + "/" + files[0]).good()) { dir = d; break; }
    CHECK(!dir.empty());
    if (dir.empty()) return;
    for (const char* f : files) {
        Config c;
        std::string err;
        std::string path = dir + "/" + f;
        bool ok = loadConfigFile(path, &c, &err);
        if (!ok) std::printf("    %s: %s\n", f, err.c_str());
        CHECK(ok);
        for (int i = 0; i < 2; i++) {
            auto w = validateMacro(c.macros[i]);
            for (auto& x : w) std::printf("    %s macro %d: %s\n", f, i, x.c_str());
            CHECK(w.empty());
        }
    }
    // 01-examples.cfg must equal the built-in examples exactly.
    Config c;
    std::string err;
    loadConfigFile(dir + "/01-examples.cfg", &c, &err);
    Config d = defaultConfig();
    d.macros[0].name = "Wall dash left (example)";
    d.macros[1].name = "Wall dash right (example)";
    CHECK(configToText(c) == configToText(d));
}
#endif
