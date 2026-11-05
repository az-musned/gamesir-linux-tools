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

bool Processor::trigger(int side, Time now) {
    if (side < 0 || side > 1 || !compiled_[side]) return false;
    if (compiled_[side]->events.empty() && compiled_[side]->duration == 0) return false;
    if (!sched_.enqueue(compiled_[side], now)) return false;
    chords_.manual++;
    return true;
}

GamepadState Processor::process(const GamepadState& phys, bool connected, Time now) {
    const Settings& s = cfg_->settings;
    if (!connected) {
        // Lost the controller: stop everything and present a neutral pad.
        if (prevConnected_ || sched_.running() || sched_.queued()) sched_.cancelAll(now);
        prevConnected_ = false;
        withheld_ = 0;
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

    if (s.chordOrder == ChordOrder::ShiftFirst) {
        processShiftFirst(phys, pressed, released, now);
    } else {
        if (pressed & s.chordButton) {
            int side = pickSide(phys);
            if (side >= 0 && !s.macrosArmed) {
                chords_.disarmed++;  // master switch off: Square behaves normally
            } else if (side < 0) {
                chords_.passthrough++;
            } else if (!sideUsable(side)) {
                chords_.disabled++;  // macro off: Square behaves normally
            } else {
                (side == 0 ? chords_.left : chords_.right)++;
                sched_.enqueue(compiled_[side], now);  // a full queue counts a drop
                withheld_ |= s.chordButton;            // withheld even if dropped: a macro was intended
            }
        }
        if (released & s.chordButton) withheld_ &= (uint16_t)~s.chordButton;
    }
    prev_ = phys;

    sched_.update(now);
    return mergeOutput(phys, chordMask(), sched_.overrides());
}

bool Processor::sideUsable(int side) const {
    const Settings& s = cfg_->settings;
    return (side == 0 ? s.leftEnabled : s.rightEnabled) && compiled_[side] &&
           !(compiled_[side]->events.empty() && compiled_[side]->duration == 0);
}

// Shift-first: the chord button is held as a shift key and a bumper TAP picks
// the side. The bumper press that starts a macro is withheld until that bumper
// is released (it never becomes air roll); a bumper tapped without the shift
// passes through at once. The shift button itself is masked in chordMask().
void Processor::processShiftFirst(const GamepadState& phys, uint16_t pressed, uint16_t released, Time now) {
    const Settings& s = cfg_->settings;
    // Release first, so a release+re-press inside one sample is not merged.
    withheld_ &= (uint16_t)~(released & (s.leftModifier | s.rightModifier));
    if (!phys.has(s.chordButton)) return;  // shift not held (it may be pressed in this same sample: that counts)
    bool l = (pressed & s.leftModifier) != 0, r = (pressed & s.rightModifier) != 0;
    if (!l && !r) return;
    int side = l && r ? (s.bothBumpers == BothBumpers::Right ? 1 : s.bothBumpers == BothBumpers::Ignore ? -1 : 0) : l ? 0 : 1;
    if (side < 0) { chords_.passthrough++; return; }
    if (!s.macrosArmed) { chords_.disarmed++; return; }  // pure passthrough
    if (!sideUsable(side)) { chords_.disabled++; return; }  // bumper behaves normally
    (side == 0 ? chords_.left : chords_.right)++;
    sched_.enqueue(compiled_[side], now);  // a full queue counts a drop
    withheld_ |= side == 0 ? s.leftModifier : s.rightModifier;
}

uint16_t Processor::chordMask() const {
    const Settings& s = cfg_->settings;
    uint16_t m = withheld_;
    if (s.chordOrder == ChordOrder::ShiftFirst && s.macrosArmed) m |= s.chordButton;  // shift key never reaches the game
    return m;
}

}  // namespace gsm
