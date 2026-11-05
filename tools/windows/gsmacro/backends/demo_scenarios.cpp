#include "demo_scenarios.h"

#include <cmath>

namespace gsm {

namespace {

struct Builder {
    Scenario sc;
    GamepadState s;
    Time t = 0;
    Builder(const char* name, const char* desc) { sc.name = name; sc.description = desc; push(); }
    void push(bool connected = true) { sc.steps.push_back({t, s, connected}); }
    Builder& wait(double m) { t += ms(m); return *this; }
    Builder& down(uint16_t b) { s.buttons |= b; push(); return *this; }
    Builder& up(uint16_t b) { s.buttons &= (uint16_t)~b; push(); return *this; }
    Builder& stick(double lx, double ly) {
        s.lx = (int16_t)axisFromNormalized(AX_LX, lx);
        s.ly = (int16_t)axisFromNormalized(AX_LY, ly);
        push();
        return *this;
    }
    Builder& tap(uint16_t b, double holdMs) { down(b); wait(holdMs); up(b); return *this; }
    Builder& disconnect() { push(false); return *this; }
    Builder& reconnect() { push(true); return *this; }
    Scenario done() { wait(300); s = GamepadState(); push(); return sc; }
};

}  // namespace

std::vector<Scenario> builtinScenarios() {
    std::vector<Scenario> v;
    {
        Builder b("Square alone", "Square tapped with no bumper held: passes straight through, no macro.");
        b.wait(100).tap(BTN_X, 120);
        v.push_back(b.done());
    }
    {
        Builder b("L1 + Square", "Hold L1 (air roll left), tap Square: left macro runs, L1 stays held, Square is withheld.");
        b.wait(100).down(BTN_LB).wait(80).tap(BTN_X, 90).wait(600).up(BTN_LB);
        v.push_back(b.done());
    }
    {
        Builder b("R1 + Square", "Hold R1 (air roll right), tap Square: right macro runs.");
        b.wait(100).down(BTN_RB).wait(80).tap(BTN_X, 90).wait(600).up(BTN_RB);
        v.push_back(b.done());
    }
    {
        Builder b("R1 + Square x5 rapid (FIFO)", "Five taps 40 ms apart, queue capacity 2: macros run one after another with the gap between them. The 4th tap arrives while the queue is full and is dropped; the 5th fits once the 2nd macro has started.");
        b.wait(100).down(BTN_RB).wait(50);
        for (int i = 0; i < 5; i++) b.tap(BTN_X, 20).wait(20);
        b.wait(900).up(BTN_RB);
        v.push_back(b.done());
    }
    {
        Builder b("Steering during macro", "Left stick moving while L1+Square fires: the macro's stick steps override the stick while they are active; untouched controls pass through.");
        b.wait(100).stick(0.5, 0.3).down(BTN_LB).wait(50).tap(BTN_X, 60);
        for (int i = 0; i < 12; i++) b.wait(30).stick(0.5 * std::cos(i * 0.5), 0.5 * std::sin(i * 0.5));
        b.wait(200).up(BTN_LB).stick(0, 0);
        v.push_back(b.done());
    }
    {
        Builder b("Bumper after Square", "Square pressed first, then L1: no macro (the bumper must already be held), Square passes through.");
        b.wait(100).down(BTN_X).wait(40).down(BTN_LB).wait(100).up(BTN_X).wait(100).up(BTN_LB);
        v.push_back(b.done());
    }
    {
        Builder b("Both bumpers", "L1 held, then R1, then Square: the default 'latest' policy picks the right macro.");
        b.wait(100).down(BTN_LB).wait(60).down(BTN_RB).wait(60).tap(BTN_X, 60).wait(500).up(BTN_RB).up(BTN_LB);
        v.push_back(b.done());
    }
    {
        Builder b("Disconnect mid-macro", "Controller drops 60 ms into a macro: the macro and queue are cancelled and the output goes neutral.");
        b.wait(100).down(BTN_LB).wait(50).tap(BTN_X, 30).wait(30).disconnect().wait(400);
        b.s = GamepadState();
        b.reconnect();
        v.push_back(b.done());
    }
    return v;
}

void ScenarioPlayer::play(const Scenario& sc, MockInput* input) {
    stop();
    {
        std::lock_guard<std::mutex> l(m_);
        name_ = sc.name;
    }
    stop_ = false;
    playing_ = true;
    t_ = std::thread([this, sc, input] {
        auto start = std::chrono::steady_clock::now();
        for (const auto& st : sc.steps) {
            auto when = start + std::chrono::nanoseconds(st.at);
            while (!stop_ && std::chrono::steady_clock::now() < when)
                std::this_thread::sleep_until(std::min(when, std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
            if (stop_) break;
            input->set(st.state, st.connected);
        }
        playing_ = false;
    });
}

void ScenarioPlayer::stop() {
    stop_ = true;
    if (t_.joinable()) t_.join();
    playing_ = false;
}

}  // namespace gsm
