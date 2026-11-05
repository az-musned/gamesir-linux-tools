#include "../core/edge_timeline.h"
#include "../core/processor.h"
#include "test.h"

using namespace gsm;

namespace {
GamepadState st(uint16_t b, int lx = 0) { GamepadState s; s.buttons = b; s.lx = (int16_t)lx; return s; }
}

TEST("edges: bursts split on silence; chord buttons ignored; relative times") {
    std::vector<StateSample> v;
    Time t = ms(1000);
    for (int k = 0; k < 3; k++) {
        Time b = t + ms(1000 * k);
        v.push_back({b, st(BTN_LB)});                    // ignored
        v.push_back({b + ms(5), st(BTN_LB | BTN_A, -32768)});
        v.push_back({b + ms(5 + 25 + k), st(BTN_LB, -32768)});  // 25..27 ms later
        v.push_back({b + ms(80), st(BTN_LB)});
        v.push_back({b + ms(90), st(0)});
    }
    TimelineReport r = analyzeTimeline(v, EdgeOptions());
    CHECK_EQ(r.bursts.size(), 3u);
    CHECK_EQ(r.matching, 3);
    CHECK(r.signature == "Cross down | LX -> -1.00 | Cross up | LX -> 0.00");
    CHECK(r.edges.size() == 4);
    if (r.edges.size() == 4) {
        CHECK_NEAR(r.edges[0].meanMs, 0, 1e-9);
        CHECK_NEAR(r.edges[2].meanMs, 26, 1e-9);
        CHECK_NEAR(r.edges[2].minMs, 25, 1e-9);
        CHECK_NEAR(r.edges[2].maxMs, 27, 1e-9);
        CHECK(r.edges[2].sdMs > 0.8 && r.edges[2].sdMs < 0.9);
        CHECK_NEAR(r.edges[3].meanMs, 75, 1e-9);
    }
}

TEST("edges: small axis noise is not an edge; different sequences are counted separately") {
    std::vector<StateSample> v;
    v.push_back({ms(0), st(0)});
    v.push_back({ms(1), st(0, 1000)});                  // ~3%: noise
    v.push_back({ms(10), st(BTN_B)});
    v.push_back({ms(20), st(0)});
    v.push_back({ms(1000), st(BTN_Y)});                 // different burst signature
    v.push_back({ms(1010), st(0)});
    TimelineReport r = analyzeTimeline(v, EdgeOptions());
    CHECK_EQ(r.bursts.size(), 2u);
    CHECK_EQ(r.matching, 1);
    CHECK_EQ(r.groups.size(), 2u);
    CHECK(formatReport(r).find("2 distinct sequence") != std::string::npos);
}

TEST("edges: analyzing the processor's own output reproduces the macro timing exactly") {
    Config c;
    c.macros[0].actions = {Action::setAxis(AX_LX, -1), Action::press(BTN_A), Action::wait(16.7),
                           Action::release(BTN_A), Action::wait(33.3), Action::releaseAxis(AX_LX), Action::wait(5)};
    Processor p;
    p.setConfig(std::make_shared<const Config>(c));
    std::vector<StateSample> v;
    Time t = ms(1000);
    for (int k = 0; k < 5; k++) {
        Time b = t + ms(500 * k);
        v.push_back({b, p.process(st(BTN_LB), true, b)});
        v.push_back({b + ms(1), p.process(st(BTN_LB | BTN_X), true, b + ms(1))});
        for (int g = 0; g < 10 && p.nextDeadline() != kNever; g++) {
            Time d = p.nextDeadline();
            v.push_back({d, p.process(st(BTN_LB | BTN_X), true, d)});
        }
        v.push_back({b + ms(200), p.process(st(0), true, b + ms(200))});
    }
    TimelineReport r = analyzeTimeline(v, EdgeOptions());
    CHECK_EQ(r.matching, 5);
    CHECK(r.edges.size() == 4);
    if (r.edges.size() == 4) {
        CHECK_NEAR(r.edges[2].meanMs, 16.7, 1e-6);   // Cross up
        CHECK_NEAR(r.edges[3].meanMs, 50.0, 1e-6);   // LX back to 0
        CHECK_NEAR(r.edges[3].sdMs, 0, 1e-6);
    }
}
