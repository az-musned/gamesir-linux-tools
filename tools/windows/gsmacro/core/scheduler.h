// FIFO macro scheduler.
//
// * One macro runs at a time. Activations that arrive while one runs wait in
//   a fixed-size FIFO (no allocation on the hot path); when it is full, new
//   activations are dropped and counted.
// * Events fire at absolute times (start + offset). A late update applies
//   every due event at once, so lateness never accumulates.
// * The next queued macro starts `gap` after the previous one's scheduled
//   end (0 allowed). Ending a macro always produces one update with every
//   override released before the next macro starts, so press/release edges
//   are never merged - but only a gap of at least one game tick guarantees
//   the GAME samples that release.
// * Ending or cancelling a macro drops every override it holds.
#pragma once
#include <array>
#include <memory>

#include "macro.h"
#include "stats.h"

namespace gsm {

struct Overrides {
    uint16_t forceOn = 0;
    uint16_t forceOff = 0;
    uint8_t axisMask = 0;
    int16_t axis[AXIS_COUNT] = {};
    void clear() { *this = Overrides(); }
    bool empty() const { return !forceOn && !forceOff && !axisMask; }
};

struct SchedulerCounters {
    uint64_t enqueued = 0, started = 0, completed = 0, dropped = 0, cancelled = 0;
};

class MacroScheduler {
public:
    static constexpr int kMaxQueue = 8;

    void setLimits(int capacity, Time gap);
    // Returns false (and counts a drop) if the queue is full.
    bool enqueue(std::shared_ptr<const CompiledMacro> m, Time now);
    // Advance to `now`: start queued macros and apply due events.
    void update(Time now);
    // Drop the running macro and the queue; overrides are cleared at once.
    void cancelAll(Time now);

    Time nextDeadline() const;
    const Overrides& overrides() const { return ov_; }

    bool running() const { return (bool)cur_; }
    const CompiledMacro* current() const { return cur_.get(); }
    int currentId() const { return cur_ ? cur_->id : -1; }
    int queued() const { return count_; }
    int queuedIdAt(int i) const { return i < count_ ? queue_[(head_ + i) % kMaxQueue].m->id : -1; }
    size_t nextEventIndex() const { return next_; }
    Time runStart() const { return start_; }

    const SchedulerCounters& counters() const { return counters_; }
    // How late each timed event (offset > 0) was applied relative to its schedule.
    const Histogram& eventLateness() const { return lateness_; }
    // How late a queued macro started relative to its earliest allowed start.
    const Histogram& startLateness() const { return startLate_; }
    void resetStats() { lateness_.reset(); startLate_.reset(); counters_ = SchedulerCounters(); }

private:
    struct Pending { std::shared_ptr<const CompiledMacro> m; Time requested = 0; };
    void apply(const MacroEvent& e);
    void finish();

    std::array<Pending, kMaxQueue> queue_{};
    int head_ = 0, count_ = 0, capacity_ = 2;
    Time gap_ = 0;

    std::shared_ptr<const CompiledMacro> cur_;
    Time start_ = 0;
    size_t next_ = 0;
    Time readyAt_ = 0;  // earliest start of the next macro
    Overrides ov_;

    SchedulerCounters counters_;
    Histogram lateness_, startLate_;
};

}  // namespace gsm
