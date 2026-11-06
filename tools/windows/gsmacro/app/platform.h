// Host services the GUI needs. Implemented by app/platform_posix.cpp (demo
// only) and backends/win/win_platform.cpp (adds the live backends and the
// read-only setup checks).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "../core/backend.h"
#include "../core/config.h"

namespace gsm {

double processCpuSeconds();               // user + kernel CPU time of this process
std::unique_ptr<IWaiter> makeEngineWaiter();
std::string defaultConfigPath();

struct LiveBackends {
    std::unique_ptr<IInputSource> input;
    std::unique_ptr<IOutputSink> output;
};
bool liveBackendsAvailable();             // compiled in (Windows build)
// Creates (but does not open) the input source for this controller type
// (GameSir: XInput; DualSense: USB HID) + the ViGEm sink. Returns "" or an error.
std::string createLiveBackends(LiveBackends* out, ControllerType controller = ControllerType::GameSir);

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

// ── background (--autostart) services ──
bool isProcessRunning(const char* exeName);       // e.g. "RocketLeague.exe" (case-insensitive)
int gameSirXInputSlot();                          // XInput slot of the GameSir (VID 3537), or -1
bool anyPadInSlot(int slot);                      // any XInput pad in this slot (as gsmacro sees it)
// GameSir's XInput interface (USB\VID_3537&PID_103D&MI_00) from the PnP tree,
// WITHOUT touching XInput (XInput keeps device handles open, which would veto
// disabling it): -1 not present, 0 present and running, 1 present but disabled/stopped.
int gameSirXusbState();
// Runs the elevated "gsmacro slot0" scheduled task (registered once by
// setup-autostart.ps1). It briefly disables the GameSir's XInput interface so
// a recreated virtual pad can take slot 0, then re-enables it.
bool runSlot0Task(std::string* err);

}  // namespace gsm
