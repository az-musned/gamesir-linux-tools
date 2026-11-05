#include "processor.h"

namespace gsm {

GamepadState mergeOutput(const GamepadState& phys, uint16_t chordMask, const Overrides& ov) {
    GamepadState o = phys;
    o.buttons = (uint16_t)(((phys.buttons & ~chordMask) & ~ov.forceOff) | ov.forceOn);
    if (ov.axisMask) {
        for (int a = 0; a < AXIS_COUNT; a++)
            if (ov.axisMask & (1u << a)) o.setAxis((Axis)a, ov.axis[a]);
    }
    return o;
}

Processor::Processor() { setConfig(std::make_shared<const Config>(defaultConfig())); }

void Processor::setConfig(std::shared_ptr<const Config> cfg) {
    cfg_ = std::move(cfg);
    for (int i = 0; i < 2; i++) compiled_[i] = compileMacro(cfg_->macros[i], i);
    sched_.setLimits(cfg_->settings.queueCapacity, ms(cfg_->settings.interMacroGapMs));
}

int Processor::pickSide(const GamepadState& phys) const {
    const Settings& s = cfg_->settings;
    bool l = phys.has(s.leftModifier), r = phys.has(s.rightModifier);
    if (l && r) {
        switch (s.bothBumpers) {
        case BothBumpers::Left: return 0;
        case BothBumpers::Right: return 1;
        case BothBumpers::Ignore: return -1;
        case BothBumpers::Latest: return leftDownAt_ >= rightDownAt_ ? 0 : 1;
        }
    }
    return l ? 0 : r ? 1 : -1;
}

GamepadState Processor::process(const GamepadState& phys, bool connected, Time now) {
    const Settings& s = cfg_->settings;
    if (!connected) {
        // Lost the controller: stop everything and present a neutral pad.
        if (prevConnected_ || sched_.running() || sched_.queued()) sched_.cancelAll(now);
        prevConnected_ = false;
        chordSuppressed_ = false;
        leftDownAt_ = rightDownAt_ = -1;
        prev_ = GamepadState();
        return GamepadState();
    }
    if (!prevConnected_) {
        // (Re)connected: take the current state as the baseline, so buttons
        // already held do not count as fresh presses.
        prevConnected_ = true;
        prev_ = phys;
        if (phys.has(s.leftModifier)) leftDownAt_ = now;
        if (phys.has(s.rightModifier)) rightDownAt_ = now;
    }

    uint16_t pressed = (uint16_t)(phys.buttons & ~prev_.buttons);
    uint16_t released = (uint16_t)(~phys.buttons & prev_.buttons);
    if (pressed & s.leftModifier) leftDownAt_ = now;
    if (released & s.leftModifier) leftDownAt_ = -1;
    if (pressed & s.rightModifier) rightDownAt_ = now;
    if (released & s.rightModifier) rightDownAt_ = -1;

    if (pressed & s.chordButton) {
        int side = pickSide(phys);
        if (side < 0) {
            chords_.passthrough++;
        } else if (!(side == 0 ? s.leftEnabled : s.rightEnabled) || !compiled_[side] ||
                   (compiled_[side]->events.empty() && compiled_[side]->duration == 0)) {
            chords_.disabled++;  // macro off: Square behaves normally
        } else {
            (side == 0 ? chords_.left : chords_.right)++;
            sched_.enqueue(compiled_[side], now);  // a full queue counts a drop
            chordSuppressed_ = true;               // withheld even if dropped: a macro was intended
        }
    }
    if (released & s.chordButton) chordSuppressed_ = false;
    prev_ = phys;

    sched_.update(now);
    return mergeOutput(phys, chordSuppressed_ ? s.chordButton : 0, sched_.overrides());
}

}  // namespace gsm
