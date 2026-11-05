// Windows host services (see app/platform.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shlobj.h>

#include "../../app/platform.h"
#include "hidhide_status.h"
#include "vigem_sink.h"
#include "win_waiter.h"
#include "xinput_source.h"

namespace gsm {

double processCpuSeconds() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    auto s = [](const FILETIME& f) { return (double)(((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
    return s(k) + s(u);
}

std::unique_ptr<IWaiter> makeEngineWaiter() { return std::make_unique<WinWaiter>(); }

std::string defaultConfigPath() {
    wchar_t* appdata = nullptr;
    std::wstring dir = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) {
        dir = std::wstring(appdata) + L"\\gsmacro";
        CoTaskMemFree(appdata);
        CreateDirectoryW(dir.c_str(), nullptr);  // per-user app folder only
    }
    std::wstring path = dir + L"\\gsmacro.cfg";
    char buf[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, buf, sizeof buf, nullptr, nullptr);
    return buf;
}

bool liveBackendsAvailable() { return true; }

std::string createLiveBackends(LiveBackends* out) {
    out->input = std::make_unique<XInputSource>();
    out->output = std::make_unique<ViGEmSink>();
    return "";
}

std::string hidHideStatusText() { return queryHidHide().detail; }

}  // namespace gsm
