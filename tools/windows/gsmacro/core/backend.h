// Backend interfaces. The engine only talks to these, so the same pipeline
// runs against the mock (demo/tests) or the Windows XInput/ViGEm backends.
#pragma once
#include <functional>
#include <string>

#include "clock.h"
#include "gamepad.h"

namespace gsm {

struct InputSample {
    GamepadState state;
    bool connected = false;
    uint64_t seq = 0;  // changes whenever the source has new data
};

class IInputSource {
public:
    virtual ~IInputSource() = default;
    virtual const char* name() const = 0;
    virtual bool open(std::string* /*err*/) { return true; }
    virtual void close() {}
    // Non-blocking: latest state. Called only from the engine thread.
    virtual void read(InputSample* out) = 0;
    // Event-driven sources call the waker when new data arrives, so the
    // engine wakes immediately instead of polling.
    virtual bool eventDriven() const { return false; }
    virtual void setWaker(std::function<void()> /*waker*/) {}
    // Rumble from the game (via the virtual pad) back to the physical pad.
    virtual void setRumble(uint8_t /*large*/, uint8_t /*small*/) {}
};

class IOutputSink {
public:
    virtual ~IOutputSink() = default;
    virtual const char* name() const = 0;
    virtual bool open(std::string* /*err*/) { return true; }
    virtual void close() {}
    // Called only when the output changes. Returns false on failure.
    virtual bool submit(const GamepadState& s) = 0;
    virtual void setRumbleHandler(std::function<void(uint8_t, uint8_t)> /*h*/) {}
};

// Sleeps until an absolute deadline (on SteadyClock's timeline), or until
// wake() is called. The last `spinTail` ns before the deadline are spun for
// precision. Returns true if woken early by wake().
class IWaiter {
public:
    virtual ~IWaiter() = default;
    virtual bool waitUntil(Time deadline, Time spinTail) = 0;
    virtual void wake() = 0;
    // Called once on the engine thread (priority / MMCSS hooks).
    virtual void onThreadStart() {}
};

}  // namespace gsm
