// Windows host services (see app/platform.h). Everything here is read-only
// except what the user explicitly starts (Live mode, opening HidHide's client).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <cfgmgr32.h>

#include <cwchar>

#include <cstdio>

#include "../../app/platform.h"
#include "dualsense_source.h"
#include "hidhide_status.h"
#include "vigem_sink.h"
#include "win_waiter.h"
#include "xinput_source.h"
#include "xinput_util.h"

namespace gsm {

namespace {
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.resize((size_t)n - 1);
    return s;
}
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize((size_t)n - 1);
    return w;
}
}  // namespace

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
    return narrow(dir + L"\\gsmacro.cfg");
}

bool liveBackendsAvailable() { return true; }

std::string createLiveBackends(LiveBackends* out, ControllerType controller) {
    if (controller == ControllerType::DualSense) out->input = std::make_unique<DualSenseSource>();
    else out->input = std::make_unique<XInputSource>();
    out->output = std::make_unique<ViGEmSink>();
    return "";
}

std::vector<PadSlot> listXInputPads() {
    std::vector<PadSlot> v;
    for (auto& s : xi::slots())
        if (s.connected) v.push_back(PadSlot{s.slot, s.vid, s.pid});
    return v;
}

CheckResult checkController() {
    CheckResult r;
    std::string err;
    if (!xi::load(&err)) {
        r.state = CheckResult::Fail;
        r.text = err;
        r.guidance = "XInput 1.4 ships with Windows 10/11; this should not happen on a normal install.";
        return r;
    }
    auto pads = listXInputPads();
    char b[200];
    std::string list;
    for (auto& p : pads) {
        snprintf(b, sizeof b, "%sslot %d = %04X:%04X", list.empty() ? "" : ", ", p.slot, p.vid, p.pid);
        list += b;
    }
    for (auto& p : pads) {
        if (p.vid != 0x3537) continue;
        r.state = p.pid == 0x103D ? CheckResult::Ok : CheckResult::Warn;
        snprintf(b, sizeof b, "GameSir found in XInput slot %d (%04X:%04X).", p.slot, p.vid, p.pid);
        r.text = b;
        if (p.pid != 0x103D) r.guidance = "Expected PID 103D (PC/XInput mode). Hold Home + X for 2 s to switch the pad to PC mode.";
        return r;
    }
    r.state = CheckResult::Fail;
    r.text = "No GameSir (VID 3537) XInput controller visible to gsmacro" + (list.empty() ? std::string(".") : " (seen: " + list + ").");
    r.guidance =
        "1) Connect the controller by USB cable.  2) Make sure it is in PC/XInput mode (Home + X for 2 s).  "
        "3) If reWASD has a config applied, unapply it - reWASD can hide the physical pad.  "
        "4) If HidHide already hides the pad, add gsmacro.exe to HidHide's applications list.";
    return r;
}

CheckResult checkViGEm() {
    CheckResult r;
    ViGEmProbe p = probeViGEm();
    r.text = p.text;
    if (p.busConnected) {
        r.state = CheckResult::Ok;
    } else if (!p.dllLoaded) {
        r.state = CheckResult::Fail;
        r.guidance = "ViGEmClient.dll must sit next to gsmacro.exe. Re-extract the gsmacro ZIP (it ships the DLL).";
    } else {
        r.state = CheckResult::Fail;
        r.guidance =
            "The ViGEmBus driver did not answer. It should already be installed on this PC: check 'Nefarius Virtual Gamepad "
            "Emulation Bus' in Device Manager > System devices. gsmacro never installs drivers; reinstalling ViGEmBus "
            "(official Nefarius release) is your decision.";
    }
    return r;
}

