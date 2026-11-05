// The per-sample pipeline: chord detection -> scheduler -> merge.
//
// process() is pure apart from its own state and the clock value passed in.
// It does not allocate, so the same code runs in the engine thread, the demo
// and the tests.
//
// Chord rule: the macro fires when the chord button (Square) goes DOWN while
// a modifier (L1 or R1) is ALREADY held, or goes down in the same sample.
// Square pressed alone is passed through at once: nothing waits to see
// whether a bumper follows, so normal Square has no added delay. A Square
// press that starts a macro is withheld from the game until Square is
// released. L1/R1 always pass through unless a macro step overrides them.
//
// chord_order = shift_first reverses the roles: Square is held as a shift
// key (hidden from the game while macros are armed) and an L1/R1 press picks
// the side; that bumper press is withheld until released. See ChordOrder.
#pragma once
#include <memory>

#include "config.h"
#include "scheduler.h"

namespace gsm {

struct ChordCounters {
    uint64_t left = 0, right = 0, passthrough = 0, disabled = 0, disarmed = 0, manual = 0;
};

GamepadState mergeOutput(const GamepadState& phys, uint16_t chordMask, const Overrides& ov);

class Processor {
public:
    Processor();
    // Compiles both macros; the running macro keeps its own copy, so an edit
    // takes effect from the next activation.
    void setConfig(std::shared_ptr<const Config> cfg);
    std::shared_ptr<const Config> config() const { return cfg_; }

    GamepadState process(const GamepadState& phys, bool connected, Time now);
    Time nextDeadline() const { return sched_.nextDeadline(); }
    void cancel(Time now) { sched_.cancelAll(now); }
    // Queue a macro without a chord (GUI "Test" button, benchmarks). Ignores
    // the armed/enabled switches - it is an explicit request - but still
    // obeys the FIFO. Returns false if the macro is empty or the queue is full.
    bool trigger(int side, Time now);

    const MacroScheduler& scheduler() const { return sched_; }
    MacroScheduler& scheduler() { return sched_; }
    // Physical buttons currently hidden from the game by the chord logic.
    uint16_t chordMask() const;
    const ChordCounters& chordCounters() const { return chords_; }
    const CompiledMacro* compiled(int side) const { return compiled_[side].get(); }

private:
    int pickSide(const GamepadState& phys) const;
    bool sideUsable(int side) const;
    void processShiftFirst(const GamepadState& phys, uint16_t pressed, uint16_t released, Time now);

    std::shared_ptr<const Config> cfg_;
    std::shared_ptr<const CompiledMacro> compiled_[2];
    MacroScheduler sched_;
    GamepadState prev_;
    bool prevConnected_ = false;
    uint16_t withheld_ = 0;  // triggering press held back until released (Square, or a bumper in shift-first)
    Time leftDownAt_ = -1, rightDownAt_ = -1;
    ChordCounters chords_;
};

}  // namespace gsm
