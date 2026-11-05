// Host services the GUI needs. Implemented by app/platform_posix.cpp (demo
// only) and backends/win/win_platform.cpp (adds the live backends).
#pragma once
#include <memory>
#include <string>

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
// Read-only, human-readable HidHide state ("not installed", service state...).
std::string hidHideStatusText();

}  // namespace gsm
