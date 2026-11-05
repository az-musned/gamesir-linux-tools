// gsm_bench - software timing measurements for gsmacro.
//
// EVERYTHING HERE IS SOFTWARE TIMING ON THE MACHINE IT RUNS ON. None of it is
// input-to-game latency: the controller's internal scan, USB, the driver
// stack before XInput, the game's own input sampling (120 Hz in Rocket
// League) and the display are all outside what software can see. Use a
// high-speed camera for end-to-end numbers (see BENCHMARKING.md).
//
//   gsm_bench mock     [--samples N] [--runs N]       mock backends, any OS
//   gsm_bench slots                                    list XInput slots (Windows)
//   gsm_bench poll     [-t S] [--period-us U]          read the real pad through the engine, no output device (Windows)
//   gsm_bench capture  (--slot N | --vid V [--pid P]) [-t S] [--period-us U] [--csv F] [--ignore L1,R1,Square]
//                                                       record any XInput pad (e.g. reWASD's virtual pad) and
//                                                       report per-edge macro timing (Windows, read-only)
//   gsm_bench live     --allow-virtual-pad [-t S] [--trigger N] [--config F]
//                                                       full pipeline with a virtual pad, read back via XInput (Windows)
//   common: --out FILE  (also write the report to FILE)
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../backends/mock_backend.h"
#include "../core/cpu_time.h"
#include "../core/edge_timeline.h"
#include "../core/engine.h"
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>  // timeBeginPeriod
#include "../backends/win/vigem_sink.h"
#include "../backends/win/win_waiter.h"
#include "../backends/win/xinput_source.h"
#include "../backends/win/xinput_util.h"
#else
#include "../backends/portable_waiter.h"
#endif

using namespace gsm;

