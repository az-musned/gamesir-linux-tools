#include "engine.h"

#include <algorithm>

#include "cpu_time.h"

namespace gsm {

bool Engine::start(IInputSource* in, IOutputSink* out, std::shared_ptr<const Config> cfg, std::string* err) {
    stop();
    in_ = in;
    out_ = out;
    if (!in_->open(err)) return false;
    if (!out_->open(err)) { in_->close(); return false; }
    proc_ = Processor();
    proc_.setConfig(std::move(cfg));
    local_ = EngineSnapshot();
    local_.inputName = in_->name();
    local_.outputName = out_->name();
    in_->setWaker([this] { waiter_.wake(); });
    IInputSource* src = in_;
    out_->setRumbleHandler([src](uint8_t l, uint8_t s) { src->setRumble(l, s); });
    stopReq_ = false;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Engine::stop() {
    if (!thread_.joinable()) return;
    stopReq_ = true;
    waiter_.wake();
    thread_.join();
    running_ = false;
    in_->setWaker(nullptr);
    out_->setRumbleHandler(nullptr);
    out_->close();
    in_->close();
    std::lock_guard<std::mutex> l(snapLock_);
    shared_.running = false;
}

void Engine::setConfig(std::shared_ptr<const Config> cfg) {
    {
        std::lock_guard<std::mutex> l(cfgLock_);
        pendingCfg_ = std::move(cfg);
    }
    cfgDirty_ = true;
    waiter_.wake();
}

void Engine::pushTimeline(const TimelineSample& s) {
    size_t h = ringHead_.load(std::memory_order_relaxed);
    size_t next = (h + 1) % kTimelineSize;
    if (next == ringTail_.load(std::memory_order_acquire)) return;  // GUI not draining: drop
    ring_[h] = s;
    ringHead_.store(next, std::memory_order_release);
}

void Engine::drainTimeline(std::vector<TimelineSample>* out) {
    size_t t = ringTail_.load(std::memory_order_relaxed);
    size_t h = ringHead_.load(std::memory_order_acquire);
    while (t != h) {
        out->push_back(ring_[t]);
        t = (t + 1) % kTimelineSize;
    }
    ringTail_.store(t, std::memory_order_release);
}

void Engine::publish(Time now, const InputSample& in, const GamepadState& out) {
    local_.running = true;
    local_.now = now;
    local_.input = in;
    local_.output = out;
    local_.chordMask = proc_.chordMask();
    const MacroScheduler& s = proc_.scheduler();
    local_.overrides = s.overrides();
    local_.currentMacro = s.currentId();
    local_.eventIndex = s.nextEventIndex();
    local_.runStart = s.runStart();
    local_.queued = s.queued();
    for (int i = 0; i < MacroScheduler::kMaxQueue; i++) local_.queueIds[i] = s.queuedIdAt(i);
    local_.sched = s.counters();
    local_.chords = proc_.chordCounters();
    local_.eventLateness = s.eventLateness();
    local_.startLateness = s.startLateness();
    local_.engineCpuSeconds = currentThreadCpuSeconds();
    // Never block the engine on the GUI: skip this publish if it is reading.
    std::unique_lock<std::mutex> l(snapLock_, std::try_to_lock);
    if (l.owns_lock()) shared_ = local_;
}

void Engine::snapshot(EngineSnapshot* out) {
    std::lock_guard<std::mutex> l(snapLock_);
    *out = shared_;
}

void Engine::run() {
    waiter_.onThreadStart();
    const bool eventDriven = in_->eventDriven();
    Time now = clock_.now();
    Time nextPoll = now;
    Time lastPublish = 0;
    InputSample lastIn;
    lastIn.seq = ~0ull;
    GamepadState lastOut;
    bool haveOut = false;
    int lastMacro = -2;

    while (!stopReq_.load(std::memory_order_relaxed)) {
        now = clock_.now();
        Time cycleStart = now;

        if (cfgDirty_.exchange(false)) {
            std::shared_ptr<const Config> c;
            {
                std::lock_guard<std::mutex> l(cfgLock_);
                c = std::move(pendingCfg_);
            }
            if (c) proc_.setConfig(std::move(c));
        }
        if (cancelReq_.exchange(false)) proc_.cancel(now);
        if (unsigned tr = triggerReq_.exchange(0)) {
            if (tr & 1) proc_.trigger(0, now);
            if (tr & 2) proc_.trigger(1, now);
        }
        if (resetReq_.exchange(false)) {
            proc_.scheduler().resetStats();
            Histogram empty;
            local_.cycleCost = local_.submitCost = local_.wakeError = empty;
            local_.cycles = local_.submits = local_.submitFailures = local_.inputChanges = local_.wakesByEvent = 0;
        }

        InputSample in;
        in_->read(&in);
        bool inputChanged = in.seq != lastIn.seq || in.connected != lastIn.connected;
        if (inputChanged) local_.inputChanges++;

        GamepadState out = proc_.process(in.state, in.connected, now);
        bool outChanged = !haveOut || out != lastOut;
        if (outChanged) {
            Time s0 = clock_.now();
            bool ok = out_->submit(out);
            local_.submitCost.add(clock_.now() - s0);
            local_.submits++;
            if (!ok) local_.submitFailures++;
            lastOut = out;
            haveOut = ok;  // a failed submit is retried on the next cycle
        }
        int macro = proc_.scheduler().currentId();
        if (inputChanged || outChanged || macro != lastMacro) {
            TimelineSample ts;
            ts.t = now;
            ts.in = in.state;
            ts.out = out;
            ts.connected = in.connected;
            ts.macro = (int8_t)macro;
            pushTimeline(ts);
        }
        lastIn = in;
        lastMacro = macro;
        local_.cycles++;
        local_.cycleCost.add(clock_.now() - cycleStart);

        if (inputChanged || outChanged || now - lastPublish >= ms(8)) {
            publish(now, in, out);
            lastPublish = now;
        }

        // Next wake: earliest macro deadline, next poll, or a safety tick.
        const Settings& st = proc_.config()->settings;
        const Time macroDeadline = proc_.nextDeadline();
        Time target = macroDeadline;
        if (!eventDriven) {
            Time period = us(st.pollPeriodUs);
            nextPoll += period;
            if (nextPoll <= now) nextPoll = now + period;  // fell behind: resync, don't sprint
            target = std::min(target, nextPoll);
        } else {
            target = std::min(target, now + ms(50));
        }
        // Spin only for macro deadlines: poll and safety wakes don't need
        // sub-timer precision, and spinning for them would waste CPU.
        Time spin = macroDeadline <= target ? us(st.spinTailUs) : 0;
        bool byEvent = waiter_.waitUntil(target, spin);
        if (byEvent) local_.wakesByEvent++;
        else local_.wakeError.add(clock_.now() - target);
    }
    // Leave the output neutral so stopping can never leave a button or stick
    // held on the virtual pad.
    GamepadState neutral;
    if (!haveOut || lastOut != neutral) {
        out_->submit(neutral);
        local_.submits++;
        TimelineSample ts;
        ts.t = clock_.now();
        ts.in = lastIn.state;
        ts.out = neutral;
        ts.connected = lastIn.connected;
        pushTimeline(ts);
    }
    publish(clock_.now(), lastIn, neutral);
}

}  // namespace gsm
