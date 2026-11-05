// gsm_bench — headless timing check of the engine with the MOCK backends.
//
// Measures, on the machine it runs on:
//   1. mock input change -> sink submit   (engine wake-up + processing; no USB,
//      no driver, no game - a software floor, not input-to-game latency)
//   2. macro event lateness for several spin-tail settings
//   3. engine-thread CPU while idle and while running macros
//
// Usage: gsm_bench [passthrough-samples=2000] [macro-runs=40]
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "../backends/mock_backend.h"
#include "../core/engine.h"
#include "../core/cpu_time.h"
#ifdef _WIN32
#include "../backends/win/win_waiter.h"
#else
#include "../backends/portable_waiter.h"
#endif

using namespace gsm;

namespace {

std::unique_ptr<IWaiter> waiter() {
#ifdef _WIN32
    return std::make_unique<WinWaiter>();
#else
    return std::make_unique<PortableWaiter>();
#endif
}

void printHist(const char* name, const Histogram& h) {
    if (!h.n) { std::printf("  %-34s n=0\n", name); return; }
    std::printf("  %-34s n=%-6llu min=%8.1fus mean=%8.1fus p50<=%8.1fus p99<=%8.1fus max=%8.1fus\n", name,
                (unsigned long long)h.n, toUs(h.min), h.meanNs() / 1e3, toUs(h.percentileUpperBound(50)),
                toUs(h.percentileUpperBound(99)), toUs(h.max));
}

Config benchConfig(double spinTailUs) {
    Config c;
    c.settings.spinTailUs = spinTailUs;
    c.settings.queueCapacity = 2;
    c.settings.interMacroGapMs = 10;
    // 10 timed events, one game tick apart.
    Macro& m = c.macros[0];
    m.actions.clear();
    for (int i = 0; i < 5; i++) {
        m.actions.push_back(Action::press(BTN_A));
        m.actions.push_back(Action::wait(8.333));
        m.actions.push_back(Action::release(BTN_A));
        m.actions.push_back(Action::wait(8.333));
    }
    return c;
}

void sleepMs(int m) { std::this_thread::sleep_for(std::chrono::milliseconds(m)); }

}  // namespace

int main(int argc, char** argv) {
    int samples = argc > 1 ? std::atoi(argv[1]) : 2000;
    int runs = argc > 2 ? std::atoi(argv[2]) : 40;
    SteadyClock clock;
    std::printf("gsm_bench: mock backends on THIS machine. Software-only timings; not input-to-game latency.\n\n");

    // 1. Passthrough: input change -> submit.
    {
        auto w = waiter();
        MockInput in(true);
        RecordingSink out(clock, (size_t)samples + 16);
        Engine e(clock, *w);
        std::string err;
        e.start(&in, &out, std::make_shared<const Config>(benchConfig(100)), &err);
        sleepMs(50);
        out.clear();
        Histogram lat;
        GamepadState s;
        for (int i = 0; i < samples; i++) {
            s.buttons ^= BTN_B;  // B is not part of any chord
            uint64_t before = out.count();
            Time t0 = clock.now();
            in.set(s);
            while (out.count() == before) std::this_thread::yield();
            auto f = out.frames();
            lat.add(f.back().t - t0);
            std::this_thread::sleep_for(std::chrono::microseconds(500 + (i % 7) * 113));  // de-correlate from timers
        }
        e.stop();
        std::printf("1) Mock input change -> virtual-output submit (event-driven wake)\n");
        printHist("input->submit", lat);
    }

    // 2/3. Macro timing per spin tail, and engine CPU.
    std::printf("\n2) Macro event lateness (10 events per run, 8.333 ms apart), %d runs per setting\n", runs);
    for (double tail : {0.0, 100.0, 300.0}) {
        auto w = waiter();
        MockInput in(true);
        RecordingSink out(clock, 0);
        Engine e(clock, *w);
        std::string err;
        e.start(&in, &out, std::make_shared<const Config>(benchConfig(tail)), &err);
        sleepMs(30);
        EngineSnapshot s0;
        e.snapshot(&s0);
        Time t0 = clock.now();
        GamepadState held;
        held.buttons = BTN_LB;
        in.set(held);
        sleepMs(20);  // the first sample after connect is a baseline, not a press
        for (int r = 0; r < runs; r++) {
            GamepadState p = held;
            p.buttons |= BTN_X;
            in.set(p);
            sleepMs(5);
            in.set(held);
            sleepMs(100);  // macro is ~83 ms + 10 ms gap
        }
        sleepMs(150);  // let the last macro finish before the snapshot
        EngineSnapshot s1;
        e.snapshot(&s1);
        Time t1 = clock.now();
        e.stop();
        char name[64];
        std::snprintf(name, sizeof name, "spin tail %3.0f us: event lateness", tail);
        printHist(name, s1.eventLateness);
        std::printf("  %-34s chords %llu, started %llu, completed %llu, dropped %llu\n", "",
                    (unsigned long long)(s1.chords.left + s1.chords.right), (unsigned long long)s1.sched.started,
                    (unsigned long long)s1.sched.completed, (unsigned long long)s1.sched.dropped);
        std::printf("  %-34s %.3f %% of one core (engine thread, while running macros)\n", "",
                    (s1.engineCpuSeconds - s0.engineCpuSeconds) / (toMs(t1 - t0) / 1000.0) * 100.0);
    }

    // Idle CPU.
    {
        auto w = waiter();
        MockInput in(true);
        RecordingSink out(clock, 0);
        Engine e(clock, *w);
        std::string err;
        e.start(&in, &out, std::make_shared<const Config>(benchConfig(100)), &err);
        sleepMs(100);
        EngineSnapshot s0, s1;
        e.snapshot(&s0);
        sleepMs(2000);
        e.snapshot(&s1);
        e.stop();
        std::printf("\n3) Idle engine thread (event-driven source, no input): %.4f %% of one core, %llu wakes in 2 s\n",
                    (s1.engineCpuSeconds - s0.engineCpuSeconds) / 2.0 * 100.0, (unsigned long long)(s1.cycles - s0.cycles));
    }
    {
        auto w = waiter();
        MockInput in(false);  // polled, like XInput
        RecordingSink out(clock, 0);
        Engine e(clock, *w);
        std::string err;
        Config c = benchConfig(100);
        c.settings.pollPeriodUs = 500;
        e.start(&in, &out, std::make_shared<const Config>(c), &err);
        sleepMs(100);
        EngineSnapshot s0, s1;
        e.snapshot(&s0);
        sleepMs(2000);
        e.snapshot(&s1);
        e.stop();
        std::printf("   Polled source at 500 us (XInput-style), idle: %.3f %% of one core, %llu polls in 2 s\n",
                    (s1.engineCpuSeconds - s0.engineCpuSeconds) / 2.0 * 100.0, (unsigned long long)(s1.cycles - s0.cycles));
        printHist("poll wake error", s1.wakeError);
    }
    return 0;
}
