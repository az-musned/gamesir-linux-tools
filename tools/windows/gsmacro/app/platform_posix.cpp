// Non-Windows host: Demo Mode only (used for development, tests and screenshots).
#include <sys/resource.h>

#include <cstdlib>

#include "../backends/portable_waiter.h"
#include "platform.h"

namespace gsm {

double processCpuSeconds() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return (double)u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + (double)u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}

std::unique_ptr<IWaiter> makeEngineWaiter() { return std::make_unique<PortableWaiter>(); }

std::string defaultConfigPath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.gsmacro.cfg";
}

bool liveBackendsAvailable() { return false; }
std::string createLiveBackends(LiveBackends*, ControllerType) { return "Live mode needs the Windows build (XInput + ViGEmBus)."; }

static CheckResult na(const char* what) {
    CheckResult r;
    r.state = CheckResult::NotApplicable;
    r.text = std::string(what) + ": Windows only - not checked on this host.";
    return r;
}
std::vector<PadSlot> listXInputPads() { return {}; }
CheckResult checkController() { return na("GameSir controller (XInput)"); }
CheckResult checkViGEm() { return na("ViGEmBus / ViGEmClient.dll"); }
CheckResult checkHidHide() { return na("HidHide"); }
int virtualPadSlot(LiveBackends*) { return -1; }
bool readXInputSlot(int, GamepadState*) { return false; }
bool openHidHideClient(std::string* err) { *err = "HidHide is Windows-only."; return false; }
bool pickFile(bool, std::string*) { return false; }
bool openGameControllersPanel() { return false; }
bool isProcessRunning(const char*) { return false; }
int gameSirXInputSlot() { return -1; }
bool anyPadInSlot(int) { return false; }
int gameSirXusbState() { return -1; }
bool runSlot0Task(std::string* err) { *err = "Windows only."; return false; }

std::string bundledConfigPath() { return ""; }
bool startAdminScript(const char*, const std::string&, std::string* err) { *err = "Windows only."; return false; }
int pollAdminScript(std::string*) { return 0; }
bool autostartEntryPresent() { return false; }
bool setAutostartEntry(bool, std::string* err) { *err = "Windows only."; return false; }
bool slot0TaskRegistered() { return false; }

}  // namespace gsm
