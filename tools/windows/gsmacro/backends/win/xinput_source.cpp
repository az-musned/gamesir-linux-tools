#include "xinput_source.h"

#include "xinput_util.h"

namespace gsm {

bool XInputSource::open(std::string* err) {
    if (!xi::load(err)) return false;
    slot_ = findSlot();
    lastScan_ = clock_.now();
    return true;  // a missing pad is not an error: read() reports disconnected and rescans
}

void XInputSource::close() {
    setRumble(0, 0);
    slot_ = -1;
}

int XInputSource::findSlot() { return xi::findSlot(vid_); }

void XInputSource::read(InputSample* out) {
    int slot = slot_.load(std::memory_order_relaxed);
    if (slot < 0) {
        out->connected = false;
        Time now = clock_.now();
        if (now - lastScan_ >= ms(1000)) {  // querying empty slots is slow: rescan at most 1/s
            lastScan_ = now;
            slot_ = findSlot();
        }
        return;
    }
    uint32_t packet = 0;
    if (!xi::getState(slot, &packet, &out->state)) {
        out->connected = false;
        out->state = GamepadState();
        slot_ = -1;
        lastScan_ = clock_.now();
        return;
    }
    out->connected = true;
    out->seq = packet;
}

void XInputSource::setRumble(uint8_t large, uint8_t small) {
    int slot = slot_.load();
    if (slot >= 0) xi::setVibration(slot, (uint16_t)(large * 257), (uint16_t)(small * 257));
}

}  // namespace gsm
