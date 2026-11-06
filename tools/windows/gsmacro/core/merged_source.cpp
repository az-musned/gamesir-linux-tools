#include "merged_source.h"

namespace gsm {

static int mag2(int x, int y) { return x * x + y * y; }

void mergeInputSamples(const InputSample& a, const InputSample& b, InputSample* out) {
    out->connected = a.connected || b.connected;
    if (!a.connected || !b.connected) {
        out->state = a.connected ? a.state : b.connected ? b.state : GamepadState{};
        return;
    }
    const GamepadState &x = a.state, &y = b.state;
    GamepadState& o = out->state;
    o.buttons = x.buttons | y.buttons;
    o.lt = x.lt > y.lt ? x.lt : y.lt;
    o.rt = x.rt > y.rt ? x.rt : y.rt;
    // Each stick as a pair, never X from one pad and Y from the other.
    if (mag2(x.lx, x.ly) >= mag2(y.lx, y.ly)) { o.lx = x.lx; o.ly = x.ly; } else { o.lx = y.lx; o.ly = y.ly; }
    if (mag2(x.rx, x.ry) >= mag2(y.rx, y.ry)) { o.rx = x.rx; o.ry = x.ry; } else { o.rx = y.rx; o.ry = y.ry; }
}

bool MergedSource::open(std::string* err) {
    std::string ea, eb;
    aOpen_ = a_ && a_->open(&ea);
    bOpen_ = b_ && b_->open(&eb);
    if (aOpen_ || bOpen_) return true;
    if (err) *err = ea + (ea.empty() || eb.empty() ? "" : "; ") + eb;
    return false;
}

void MergedSource::close() {
    if (aOpen_) a_->close();
    if (bOpen_) b_->close();
    aOpen_ = bOpen_ = false;
}

void MergedSource::read(InputSample* out) {
    InputSample sa, sb;
    if (aOpen_) a_->read(&sa);
    if (bOpen_) b_->read(&sb);
    if (sa.seq != lastA_.seq || sa.connected != lastA_.connected || sb.seq != lastB_.seq || sb.connected != lastB_.connected) seq_++;
    lastA_ = sa;
    lastB_ = sb;
    aConn_.store(sa.connected, std::memory_order_relaxed);
    bConn_.store(sb.connected, std::memory_order_relaxed);
    mergeInputSamples(sa, sb, out);
    out->seq = seq_;
}

void MergedSource::setWaker(std::function<void()> waker) {
    if (a_) a_->setWaker(waker);
    if (b_) b_->setWaker(waker);
}

void MergedSource::setRumble(uint8_t large, uint8_t smallMotor) {
    if (aOpen_) a_->setRumble(large, smallMotor);
    if (bOpen_) b_->setRumble(large, smallMotor);
}

}  // namespace gsm