CheckResult checkHidHide() {
    CheckResult r;
    HidHideStatus h = queryHidHide();
    r.text = h.detail;
    if (!h.serviceFound) {
        r.state = CheckResult::Warn;
        r.guidance = "Without HidHide the game sees BOTH the physical pad and the virtual pad (doubled input). "
                     "Install HidHide (official Nefarius release) only if you decide to; gsmacro will not install it.";
    } else if (!h.running) {
        r.state = CheckResult::Warn;
        r.guidance = "HidHide is installed but its driver service is not running. A reboot usually fixes this.";
    } else {
        r.state = CheckResult::Ok;
    }
    return r;
}

int virtualPadSlot(LiveBackends* lb) {
    if (!lb || !lb->output) return -1;
    auto* sink = dynamic_cast<ViGEmSink*>(lb->output.get());
    return sink ? sink->userIndex() : -1;
}

bool readXInputSlot(int slot, GamepadState* out) { return xi::getState(slot, nullptr, out); }

bool openHidHideClient(std::string* err) {
    const wchar_t* candidates[] = {
        L"C:\\Program Files\\Nefarius Software Solutions\\HidHide\\x64\\HidHideClient.exe",
        L"C:\\Program Files\\Nefarius Software Solutions e.U\\HidHide\\x64\\HidHideClient.exe",
    };
    for (auto* p : candidates) {
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) continue;
        if ((INT_PTR)ShellExecuteW(nullptr, L"open", p, nullptr, nullptr, SW_SHOWNORMAL) > 32) return true;
    }
    *err = "HidHide Configuration Client not found at its default location. Open it from the Start menu (search \"HidHide\").";
    return false;
}

bool openGameControllersPanel() {
    return (INT_PTR)ShellExecuteW(nullptr, L"open", L"control.exe", L"joy.cpl", nullptr, SW_SHOWNORMAL) > 32;
}

bool pickFile(bool save, std::string* path) {
    wchar_t buf[MAX_PATH * 2] = L"";
    std::wstring init = widen(*path);
    if (init.size() < MAX_PATH * 2) wcscpy(buf, init.c_str());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"gsmacro config (*.cfg)\0*.cfg\0Text (*.txt)\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrDefExt = L"cfg";
    ofn.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return false;
    *path = narrow(buf);
    return true;
}

bool isProcessRunning(const char* exeName) {
    std::wstring want = widen(exeName);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, want.c_str()) == 0;
    CloseHandle(snap);
    return found;
}

int gameSirXInputSlot() {
    std::string err;
    if (!xi::load(&err)) return -1;
    return xi::findSlot(0x3537);
}

bool anyPadInSlot(int slot) {
    std::string err;
    if (!xi::load(&err)) return false;
    for (auto& s : xi::slots())
        if (s.slot == slot && s.connected) return true;
    return false;
}

int gameSirXusbState() {
    const wchar_t* prefix = L"USB\\VID_3537&PID_103D&MI_00\\";
    ULONG len = 0;
    const ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    if (CM_Get_Device_ID_List_SizeW(&len, L"USB", flags) != CR_SUCCESS || len == 0) return -1;
    std::wstring buf(len, L'\0');
    if (CM_Get_Device_ID_ListW(L"USB", &buf[0], len, flags) != CR_SUCCESS) return -1;
    for (const wchar_t* id = buf.c_str(); *id; id += wcslen(id) + 1) {
        if (_wcsnicmp(id, prefix, wcslen(prefix)) != 0) continue;
        DEVINST dn;
        if (CM_Locate_DevNodeW(&dn, const_cast<wchar_t*>(id), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return 1;
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, dn, 0) != CR_SUCCESS) return 1;
        return (status & DN_STARTED) && problem != CM_PROB_DISABLED ? 0 : 1;
    }
    return -1;
}

bool runSlot0Task(std::string* err) {
    wchar_t cmd[] = L"schtasks.exe /run /tn \"gsmacro slot0\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        *err = "could not start schtasks.exe";
        return false;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        *err = "the \"gsmacro slot0\" scheduled task is missing or could not run - run setup-autostart.ps1 once";
        return false;
    }
    return true;
}

}  // namespace gsm
