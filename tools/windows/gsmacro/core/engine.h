// Engine thread: wait -> read input -> process -> submit-if-changed.
//
// One thread owns the Processor; the hot path takes no locks it can block on.
// The GUI reads a snapshot (published with try_lock, so the engine never
// waits for the GUI) and a lock-free timeline of input/output changes. Config
// changes and cancel requests are picked up at the top of the next cycle.
//
// The thread sleeps until the earliest of: the next macro event, the next
// poll (polled sources only), or a wake from an event-driven source. While
// idle with an event-driven source it only wakes every 50 ms.
#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "backend.h"
#include "processor.h"

namespace gsm {

struct EngineSnapshot {
    bool running = false;
    std::string inputName, outputName;
    Time now = 0;
    InputSample input;
    GamepadState output;
    uint16_t chordMask = 0;
    Overrides overrides;
    int currentMacro = -1;
    size_t eventIndex = 0;
    Time runStart = 0;
    int queued = 0;
    int queueIds[MacroScheduler::kMaxQueue] = {};
    SchedulerCounters sched;
    ChordCounters chords;
    // Measured on this machine, inside the engine.
    Histogram cycleCost;      // read + process + submit, per wake
    Histogram submitCost;     // IOutputSink::submit
    Histogram wakeError;      // timer wake vs. intended deadline
    Histogram eventLateness;  // macro event applied vs. scheduled
    Histogram startLateness;  // queued macro start vs. earliest allowed
    uint64_t cycles = 0, submits = 0, submitFailures = 0, inputChanges = 0, wakesByEvent = 0;
    double engineCpuSeconds = 0;  // CPU time of the engine thread only
};

struct TimelineSample {
    Time t = 0;
    GamepadState in, out;
    bool connected = false;
    int8_t macro = -1;
};

class Engine {
public:
    Engine(const Clock& clock, IWaiter& waiter) : clock_(clock), waiter_(waiter) {}
    ~Engine() { stop(); }

    bool start(IInputSource* in, IOutputSink* out, std::shared_ptr<const Config> cfg, std::string* err);
    void stop();
    bool isRunning() const { return running_.load(); }

    void setConfig(std::shared_ptr<const Config> cfg);
    void cancelMacros() { cancelReq_ = true; waiter_.wake(); }
    // Queue a macro as if its chord was pressed (GUI "Test" button, bench).
    void triggerMacro(int side) { triggerReq_.fetch_or(side == 0 ? 1u : 2u); waiter_.wake(); }
    void resetStats() { resetReq_ = true; waiter_.wake(); }

    void snapshot(EngineSnapshot* out);
    // Appends timeline samples produced since the last call (single consumer).
    void drainTimeline(std::vector<TimelineSample>* out);

private:
    void run();
    void publish(Time now, const InputSample& in, const GamepadState& out);
    void pushTimeline(const TimelineSample& s);

    const Clock& clock_;
    IWaiter& waiter_;
    IInputSource* in_ = nullptr;
    IOutputSink* out_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false}, stopReq_{false}, cancelReq_{false}, resetReq_{false};
    std::atomic<unsigned> triggerReq_{0};

    std::mutex cfgLock_;
    std::shared_ptr<const Config> pendingCfg_;
    std::atomic<bool> cfgDirty_{false};

    Processor proc_;  // engine thread only (after start)

    // Engine-thread stats, copied into the snapshot.
    EngineSnapshot local_;
    std::mutex snapLock_;
    EngineSnapshot shared_;

    static constexpr size_t kTimelineSize = 8192;
    // Heap-allocated once at construction (~320 KB): an inline array made a
    // stack-allocated Engine overflow MSVC's 1 MB default stack.
    std::vector<TimelineSample> ring_ = std::vector<TimelineSample>(kTimelineSize);
    std::atomic<size_t> ringHead_{0}, ringTail_{0};
};

}  // namespace gsm
