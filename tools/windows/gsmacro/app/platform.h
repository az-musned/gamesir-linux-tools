// Host services the GUI needs. Implemented by app/platform_posix.cpp (demo
// only) and backends/win/win_platform.cpp (adds the live backends and the
// read-only setup checks).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "../core/backend.h"

namespace gsm {

double processCpuSeconds();               // user + kernel CPU time of this process
std::unique_ptr<IWaiter> makeEngineWaiter();
std::string defaultConfigPath();

struct LiveBackends {
    std::unique_ptr<IInputSource> input;
    std::unique_ptr<IOutputSink> output;
};
bool liveBackendsAvailable();             // compiled in (Windows build)
// Creates (but does not open) the XInput source + ViGEm sink. Returns "" or an error.
std::string createLiveBackends(LiveBackends* out);

// ── read-only checks for the setup wizard ──
struct CheckResult {
    enum State { Unknown, Ok, Warn, Fail, NotApplicable } state = Unknown;
    std::string text;      // what was found
    std::string guidance;  // what to do about it
};
struct PadSlot { int slot = -1; uint16_t vid = 0, pid = 0; };
std::vector<PadSlot> listXInputPads();            // connected XInput slots
CheckResult checkController();                    // GameSir 3537:103D present in XInput?
CheckResult checkViGEm();                         // ViGEmClient.dll + ViGEmBus reachable (no device created)
CheckResult checkHidHide();                       // HidHide service state (never changes HidHide)
int virtualPadSlot(LiveBackends* lb);             // XInput slot of our virtual pad, or -1
bool readXInputSlot(int slot, GamepadState* out); // for the passthrough self-test
bool openHidHideClient(std::string* err);         // launches HidHide's own configuration app
bool openGameControllersPanel();                  // Windows "Set up USB game controllers" (joy.cpl)
// Native open/save dialog. Returns false if cancelled or unsupported (the
// GUI then uses its path box).
bool pickFile(bool save, std::string* path);

}  // namespace gsm
