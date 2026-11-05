#include "scheduler.h"

#include <algorithm>

namespace gsm {

void MacroScheduler::setLimits(int capacity, Time gap) {
    capacity_ = std::clamp(capacity, 0, kMaxQueue);
    gap_ = std::max<Time>(0, gap);
    // Shrinking the queue drops the newest waiting activations.
    while (count_ > capacity_) {
        int tail = (head_ + count_ - 1) % kMaxQueue;
        queue_[tail].m.reset();
        count_--;
        counters_.dropped++;
    }
}

bool MacroScheduler::enqueue(std::shared_ptr<const CompiledMacro> m, Time now) {
    if (!m) return false;
    // Capacity counts activations WAITING behind a running macro. With nothing
    // running, the head of the queue starts on the next update, so it doesn't
    // count against the capacity.
    int limit = capacity_ + (cur_ ? 0 : 1);
    if (count_ >= limit || count_ >= kMaxQueue) {
        counters_.dropped++;
        return false;
    }
    queue_[(head_ + count_) % kMaxQueue] = Pending{std::move(m), now};
    count_++;
    counters_.enqueued++;
    return true;
}

void MacroScheduler::apply(const MacroEvent& e) {
    switch (e.op) {
    case Op::Press: ov_.forceOn |= e.buttons; ov_.forceOff &= (uint16_t)~e.buttons; break;
    case Op::Release: ov_.forceOn &= (uint16_t)~e.buttons; ov_.forceOff &= (uint16_t)~e.buttons; break;
    case Op::Suppress: ov_.forceOff |= e.buttons; ov_.forceOn &= (uint16_t)~e.buttons; break;
    case Op::SetAxis: ov_.axisMask |= (uint8_t)(1u << e.axis); ov_.axis[e.axis] = (int16_t)e.raw; break;
    case Op::ReleaseAxis: ov_.axisMask &= (uint8_t)~(1u << e.axis); break;
    case Op::ReleaseAll: ov_.clear(); break;
    case Op::Wait: break;
    }
}

void MacroScheduler::finish() {
    ov_.clear();
    readyAt_ = start_ + cur_->duration + gap_;
    cur_.reset();
    next_ = 0;
    counters_.completed++;
}

void MacroScheduler::update(Time now) {
    for (;;) {
        if (!cur_) {
            if (count_ == 0) return;
            Pending& p = queue_[head_];
            Time startAt = std::max(readyAt_, p.requested);
            if (now < startAt) return;
            // Start now. If we are late, the whole macro shifts (keeping its
            // internal timing intact) and the delay is recorded.
            if (now > startAt) startLate_.add(now - startAt);
            cur_ = std::move(p.m);
            head_ = (head_ + 1) % kMaxQueue;
            count_--;
            start_ = now;
            next_ = 0;
            ov_.clear();
            counters_.started++;
        }
        const auto& ev = cur_->events;
        while (next_ < ev.size() && start_ + ev[next_].offset <= now) {
            if (ev[next_].offset > 0) lateness_.add(now - (start_ + ev[next_].offset));
            apply(ev[next_]);
            next_++;
        }
        if (next_ < ev.size() || now < start_ + cur_->duration) return;
        if (cur_->duration > 0) lateness_.add(now - (start_ + cur_->duration));
        finish();
        // Return instead of starting the next queued macro in this same
        // update: the caller then emits one output sample with every
        // override released, so even with a 0 ms gap each macro's buttons
        // show a real release before the next macro presses them again.
        // nextDeadline() is already due, so the engine runs the next update
        // immediately (same scheduled start time - no drift).
        return;
    }
}

void MacroScheduler::cancelAll(Time now) {
    uint64_t n = (cur_ ? 1 : 0) + (uint64_t)count_;
    counters_.cancelled += n;
    cur_.reset();
    for (auto& p : queue_) p.m.reset();
    head_ = count_ = 0;
    next_ = 0;
    ov_.clear();
    readyAt_ = now + gap_;
}

Time MacroScheduler::nextDeadline() const {
    if (cur_) {
        if (next_ < cur_->events.size()) return start_ + cur_->events[next_].offset;
        return start_ + cur_->duration;
    }
    if (count_ > 0) return std::max(readyAt_, queue_[head_].requested);
    return kNever;
}

}  // namespace gsm