namespace {

FILE* g_out = nullptr;
void say(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vprintf(fmt, a);
    va_end(a);
    if (g_out) {
        va_start(a, fmt);
        std::vfprintf(g_out, fmt, a);
        va_end(a);
    }
    std::fflush(stdout);
}

std::unique_ptr<IWaiter> waiter() {
#ifdef _WIN32
    return std::make_unique<WinWaiter>();
#else
    return std::make_unique<PortableWaiter>();
#endif
}

void hist(const char* name, const Histogram& h) {
    if (!h.n) { say("  %-34s n=0\n", name); return; }
    say("  %-34s n=%-7llu min=%9.1fus mean=%9.1fus p50<=%9.1fus p99<=%9.1fus max=%9.1fus\n", name, (unsigned long long)h.n,
        toUs(h.min), h.meanNs() / 1e3, toUs(h.percentileUpperBound(50)), toUs(h.percentileUpperBound(99)), toUs(h.max));
}

void sleepMs(int m) { std::this_thread::sleep_for(std::chrono::milliseconds(m)); }

struct Args {
    std::map<std::string, std::string> kv;
    bool has(const char* k) const { return kv.count(k) > 0; }
    std::string str(const char* k, const char* d = "") const { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
    double num(const char* k, double d) const { auto it = kv.find(k); return it == kv.end() ? d : std::atof(it->second.c_str()); }
};

Config benchMacros(double spinTailUs, double gapMs, int capacity) {
    Config c;
    c.settings.spinTailUs = spinTailUs;
    c.settings.queueCapacity = capacity;
    c.settings.interMacroGapMs = gapMs;
    for (int side = 0; side < 2; side++) {
        Macro& m = c.macros[side];
        m.actions.clear();
        m.actions.push_back(Action::setAxis(AX_LX, side ? 1.0 : -1.0));
        for (int i = 0; i < 4; i++) {  // 8 timed edges one game tick apart
            m.actions.push_back(Action::press(side ? BTN_B : BTN_A));
            m.actions.push_back(Action::wait(8.333));
            m.actions.push_back(Action::release(side ? BTN_B : BTN_A));
            m.actions.push_back(Action::wait(8.333));
        }
        m.actions.push_back(Action::releaseAxis(AX_LX));
        m.actions.push_back(Action::wait(5));
    }
    return c;
}
const double kBenchMacroMs = 8 * 8.333 + 5;

// ─────────────────────────── mock ───────────────────────────

int runMock(const Args& a) {
    int samples = (int)a.num("--samples", 2000), runs = (int)a.num("--runs", 40);
    SteadyClock clock;
    say("== gsm_bench mock: MOCK backends (no controller, no driver, no game) ==\n");
    say("   Software floor of the pipeline on this machine. Not input-to-game latency.\n\n");

    {
        auto w = waiter();
        MockInput in(true);
        RecordingSink out(clock, (size_t)samples + 16);
        Engine e(clock, *w);
        std::string err;
        e.start(&in, &out, std::make_shared<const Config>(benchMacros(300, 10, 2)), &err);
        sleepMs(50);
        out.clear();
        Histogram lat;
        GamepadState s;
        for (int i = 0; i < samples; i++) {
            s.buttons ^= BTN_Y;
            uint64_t before = out.count();
            Time t0 = clock.now();
            in.set(s);
            while (out.count() == before) std::this_thread::yield();
            lat.add(out.frames().back().t - t0);
            std::this_thread::sleep_for(std::chrono::microseconds(500 + (i % 7) * 113));
        }
        EngineSnapshot snap;
        e.snapshot(&snap);
        e.stop();
        say("1) Input change -> output submit, event-driven source (n=%d)\n", samples);
        hist("input -> submit", lat);
        hist("engine cycle (read+process+submit)", snap.cycleCost);
        hist("output submit (mock)", snap.submitCost);
    }

    say("\n2) Macro timing per spin tail (%d runs, 8 timed edges per run, 8.333 ms apart)\n", runs);
    for (double tail : {0.0, 100.0, 300.0}) {
        auto w = waiter();
        MockInput in(true);
        RecordingSink out(clock, 200000);
        Engine e(clock, *w);
        std::string err;
        in.set(GamepadState());  // connected, idle (a disconnected pad cancels macros by design)
        e.start(&in, &out, std::make_shared<const Config>(benchMacros(tail, 10, 2)), &err);
        sleepMs(30);
        EngineSnapshot s0;
        e.snapshot(&s0);
        Time t0 = clock.now();
        for (int r = 0; r < runs; r++) {
            e.triggerMacro(r % 2);
            sleepMs(120);  // macro ~72 ms + 10 ms gap
        }
        sleepMs(150);
        EngineSnapshot s1;
        e.snapshot(&s1);
        double secs = toMs(clock.now() - t0) / 1000.0;
        e.stop();
        say("  -- spin tail %.0f us --\n", tail);
        hist("macro event lateness", s1.eventLateness);
        say("  completed %llu of %d, engine CPU %.3f %% of one core\n", (unsigned long long)s1.sched.completed, runs,
            (s1.engineCpuSeconds - s0.engineCpuSeconds) / secs * 100.0);
        if (tail == 300.0) {
            std::vector<StateSample> v;
            for (auto& f : out.frames()) v.push_back({f.t, f.s});
            EdgeOptions o;
            o.ignoreButtons = 0;
            o.quiet = ms(30);  // runs are ~48 ms apart; edges inside a run at most 8.3 ms
            say("  Edge timing of the delivered output, relative to each run's first edge (scheduled: multiples of 8.333 ms):\n");
            say("%s", formatReport(analyzeTimeline(v, o)).c_str());
        }
    }

    say("\n3) FIFO queue: 10 activations 2 ms apart, alternating L/R, capacity 2, gap 10 ms\n");
    {
        auto w = waiter();
        MockInput in(true);
        RecordingSink out(clock, 100000);
        Engine e(clock, *w);
        std::string err;
        in.set(GamepadState());
        e.start(&in, &out, std::make_shared<const Config>(benchMacros(300, 10, 2)), &err);
        sleepMs(30);
        for (int i = 0; i < 10; i++) { e.triggerMacro(i % 2); sleepMs(2); }
        sleepMs(500);
        EngineSnapshot s;
        e.snapshot(&s);
        e.stop();
        std::vector<Time> starts;
        std::vector<int> sides;
        bool active = false;
        for (auto& f : out.frames()) {
            bool now = f.s.lx != 0;
            if (now && !active) { starts.push_back(f.t); sides.push_back(f.s.lx < 0 ? 0 : 1); }
            active = now;
        }
        say("  enqueued %llu, dropped %llu, completed %llu   (expected 3 / 7 / 3)\n", (unsigned long long)s.sched.enqueued,
            (unsigned long long)s.sched.dropped, (unsigned long long)s.sched.completed);
        say("  run order:");
        for (int sd : sides) say(" %s", sd ? "R" : "L");
        say("   (expected L R L)\n");
        Histogram gapErr;
        for (size_t i = 1; i < starts.size(); i++) gapErr.add(std::llabs((starts[i] - starts[i - 1]) - ms(kBenchMacroMs + 10)));
        char nm[64];
        snprintf(nm, sizeof nm, "start-to-start error vs %.3f ms", kBenchMacroMs + 10);
        hist(nm, gapErr);
    }

    say("\n4) Idle cost\n");
    for (int polled = 0; polled < 2; polled++) {
        auto w = waiter();
        MockInput in(!polled);
        RecordingSink out(clock, 0);
        Engine e(clock, *w);
        std::string err;
        Config c = benchMacros(300, 10, 2);
        c.settings.pollPeriodUs = 500;
        e.start(&in, &out, std::make_shared<const Config>(c), &err);
        sleepMs(100);
        EngineSnapshot s0, s1;
        e.snapshot(&s0);
        sleepMs(2000);
        e.snapshot(&s1);
        e.stop();
        say("  %-36s %.3f %% of one core, %llu wakes in 2 s\n", polled ? "polled every 500 us (like XInput):" : "event-driven, no input:",
            (s1.engineCpuSeconds - s0.engineCpuSeconds) / 2.0 * 100.0, (unsigned long long)(s1.cycles - s0.cycles));
        if (polled) hist("poll wake error", s1.wakeError);
    }
    return 0;
}

#ifdef _WIN32
// ─────────────────────────── Windows modes ───────────────────────────

void engineStats(const EngineSnapshot& s, double seconds, double cpuDelta) {
    hist("engine cycle (read+process+submit)", s.cycleCost);
    hist("output submit", s.submitCost);
    hist("wake / poll error (timer)", s.wakeError);
    hist("macro event lateness", s.eventLateness);
    hist("queued macro start lateness", s.startLateness);
    say("  wakes %.0f/s, submits %.0f/s, input changes %.0f/s, submit failures %llu\n", s.cycles / seconds, s.submits / seconds,
        s.inputChanges / seconds, (unsigned long long)s.submitFailures);
    say("  macros: started %llu, completed %llu, dropped %llu, cancelled %llu\n", (unsigned long long)s.sched.started,
        (unsigned long long)s.sched.completed, (unsigned long long)s.sched.dropped, (unsigned long long)s.sched.cancelled);
    say("  engine thread CPU: %.3f %% of one core\n", cpuDelta / seconds * 100.0);
}


// Records every successful submit, then forwards to the real sink.
class TimedSink final : public IOutputSink {
public:
    TimedSink(IOutputSink* inner, const Clock& c) : inner_(inner), clock_(c) {}
    const char* name() const override { return inner_->name(); }
    bool open(std::string* e) override { return inner_->open(e); }
    void close() override { inner_->close(); }
    bool submit(const GamepadState& s) override {
        bool ok = inner_->submit(s);
        Time t = clock_.now();
        std::lock_guard<std::mutex> l(m_);
        if (ok && frames_.size() < 2'000'000) frames_.push_back({t, s});
        return ok;
    }
    void setRumbleHandler(std::function<void(uint8_t, uint8_t)> h) override { inner_->setRumbleHandler(std::move(h)); }
    std::vector<StateSample> frames() { std::lock_guard<std::mutex> l(m_); return frames_; }
private:
    IOutputSink* inner_;
    const Clock& clock_;
    std::mutex m_;
    std::vector<StateSample> frames_;
};

class NullSink final : public IOutputSink {
public:
    const char* name() const override { return "none (measurement only)"; }
    bool submit(const GamepadState&) override { return true; }
};

int runSlots() {
    std::string err;
    if (!xi::load(&err)) { say("XInput unavailable: %s\n", err.c_str()); return 1; }
    say("XInput slots (as seen by this process):\n");
    for (auto& s : xi::slots()) {
        if (!s.connected) { say("  slot %d: -\n", s.slot); continue; }
        const char* what = s.vid == 0x3537 ? "  <- GameSir" : (s.vid == 0x045E && s.pid == 0x028E) ? "  <- Xbox 360 identity (real or virtual)" : "";
        say("  slot %d: %04X:%04X%s\n", s.slot, s.vid, s.pid, what);
    }
    return 0;
}

int runPoll(const Args& a) {
    double secs = a.num("-t", 20);
    SteadyClock clock;
    auto w = waiter();
    XInputSource in;
    NullSink out;
    Engine e(clock, *w);
    Config c;
    c.settings.pollPeriodUs = a.num("--period-us", 500);
    c.settings.macrosArmed = false;
    std::string err;
    if (!e.start(&in, &out, std::make_shared<const Config>(c), &err)) { say("start failed: %s\n", err.c_str()); return 1; }
    say("== gsm_bench poll: real controller through the engine, NO output device, %.0f s ==\n", secs);
    say("   Move both sticks continuously. Read-only.\n");
    sleepMs(200);
    EngineSnapshot s0;
    e.snapshot(&s0);
    if (!s0.input.connected) say("   WARNING: no GameSir (VID 3537) XInput pad found - check cable / PC mode / HidHide allow-list.\n");
    Time t0 = clock.now();
    sleepMs((int)(secs * 1000));
    EngineSnapshot s1;
    e.snapshot(&s1);
    double el = toMs(clock.now() - t0) / 1000.0;
    e.stop();
    say("  poll period %.0f us; 'input changes' = new XInput packets delivered by the pad.\n", c.settings.pollPeriodUs);
    engineStats(s1, el, s1.engineCpuSeconds - s0.engineCpuSeconds);
    return 0;
}

std::vector<StateSample> recordSlot(int slot, double secs, double periodUs, Histogram* pollErr, uint64_t* polls) {
    std::vector<StateSample> v;
    SteadyClock clock;
    WinWaiter w;
    w.onThreadStart();
    Time period = us(periodUs), next = clock.now(), end = clock.now() + ms(secs * 1000);
    uint32_t lastPkt = ~0u;
    GamepadState st;
    while (clock.now() < end) {
        w.waitUntil(next, us(50));
        Time t = clock.now();
        pollErr->add(t - next);
        next += period;
        if (next < t) next = t + period;
        uint32_t pkt = 0;
        (*polls)++;
        if (!xi::getState(slot, &pkt, &st)) continue;
        if (pkt != lastPkt) { v.push_back({t, st}); lastPkt = pkt; }
    }
    return v;
}

uint16_t parseIgnore(const std::string& s) {
    if (s.empty()) return BTN_LB | BTN_RB | BTN_X;
    if (s == "none") return 0;
    uint16_t m = 0;
    size_t p = 0;
    for (;;) {
        size_t q = s.find(',', p);
        m |= buttonFromName(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return m;
}

int runCapture(const Args& a) {
    std::string err;
    if (!xi::load(&err)) { say("XInput unavailable: %s\n", err.c_str()); return 1; }
    int slot = a.has("--slot") ? (int)a.num("--slot", -1)
                               : xi::findSlot((uint16_t)std::strtoul(a.str("--vid", "0").c_str(), nullptr, 16),
                                              (uint16_t)std::strtoul(a.str("--pid", "0").c_str(), nullptr, 16));
    if (slot < 0 || slot > 3) { say("No such pad. Run 'gsm_bench slots' and pass --slot N (0-3) or --vid/--pid.\n"); return 1; }
    double secs = a.num("-t", 60), period = a.num("--period-us", 250);
    say("== gsm_bench capture: XInput slot %d, %.0f s, polled every %.0f us (read-only) ==\n", slot, secs, period);
    say("   Trigger the macro 10-20 times, ~1 s apart, without touching anything else.\n");
    Histogram pollErr;
    uint64_t polls = 0;
    auto v = recordSlot(slot, secs, period, &pollErr, &polls);
    say("  %llu polls, %zu state changes\n", (unsigned long long)polls, v.size());
    hist("poll timer error", pollErr);
    say("  Each edge time carries up to %.0f us of sampling uncertainty.\n", period);
    EdgeOptions o;
    o.ignoreButtons = parseIgnore(a.str("--ignore"));
    say("%s", formatReport(analyzeTimeline(v, o)).c_str());
    if (a.has("--csv")) {
        FILE* f = std::fopen(a.str("--csv").c_str(), "w");
        if (f) {
            std::fprintf(f, "t_ms,buttons,lt,rt,lx,ly,rx,ry\n");
            Time t0 = v.empty() ? 0 : v.front().t;
            for (auto& s : v)
                std::fprintf(f, "%.4f,%04x,%u,%u,%d,%d,%d,%d\n", toMs(s.t - t0), s.s.buttons, s.s.lt, s.s.rt, s.s.lx, s.s.ly, s.s.rx, s.s.ry);
            std::fclose(f);
            say("  wrote %s\n", a.str("--csv").c_str());
        }
    }
    return 0;
}

int runLive(const Args& a) {
    if (!a.has("--allow-virtual-pad")) {
        say("live mode creates ONE virtual Xbox 360 pad for the duration of the test (removed afterwards).\n"
            "Re-run with --allow-virtual-pad to confirm. Keep games closed.\n");
        return 2;
    }
    double secs = a.num("-t", 30);
    int triggers = (int)a.num("--trigger", 0);
    Config c;
    std::string err;
    if (a.has("--config") && !loadConfigFile(a.str("--config"), &c, &err)) { say("config: %s\n", err.c_str()); return 1; }
    c.settings.macrosArmed = false;  // only --trigger runs macros; chords pass through
    SteadyClock clock;
    auto w = waiter();
    XInputSource in;
    ViGEmSink vigem;
    TimedSink out(&vigem, clock);
    Engine e(clock, *w);
    if (!e.start(&in, &out, std::make_shared<const Config>(c), &err)) { say("start failed: %s\n", err.c_str()); return 1; }
    say("== gsm_bench live: GameSir -> gsmacro -> virtual Xbox 360 pad -> read back via XInput, %.0f s ==\n", secs);
    int vslot = -1;
    for (int i = 0; i < 40 && vslot < 0; i++) { vslot = vigem.userIndex(); if (vslot < 0) sleepMs(50); }
    if (vslot < 0) say("  virtual pad slot unknown - readback skipped\n");
    else say("  virtual pad is XInput slot %d. %s\n", vslot, triggers ? "Do not touch the controller while macros are triggered." : "Move the sticks and press buttons.");
    std::vector<StateSample> back;
    Histogram pollErr;
    uint64_t polls = 0;
    std::thread rb;
    if (vslot >= 0) rb = std::thread([&] { back = recordSlot(vslot, secs, 100, &pollErr, &polls); });
    EngineSnapshot s0;
    e.snapshot(&s0);
    Time t0 = clock.now();
    for (int i = 0; i < triggers; i++) { sleepMs(800); e.triggerMacro(i % 2); }
    while (clock.now() - t0 < ms(secs * 1000)) sleepMs(50);
    if (rb.joinable()) rb.join();
    EngineSnapshot s1;
    e.snapshot(&s1);
    double el = toMs(clock.now() - t0) / 1000.0;
    e.stop();
    engineStats(s1, el, s1.engineCpuSeconds - s0.engineCpuSeconds);
    if (vslot >= 0) {
        auto frames = out.frames();
        Histogram vis;
        size_t j = 0, unmatched = 0;
        for (size_t i = 0; i < frames.size(); i++) {
            Time limit = i + 1 < frames.size() ? frames[i + 1].t : frames[i].t + ms(50);
            while (j < back.size() && back[j].t < frames[i].t) j++;
            size_t k = j;
            while (k < back.size() && back[k].t <= limit && back[k].s != frames[i].s) k++;
            if (k < back.size() && back[k].t <= limit) vis.add(back[k].t - frames[i].t);
            else unmatched++;
        }
        say("  submit -> visible to XInput readers. Includes the readback's own sampling delay: 100 us target,\n"
            "  but the OS timer may wake later (see 'readback poll timer error'); this is an upper bound:\n");
        hist("submit -> XInput readback", vis);
        say("  %zu of %zu submitted states were superseded before the readback saw them\n", unmatched, frames.size());
        hist("readback poll timer error", pollErr);
        if (triggers) {
            say("  Macro edges as read back from the virtual pad:\n");
            say("%s", formatReport(analyzeTimeline(back, EdgeOptions())).c_str());
        }
    }
    return 0;
}
#endif

void usage() {
    say("gsm_bench - software timing measurements (NOT input-to-game latency)\n"
        "  gsm_bench mock    [--samples N] [--runs N]\n"
        "  gsm_bench slots\n"
        "  gsm_bench poll    [-t S] [--period-us U]\n"
        "  gsm_bench capture (--slot N | --vid HEX [--pid HEX]) [-t S] [--period-us U] [--csv F] [--ignore L1,R1,Square|none]\n"
        "  gsm_bench live    --allow-virtual-pad [-t S] [--trigger N] [--config F]\n"
        "  common: --out FILE\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool hasMode = argc > 1 && argv[1][0] != '-';
    std::string mode = hasMode ? argv[1] : "mock";
    Args a;
    for (int i = hasMode ? 2 : 1; i < argc; i++) {
        std::string k = argv[i];
        if (k == "--allow-virtual-pad") { a.kv[k] = "1"; continue; }
        if (i + 1 < argc) a.kv[k] = argv[++i];
        else { usage(); return 1; }
    }
    if (a.has("--out")) g_out = std::fopen(a.str("--out").c_str(), "w");
    say("gsm_bench (%s), %u hardware threads\n", mode.c_str(), std::thread::hardware_concurrency());
#ifdef _WIN32
    // The engine waits on a high-resolution waitable timer and is unaffected,
    // but the harness's own sleeps (input pacing, activation spacing) would
    // otherwise round up to the ~15.6 ms default tick.
    timeBeginPeriod(1);
    say("   harness timer resolution: 1 ms (timeBeginPeriod; engine uses its own high-resolution timer)\n");
#endif
    int rc = 1;
    if (mode == "mock") rc = runMock(a);
#ifdef _WIN32
    else if (mode == "slots") rc = runSlots();
    else if (mode == "poll") rc = runPoll(a);
    else if (mode == "capture") rc = runCapture(a);
    else if (mode == "live") rc = runLive(a);
#endif
    else usage();
    if (g_out) std::fclose(g_out);
    return rc;
}
