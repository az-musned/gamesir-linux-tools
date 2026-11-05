// gsprobe — read-only input timing probe for GameSir controllers on Windows.
//
// What it does (and only this):
//   * enumerates HID collections and XUSB (XInput) interfaces, with each
//     device's driver stack (service, upper/lower filters, class filters)
//   * opens a HID collection with SHARED READ access and timestamps every
//     input report with QueryPerformanceCounter (event-driven, overlapped I/O)
//   * polls XInputGetStateEx on a high-resolution waitable timer and
//     timestamps every packet-number change
//   * in "both" mode, matches button presses across the two streams
//
// What it never does: write output/feature reports, send vendor commands,
// change device configuration, hide devices, or create virtual devices.
// It only calls CreateFile(GENERIC_READ), ReadFile, HidD_Get*/HidP_* queries,
// XInputGetState(Ex)/XInputGetCapabilities(Ex), SetupAPI/CfgMgr queries and
// registry reads.
//
// Build (MSVC, Developer Command Prompt):
//   cl /nologo /O2 /EHsc /std:c++17 gsprobe.cpp setupapi.lib hid.lib cfgmgr32.lib advapi32.lib
// Build (MinGW-w64):
//   x86_64-w64-mingw32-g++ -O2 -std=c++17 -static -o gsprobe.exe gsprobe.cpp -lsetupapi -lhid -lcfgmgr32 -ladvapi32

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <cfgmgr32.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

// XUSB device interface (what xinput1_4.dll opens).
DEFINE_GUID(GUID_XUSB_INTERFACE, 0xEC87F1E3, 0xC13B, 0x4100, 0xB5, 0xF7, 0x8B, 0x84, 0xD5, 0x42, 0x60, 0xCB);

static const USHORT GAMESIR_VID = 0x3537;

// ───────────────────────── time ─────────────────────────

static LARGE_INTEGER g_qpf;
static inline int64_t qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static inline double ticks_to_us(int64_t d) { return (double)d * 1e6 / (double)g_qpf.QuadPart; }

static HANDLE g_stop = nullptr;
static BOOL WINAPI on_ctrl(DWORD) { SetEvent(g_stop); return TRUE; }
static bool stopping() { return WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0; }

// ───────────────────────── strings ─────────────────────────

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

// Parse "vid_XXXX" / "pid_XXXX" / "mi_XX" / "ig_XX" / "col0X" out of a path.
static std::wstring path_token(const std::wstring& path, const wchar_t* key, size_t len) {
    std::wstring l = lower(path);
    size_t p = l.find(key);
    if (p == std::wstring::npos) return {};
    return l.substr(p + wcslen(key), len);
}
static USHORT hex16(const std::wstring& s) { return s.empty() ? 0 : (USHORT)wcstoul(s.c_str(), nullptr, 16); }

static std::string multi_sz(const wchar_t* p, size_t bytes) {
    std::string out;
    size_t n = bytes / sizeof(wchar_t);
    size_t i = 0;
    while (i < n && p[i]) {
        std::wstring item(p + i);
        if (!out.empty()) out += ",";
        out += narrow(item);
        i += item.size() + 1;
    }
    return out.empty() ? "-" : out;
}

// ───────────────────────── driver stack ─────────────────────────

static std::string devnode_prop(DEVINST dn, ULONG prop, bool multi) {
    wchar_t buf[2048]; ULONG len = sizeof(buf); ULONG type = 0;
    if (CM_Get_DevNode_Registry_PropertyW(dn, prop, &type, buf, &len, 0) != CR_SUCCESS) return "-";
    if (multi || type == REG_MULTI_SZ) return multi_sz(buf, len);
    return narrow(buf);
}

static std::string class_filters(const std::string& classGuid, const wchar_t* value) {
    if (classGuid == "-" || classGuid.empty()) return "-";
    std::wstring key = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
    key += std::wstring(classGuid.begin(), classGuid.end());
    wchar_t buf[2048]; DWORD len = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_MULTI_SZ, nullptr, buf, &len) != ERROR_SUCCESS)
        return "-";
    return multi_sz(buf, len);
}

// Print the device node behind an interface path and its ancestors up to the
// USB device: instance id, service, device filters, class filters.
static void print_stack(const std::wstring& instanceId, const char* indent) {
    DEVINST dn;
    if (CM_Locate_DevNodeW(&dn, (DEVINSTID_W)instanceId.c_str(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        printf("%sstack: (devnode not found)\n", indent);
        return;
    }
    for (int depth = 0; depth < 6; depth++) {
        wchar_t id[MAX_DEVICE_ID_LEN];
        if (CM_Get_Device_IDW(dn, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) break;
        std::string cls = devnode_prop(dn, CM_DRP_CLASS, false);
        std::string clsGuid = devnode_prop(dn, CM_DRP_CLASSGUID, false);
        printf("%s%s[%d] %s\n", indent, depth ? "  ^" : "", depth, narrow(id).c_str());
        printf("%s      class=%s service=%s\n", indent, cls.c_str(), devnode_prop(dn, CM_DRP_SERVICE, false).c_str());
        printf("%s      dev upper=%s lower=%s\n", indent,
               devnode_prop(dn, CM_DRP_UPPERFILTERS, true).c_str(),
               devnode_prop(dn, CM_DRP_LOWERFILTERS, true).c_str());
        printf("%s      class upper=%s lower=%s\n", indent,
               class_filters(clsGuid, L"UpperFilters").c_str(),
               class_filters(clsGuid, L"LowerFilters").c_str());
        if (_wcsnicmp(id, L"USB\\VID_", 8) == 0 && wcsstr(id, L"&MI_") == nullptr) break; // reached the USB device
        DEVINST parent;
        if (CM_Get_Parent(&parent, dn, 0) != CR_SUCCESS) break;
        dn = parent;
    }
}

// ───────────────────────── enumeration ─────────────────────────

struct HidInfo {
    std::wstring path, instanceId;
    USHORT vid = 0, pid = 0, version = 0;
    USHORT usagePage = 0, usage = 0;
    USHORT inLen = 0, outLen = 0, featLen = 0;
    USHORT nButtonCaps = 0, nValueCaps = 0;
    std::wstring product, manufacturer;
    std::wstring mi, ig, col;
    DWORD openReadErr = 0;     // 0 = shared GENERIC_READ open succeeded
    DWORD openQueryErr = 0;    // 0 = zero-access open succeeded
    std::vector<UCHAR> reportIds;
};

static std::vector<std::wstring> interface_paths(const GUID& g, std::vector<std::wstring>* instIds) {
    std::vector<std::wstring> out;
    HDEVINFO set = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA ifd{}; ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &g, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<BYTE> buf(need);
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dd{}; dd.cbSize = sizeof(dd);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, &dd)) continue;
        out.push_back(det->DevicePath);
        if (instIds) {
            wchar_t id[MAX_DEVICE_ID_LEN] = L"";
            SetupDiGetDeviceInstanceIdW(set, &dd, id, MAX_DEVICE_ID_LEN, nullptr);
            instIds->push_back(id);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

static HidInfo query_hid(const std::wstring& path, const std::wstring& inst) {
    HidInfo h; h.path = path; h.instanceId = inst;
    h.mi = path_token(path, L"mi_", 2);
    h.ig = path_token(path, L"ig_", 2);
    h.col = path_token(path, L"col", 2);

    // Zero-access open: allowed even for devices opened exclusively by others.
    HANDLE q = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (q == INVALID_HANDLE_VALUE) { h.openQueryErr = GetLastError(); }
    else {
        HIDD_ATTRIBUTES a{}; a.Size = sizeof(a);
        if (HidD_GetAttributes(q, &a)) { h.vid = a.VendorID; h.pid = a.ProductID; h.version = a.VersionNumber; }
        wchar_t s[256];
        if (HidD_GetProductString(q, s, sizeof(s))) h.product = s;
        if (HidD_GetManufacturerString(q, s, sizeof(s))) h.manufacturer = s;
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(q, &pp)) {
            HIDP_CAPS c{};
            if (HidP_GetCaps(pp, &c) == HIDP_STATUS_SUCCESS) {
                h.usagePage = c.UsagePage; h.usage = c.Usage;
                h.inLen = c.InputReportByteLength; h.outLen = c.OutputReportByteLength; h.featLen = c.FeatureReportByteLength;
                h.nButtonCaps = c.NumberInputButtonCaps; h.nValueCaps = c.NumberInputValueCaps;
                std::vector<HIDP_BUTTON_CAPS> bc(c.NumberInputButtonCaps);
                USHORT n = (USHORT)bc.size();
                if (n && HidP_GetButtonCaps(HidP_Input, bc.data(), &n, pp) == HIDP_STATUS_SUCCESS)
                    for (USHORT i = 0; i < n; i++) h.reportIds.push_back(bc[i].ReportID);
                std::vector<HIDP_VALUE_CAPS> vc(c.NumberInputValueCaps);
                n = (USHORT)vc.size();
                if (n && HidP_GetValueCaps(HidP_Input, vc.data(), &n, pp) == HIDP_STATUS_SUCCESS)
                    for (USHORT i = 0; i < n; i++) h.reportIds.push_back(vc[i].ReportID);
                std::sort(h.reportIds.begin(), h.reportIds.end());
                h.reportIds.erase(std::unique(h.reportIds.begin(), h.reportIds.end()), h.reportIds.end());
            }
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(q);
    }
    if (!h.vid) { h.vid = hex16(path_token(path, L"vid_", 4)); h.pid = hex16(path_token(path, L"pid_", 4)); }

    // Shared read open: the access the probe needs to receive input reports.
    HANDLE r = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (r == INVALID_HANDLE_VALUE) h.openReadErr = GetLastError(); else CloseHandle(r);
    return h;
}

static bool is_game_collection(const HidInfo& h) {
    return h.usagePage == 0x01 && (h.usage == 0x04 || h.usage == 0x05 || h.usage == 0x08);
}

static std::vector<HidInfo> enum_hid(bool all) {
    GUID hidGuid; HidD_GetHidGuid(&hidGuid);
    std::vector<std::wstring> ids;
    auto paths = interface_paths(hidGuid, &ids);
    std::vector<HidInfo> out;
    for (size_t i = 0; i < paths.size(); i++) {
        // Cheap filter before opening: GameSir VID, or (with --all) anything.
        USHORT vid = hex16(path_token(paths[i], L"vid_", 4));
        if (!all && vid != GAMESIR_VID) continue;
        HidInfo h = query_hid(paths[i], ids[i]);
        if (all && h.vid != GAMESIR_VID && !is_game_collection(h)) continue;
        out.push_back(std::move(h));
    }
    return out;
}

static const char* hid_role(const HidInfo& h) {
    if (!h.ig.empty()) return "XInput HID companion (IG_)";
    if (h.usagePage >= 0xFF00) return "vendor-defined";
    if (is_game_collection(h)) return "game controller";
    if (h.usagePage == 0x01 && h.usage == 0x06) return "keyboard";
    if (h.usagePage == 0x01 && h.usage == 0x02) return "mouse";
    if (h.usagePage == 0x0C) return "consumer control";
    return "other";
}

// ───────────────────────── XInput (dynamic) ─────────────────────────

struct XGamepad { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XState { DWORD dwPacketNumber; XGamepad Gamepad; };
struct XCaps { BYTE Type, SubType; WORD Flags; XGamepad Gamepad; struct { WORD l, r; } Vibration; };
struct XCapsEx { XCaps Capabilities; WORD VendorId, ProductId, ProductVersion, unk1; DWORD unk2; };

typedef DWORD(WINAPI* PFN_GetState)(DWORD, XState*);
typedef DWORD(WINAPI* PFN_GetCaps)(DWORD, DWORD, XCaps*);
typedef DWORD(WINAPI* PFN_GetCapsEx)(DWORD, DWORD, DWORD, XCapsEx*);

static PFN_GetState  xGetState = nullptr;    // ordinal 100 (Ex, includes Guide) when available
static PFN_GetCaps   xGetCaps = nullptr;
static PFN_GetCapsEx xGetCapsEx = nullptr;   // ordinal 108, reports VID/PID
static bool xHasEx = false;

template <class F> static F proc(HMODULE m, LPCSTR name) {
    FARPROC p = GetProcAddress(m, name);
    F f; static_assert(sizeof(f) == sizeof(p), "pointer size"); memcpy(&f, &p, sizeof(f)); return f;
}

static bool load_xinput() {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) m = LoadLibraryW(L"xinput1_3.dll");
    if (!m) return false;
    xGetState = proc<PFN_GetState>(m, (LPCSTR)(ULONG_PTR)100);
    xHasEx = xGetState != nullptr;
    if (!xGetState) xGetState = proc<PFN_GetState>(m, "XInputGetState");
    xGetCaps = proc<PFN_GetCaps>(m, "XInputGetCapabilities");
    xGetCapsEx = proc<PFN_GetCapsEx>(m, (LPCSTR)(ULONG_PTR)108);
    return xGetState != nullptr;
}

struct XSlot { bool connected = false; USHORT vid = 0, pid = 0, ver = 0; BYTE subType = 0; };
static XSlot query_slot(DWORD i) {
    XSlot s; XState st{};
    s.connected = xGetState && xGetState(i, &st) == ERROR_SUCCESS;
    if (!s.connected) return s;
    XCapsEx ce{};
    if (xGetCapsEx && xGetCapsEx(1, i, 0, &ce) == ERROR_SUCCESS) {
        s.vid = ce.VendorId; s.pid = ce.ProductId; s.ver = ce.ProductVersion; s.subType = ce.Capabilities.SubType;
    } else {
        XCaps c{};
        if (xGetCaps && xGetCaps(i, 0, &c) == ERROR_SUCCESS) s.subType = c.SubType;
    }
    return s;
}

// ───────────────────────── statistics ─────────────────────────

struct Series {
    std::vector<double> v;
    void add(double x) { if (v.size() < 30'000'000) v.push_back(x); }
};

static double pct(const std::vector<double>& s, double p) {
    if (s.empty()) return NAN;
    double idx = p / 100.0 * (s.size() - 1);
    size_t lo = (size_t)idx, hi = std::min(lo + 1, s.size() - 1);
    return s[lo] + (s[hi] - s[lo]) * (idx - lo);
}

static void print_series(const char* name, const Series& in, bool histogram) {
    if (in.v.empty()) { printf("  %-28s n=0\n", name); return; }
    std::vector<double> s = in.v; std::sort(s.begin(), s.end());
    double sum = 0, sq = 0; for (double x : s) { sum += x; sq += x * x; }
    double mean = sum / s.size(), sd = std::sqrt(std::max(0.0, sq / s.size() - mean * mean));
    printf("  %-28s n=%zu mean=%.1f sd=%.1f min=%.1f p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f max=%.1f (us)\n",
           name, s.size(), mean, sd, s.front(), pct(s, 50), pct(s, 90), pct(s, 99), pct(s, 99.9), s.back());
    if (!histogram) return;
    static const double edges[] = { 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000, 1e18 };
    static const char* labels[] = { "<62.5us", "<125us", "<250us", "<500us", "<1ms", "<2ms", "<4ms", "<8ms", "<16ms", ">=16ms" };
    size_t counts[10] = {};
    for (double x : s) for (int b = 0; b < 10; b++) if (x < edges[b]) { counts[b]++; break; }
    printf("    histogram:");
    for (int b = 0; b < 10; b++) if (counts[b]) printf(" %s:%zu(%.1f%%)", labels[b], counts[b], 100.0 * counts[b] / s.size());
    printf("\n");
    double med = pct(s, 50);
    if (med > 0) {
        size_t late = 0, burst = 0;
        for (double x : s) { if (x > 1.5 * med) late++; if (x < 0.25 * med) burst++; }
        printf("    vs median %.1fus: >1.5x (late / possible missed) %zu (%.2f%%), <0.25x (batched) %zu (%.2f%%)\n",
               med, late, 100.0 * late / s.size(), burst, 100.0 * burst / s.size());
    }
}

// ───────────────────────── shared event log ─────────────────────────

enum Src : uint8_t { SRC_HID = 0, SRC_XI = 1 };
struct PressEvent { int64_t t; Src src; uint32_t newBits; };

static std::mutex g_evLock;
static std::vector<PressEvent> g_presses;
static FILE* g_csv = nullptr;
static std::mutex g_csvLock;
static bool g_show = false;
static int64_t g_t0 = 0;

static void csv_line(int64_t t, const char* src, const char* kind, const std::string& data) {
    if (!g_csv) return;
    std::lock_guard<std::mutex> l(g_csvLock);
    fprintf(g_csv, "%.3f,%s,%s,%s\n", ticks_to_us(t - g_t0), src, kind, data.c_str());
}

static std::string hexbytes(const BYTE* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

// ───────────────────────── HID reader ─────────────────────────

struct HidSelector { USHORT vid, pid, usagePage, usage; std::wstring mi, ig, col; };

struct HidStats {
    std::atomic<uint64_t> reports{0}, changed{0}, buttonEdges{0}, axisReports{0}, reconnects{0}, errors{0};
    Series interval, changedInterval, parseCost, completionBacklog;
    std::mutex lock;
};

static bool find_hid(const HidSelector& sel, HidInfo* out) {
    for (auto& h : enum_hid(sel.vid != GAMESIR_VID)) {
        if (h.vid == sel.vid && h.pid == sel.pid && h.usagePage == sel.usagePage && h.usage == sel.usage &&
            h.mi == sel.mi && h.ig == sel.ig && h.col == sel.col) { *out = h; return true; }
    }
    return false;
}

static void hid_thread(HidSelector sel, HidStats* st) {
    const int NREADS = 4;   // outstanding reads, so no report waits on our re-issue
    while (!stopping()) {
        HidInfo info;
        if (!find_hid(sel, &info)) { WaitForSingleObject(g_stop, 500); continue; }
        HANDLE h = CreateFileW(info.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            printf("[hid] open failed (err %lu); retrying\n", GetLastError());
            WaitForSingleObject(g_stop, 1000); continue;
        }
        PHIDP_PREPARSED_DATA pp = nullptr;
        HidD_GetPreparsedData(h, &pp);
        HIDP_CAPS caps{}; if (pp) HidP_GetCaps(pp, &caps);
        const ULONG len = caps.InputReportByteLength ? caps.InputReportByteLength : 64;

        // Collect the value usages we decode (generic desktop axes + hat).
        std::vector<HIDP_VALUE_CAPS> vcaps(caps.NumberInputValueCaps);
        USHORT nv = (USHORT)vcaps.size();
        if (pp && nv) HidP_GetValueCaps(HidP_Input, vcaps.data(), &nv, pp);
        vcaps.resize(nv);

        printf("[hid] reading %s (in=%lu bytes)\n", narrow(info.path).c_str(), len);
        std::vector<std::vector<BYTE>> bufs(NREADS, std::vector<BYTE>(len));
        std::vector<OVERLAPPED> ov(NREADS);
        std::vector<HANDLE> evs(NREADS);
        std::vector<bool> issued(NREADS, false);
        bool ok = true;
        for (int i = 0; i < NREADS; i++) {
            evs[i] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ZeroMemory(&ov[i], sizeof(OVERLAPPED)); ov[i].hEvent = evs[i];
            issued[i] = ReadFile(h, bufs[i].data(), len, nullptr, &ov[i]) || GetLastError() == ERROR_IO_PENDING;
            if (!issued[i]) ok = false;
        }
        std::vector<BYTE> prev;
        uint32_t prevButtons = 0;
        std::vector<LONG> prevVals(vcaps.size(), 0);
        int64_t lastT = 0, lastChangedT = 0;
        int next = 0;
        while (ok && !stopping()) {
            HANDLE w[2] = { evs[next], g_stop };
            DWORD r = WaitForMultipleObjects(2, w, FALSE, INFINITE);
            int64_t t = qpc();   // timestamp first: this is when user mode learns of the report
            if (r != WAIT_OBJECT_0) break;
            DWORD got = 0;
            if (!GetOverlappedResult(h, &ov[next], &got, FALSE)) {
                DWORD e = GetLastError();
                printf("[hid] read error %lu (%s); reconnecting\n", e,
                       e == ERROR_DEVICE_NOT_CONNECTED ? "device removed" : "I/O error");
                issued[next] = false; st->errors++; ok = false; break;
            }
            int64_t p0 = qpc();
            BYTE* rep = bufs[next].data();
            bool changed = prev.size() != got || memcmp(prev.data(), rep, got) != 0;
            uint32_t buttons = 0; bool axisChanged = false;
            if (pp) {
                USAGE usages[64]; ULONG nu = 64;
                if (HidP_GetUsages(HidP_Input, 0x09, 0, usages, &nu, pp, (PCHAR)rep, got) == HIDP_STATUS_SUCCESS)
                    for (ULONG i = 0; i < nu; i++) if (usages[i] >= 1 && usages[i] <= 32) buttons |= 1u << (usages[i] - 1);
                for (size_t i = 0; i < vcaps.size(); i++) {
                    USAGE u = vcaps[i].IsRange ? vcaps[i].Range.UsageMin : vcaps[i].NotRange.Usage;
                    ULONG val = 0;
                    if (HidP_GetUsageValue(HidP_Input, vcaps[i].UsagePage, 0, u, &val, pp, (PCHAR)rep, got) == HIDP_STATUS_SUCCESS) {
                        if ((LONG)val != prevVals[i]) axisChanged = true;
                        prevVals[i] = (LONG)val;
                    }
                }
            }
            int64_t p1 = qpc();
            {
                std::lock_guard<std::mutex> l(st->lock);
                if (lastT) st->interval.add(ticks_to_us(t - lastT));
                if (changed) { if (lastChangedT) st->changedInterval.add(ticks_to_us(t - lastChangedT)); lastChangedT = t; }
                st->parseCost.add(ticks_to_us(p1 - p0));
            }
            lastT = t;
            st->reports++;
            if (changed) st->changed++;
            if (axisChanged) st->axisReports++;
            uint32_t pressed = buttons & ~prevButtons;
            if (buttons != prevButtons) {
                st->buttonEdges++;
                if (g_show) printf("[hid %10.3f ms] buttons %08x -> %08x\n", ticks_to_us(t - g_t0) / 1000.0, prevButtons, buttons);
            }
            if (pressed) { std::lock_guard<std::mutex> l(g_evLock); g_presses.push_back({ t, SRC_HID, pressed }); }
            prevButtons = buttons;
            if (g_csv) csv_line(t, "hid", changed ? "report" : "dup", hexbytes(rep, got));
            prev.assign(rep, rep + got);

            issued[next] = false;
            ResetEvent(evs[next]);
            issued[next] = ReadFile(h, bufs[next].data(), len, nullptr, &ov[next]) || GetLastError() == ERROR_IO_PENDING;
            if (!issued[next]) {
                printf("[hid] re-issue failed (err %lu); reconnecting\n", GetLastError());
                ok = false; break;
            }
            next = (next + 1) % NREADS;
        }
        CancelIoEx(h, nullptr);
        for (int i = 0; i < NREADS; i++) { DWORD d; if (issued[i]) GetOverlappedResult(h, &ov[i], &d, TRUE); CloseHandle(evs[i]); }
        if (pp) HidD_FreePreparsedData(pp);
        CloseHandle(h);
        if (!stopping()) { st->reconnects++; WaitForSingleObject(g_stop, 500); }
    }
}

// ───────────────────────── XInput poller ─────────────────────────

struct XiStats {
    std::atomic<uint64_t> polls{0}, packets{0}, skipped{0}, disconnected{0}, buttonEdges{0};
    Series tick, callCost, packetInterval;
    std::mutex lock;
};

static void xi_thread(DWORD slot, double periodUs, bool spin, XiStats* st) {
    HANDLE timer = spin ? nullptr : CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!spin && !timer) printf("[xi] high-resolution timer unavailable (err %lu); using Sleep(1)\n", GetLastError());
    const int64_t period = (int64_t)(periodUs * g_qpf.QuadPart / 1e6);
    int64_t deadline = qpc() + period, lastTick = 0, lastPktT = 0;
    DWORD lastPkt = 0; bool havePkt = false, wasConnected = true;
    WORD prevButtons = 0;
    while (!stopping()) {
        // Wait for the absolute deadline (no drift accumulation).
        if (spin) { while (qpc() < deadline) YieldProcessor(); }
        else if (timer) {
            int64_t rem = deadline - qpc();
            if (rem > 0) {
                LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(rem * 10'000'000 / g_qpf.QuadPart);
                if (due.QuadPart < 0 && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
                    HANDLE w[2] = { timer, g_stop };
                    if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) break;
                }
            }
        } else Sleep(1);
        int64_t t = qpc();
        deadline += period;
        if (deadline < t) deadline = t + period;   // fell behind: resync, don't sprint

        XState s{};
        int64_t c0 = qpc();
        DWORD r = xGetState(slot, &s);
        int64_t c1 = qpc();
        st->polls++;
        {
            std::lock_guard<std::mutex> l(st->lock);
            if (lastTick) st->tick.add(ticks_to_us(t - lastTick));
            st->callCost.add(ticks_to_us(c1 - c0));
        }
        lastTick = t;
        if (r != ERROR_SUCCESS) {
            if (wasConnected) { printf("[xi] slot %lu not connected (err %lu); waiting\n", slot, r); st->disconnected++; }
            wasConnected = false; havePkt = false;
            WaitForSingleObject(g_stop, 500);
            deadline = qpc() + period;
            continue;
        }
        if (!wasConnected) printf("[xi] slot %lu connected\n", slot);
        wasConnected = true;
        if (!havePkt) { lastPkt = s.dwPacketNumber; havePkt = true; prevButtons = s.Gamepad.wButtons; continue; }
        if (s.dwPacketNumber != lastPkt) {
            DWORD delta = s.dwPacketNumber - lastPkt;
            if (delta > 1) st->skipped += delta - 1;
            st->packets++;
            {
                std::lock_guard<std::mutex> l(st->lock);
                if (lastPktT) st->packetInterval.add(ticks_to_us(c1 - lastPktT));
            }
            lastPktT = c1;
            lastPkt = s.dwPacketNumber;
            WORD pressed = s.Gamepad.wButtons & ~prevButtons;
            if (s.Gamepad.wButtons != prevButtons) {
                st->buttonEdges++;
                if (g_show) printf("[xi  %10.3f ms] buttons %04x -> %04x\n", ticks_to_us(c1 - g_t0) / 1000.0, prevButtons, s.Gamepad.wButtons);
            }
            if (pressed) { std::lock_guard<std::mutex> l(g_evLock); g_presses.push_back({ c1, SRC_XI, pressed }); }
            prevButtons = s.Gamepad.wButtons;
            if (g_csv) {
                char d[160];
                snprintf(d, sizeof(d), "pkt=%lu btn=%04x lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d", s.dwPacketNumber,
                         s.Gamepad.wButtons, s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger,
                         s.Gamepad.sThumbLX, s.Gamepad.sThumbLY, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY);
                csv_line(c1, "xi", "packet", d);
            }
        }
    }
    if (timer) CloseHandle(timer);
}

// ───────────────────────── commands ─────────────────────────

static void cmd_enum(bool all) {
    printf("== HID collections (%s) ==\n", all ? "GameSir + all game controllers" : "VID 3537");
    auto hids = enum_hid(all);
    for (size_t i = 0; i < hids.size(); i++) {
        auto& h = hids[i];
        printf("[%zu] %04x:%04x v%04x  %s  \"%s\" / \"%s\"\n", i, h.vid, h.pid, h.version, hid_role(h),
               narrow(h.product).c_str(), narrow(h.manufacturer).c_str());
        printf("     usage %04x:%04x  in=%u out=%u feat=%u  btnCaps=%u valCaps=%u  reportIds=",
               h.usagePage, h.usage, h.inLen, h.outLen, h.featLen, h.nButtonCaps, h.nValueCaps);
        if (h.reportIds.empty()) printf("-");
        for (auto id : h.reportIds) printf("%02x ", id);
        printf("\n     mi=%s ig=%s col=%s  shared-read open: %s",
               h.mi.empty() ? "-" : narrow(h.mi).c_str(), h.ig.empty() ? "-" : narrow(h.ig).c_str(),
               h.col.empty() ? "-" : narrow(h.col).c_str(), h.openReadErr ? "FAILED" : "ok");
        if (h.openReadErr) printf(" (err %lu%s)", h.openReadErr, h.openReadErr == ERROR_ACCESS_DENIED ? " access denied" :
                                                   h.openReadErr == ERROR_FILE_NOT_FOUND ? " not found / hidden" : "");
        if (h.openQueryErr) printf("  query open err %lu", h.openQueryErr);
        printf("\n     path %s\n", narrow(h.path).c_str());
        print_stack(h.instanceId, "     ");
    }
    if (hids.empty()) printf("(none — unplugged, or hidden from this process by HidHide/reWASD)\n");

    printf("\n== XUSB interfaces (devices xinput1_4 can open) ==\n");
    std::vector<std::wstring> ids;
    auto xp = interface_paths(GUID_XUSB_INTERFACE, &ids);
    for (size_t i = 0; i < xp.size(); i++) {
        printf("[x%zu] vid=%04x pid=%04x  %s\n", i, hex16(path_token(xp[i], L"vid_", 4)), hex16(path_token(xp[i], L"pid_", 4)),
               narrow(xp[i]).c_str());
        print_stack(ids[i], "     ");
    }
    if (xp.empty()) printf("(none)\n");

    printf("\n== XInput slots (as this process sees them; %s) ==\n", xHasEx ? "GetStateEx" : "GetState");
    for (DWORD i = 0; i < 4; i++) {
        XSlot s = query_slot(i);
        if (!s.connected) { printf("slot %lu: -\n", i); continue; }
        printf("slot %lu: connected  vid=%04x pid=%04x ver=%04x subtype=%u%s\n", i, s.vid, s.pid, s.ver, s.subType,
               s.vid == GAMESIR_VID ? "  <- GameSir" : (s.vid == 0x045e && s.pid == 0x028e) ? "  <- Xbox 360 identity (real or virtual)" : "");
    }
}

static void print_hid_stats(HidStats& st, double secs) {
    std::lock_guard<std::mutex> l(st.lock);
    printf("HID: reports=%llu (%.1f/s)  content-changed=%llu (%.1f/s)  identical-to-previous=%llu  axis-change reports=%llu  button edges=%llu  reconnects=%llu errors=%llu\n",
           (unsigned long long)st.reports.load(), st.reports / secs, (unsigned long long)st.changed.load(), st.changed / secs,
           (unsigned long long)(st.reports - st.changed), (unsigned long long)st.axisReports.load(),
           (unsigned long long)st.buttonEdges.load(), (unsigned long long)st.reconnects.load(), (unsigned long long)st.errors.load());
    print_series("report arrival interval", st.interval, true);
    print_series("changed-report interval", st.changedInterval, true);
    print_series("parse cost (HidP decode)", st.parseCost, false);
}

static void print_xi_stats(XiStats& st, double secs, double periodUs) {
    std::lock_guard<std::mutex> l(st.lock);
    printf("XInput: polls=%llu (%.1f/s, target %.1f/s)  packet changes=%llu (%.1f/s)  packet numbers skipped between polls=%llu  disconnects=%llu  button edges=%llu\n",
           (unsigned long long)st.polls.load(), st.polls / secs, 1e6 / periodUs, (unsigned long long)st.packets.load(), st.packets / secs,
           (unsigned long long)st.skipped.load(), (unsigned long long)st.disconnected.load(), (unsigned long long)st.buttonEdges.load());
    print_series("poll tick interval", st.tick, true);
    print_series("XInputGetState call cost", st.callCost, false);
    print_series("packet-change interval", st.packetInterval, true);
}

// For each XInput press, the nearest HID press within ±100 ms sharing a press.
static void print_correlation() {
    std::lock_guard<std::mutex> l(g_evLock);
    std::vector<PressEvent> hid, xi;
    for (auto& e : g_presses) (e.src == SRC_HID ? hid : xi).push_back(e);
    Series d;
    const int64_t win = g_qpf.QuadPart / 10;
    size_t unmatched = 0;
    for (auto& x : xi) {
        int64_t best = INT64_MAX; bool found = false;
        for (auto& h : hid) {
            int64_t dt = x.t - h.t;
            if (dt > win || dt < -win) continue;
            if (!found || std::llabs(dt) < std::llabs(best)) { best = dt; found = true; }
        }
        if (found) d.add(ticks_to_us(best)); else unmatched++;
    }
    printf("Press correlation: HID presses=%zu XInput presses=%zu matched=%zu unmatched=%zu\n",
           hid.size(), xi.size(), d.v.size(), unmatched);
    printf("  (delta = XInput observation time - HID arrival time; positive = HID seen first.\n"
           "   Includes up to one XInput poll period of sampling delay.)\n");
    print_series("XInput minus HID", d, false);
}

static double cpu_percent(int64_t wallTicks) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return NAN;
    auto t = [](FILETIME f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
    return 100.0 * (t(k) + t(u)) / (ticks_to_us(wallTicks) / 1e6);
}

static void usage() {
    printf(
        "gsprobe — read-only GameSir input timing probe\n\n"
        "  gsprobe enum [--all]               list HID/XUSB interfaces, driver stacks, XInput slots\n"
        "  gsprobe hid    [opts]              event-driven HID report timing\n"
        "  gsprobe xinput [opts]              XInput polling timing\n"
        "  gsprobe both   [opts]              both at once + press correlation\n\n"
        "options:\n"
        "  --dev N          HID collection index from 'enum' (default: GameSir IG_ collection, else first GameSir game collection)\n"
        "  --all            with enum/--dev: include non-GameSir game controllers (e.g. a virtual pad)\n"
        "  --slot N         XInput slot (default: slot reporting VID 3537, else first connected)\n"
        "  --period-us U    XInput poll period in microseconds (default 1000)\n"
        "  --spin           busy-wait XInput polling instead of the high-resolution timer (comparison only; burns a core)\n"
        "  -t SECONDS       run time (default 30; Ctrl+C stops early)\n"
        "  --csv FILE       write every report/packet with its timestamp\n"
        "  --show           print button transitions live\n"
        "  --high           raise thread priority to TIME_CRITICAL\n");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // live lines must reach Tee-Object / a pipe immediately
    QueryPerformanceFrequency(&g_qpf);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    load_xinput();
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    int dev = -1, slot = -1; double periodUs = 1000, secs = 30; bool spin = false, high = false, all = false;
    const char* csv = nullptr;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { if (i + 1 >= argc) { usage(); exit(1); } return argv[++i]; };
        if (a == "--dev") dev = atoi(next());
        else if (a == "--slot") slot = atoi(next());
        else if (a == "--period-us") periodUs = std::max(50.0, atof(next()));
        else if (a == "-t") secs = atof(next());
        else if (a == "--csv") csv = next();
        else if (a == "--spin") spin = true;
        else if (a == "--show") g_show = true;
        else if (a == "--high") high = true;
        else if (a == "--all") all = true;
        else { usage(); return 1; }
    }
    printf("QPC frequency: %lld Hz (resolution %.3f us)\n", (long long)g_qpf.QuadPart, 1e6 / g_qpf.QuadPart);
    printf("XInput: %s\n", xGetState ? (xHasEx ? "xinput loaded (GetStateEx)" : "xinput loaded (GetState)") : "NOT available");
    if (cmd == "enum") { cmd_enum(all); return 0; }

    bool wantHid = cmd == "hid" || cmd == "both", wantXi = cmd == "xinput" || cmd == "both";
    if (!wantHid && !wantXi) { usage(); return 1; }

    HidSelector sel{};
    if (wantHid) {
        auto hids = enum_hid(all);
        int pick = dev;
        if (pick < 0) {
            for (size_t i = 0; i < hids.size() && pick < 0; i++) if (hids[i].vid == GAMESIR_VID && !hids[i].ig.empty()) pick = (int)i;
            for (size_t i = 0; i < hids.size() && pick < 0; i++) if (hids[i].vid == GAMESIR_VID && is_game_collection(hids[i])) pick = (int)i;
        }
        if (pick < 0 || pick >= (int)hids.size()) { printf("No HID collection selected/found. Run 'gsprobe enum'.\n"); return 2; }
        auto& h = hids[pick];
        if (h.openReadErr) printf("warning: shared-read open of [%d] failed earlier (err %lu)\n", pick, h.openReadErr);
        sel = { h.vid, h.pid, h.usagePage, h.usage, h.mi, h.ig, h.col };
        printf("HID: [%d] %04x:%04x usage %04x:%04x %s\n", pick, h.vid, h.pid, h.usagePage, h.usage, hid_role(h));
    }
    if (wantXi) {
        if (!xGetState) { printf("XInput not available.\n"); return 2; }
        if (slot < 0) {
            for (DWORD i = 0; i < 4 && slot < 0; i++) { XSlot s = query_slot(i); if (s.connected && s.vid == GAMESIR_VID) slot = (int)i; }
            for (DWORD i = 0; i < 4 && slot < 0; i++) if (query_slot(i).connected) slot = (int)i;
        }
        if (slot < 0) { printf("No connected XInput slot.\n"); return 2; }
        XSlot s = query_slot(slot);
        printf("XInput: slot %d vid=%04x pid=%04x, period %.1f us, %s\n", slot, s.vid, s.pid, periodUs,
               spin ? "busy-wait" : "high-resolution waitable timer");
    }
    if (csv) { g_csv = fopen(csv, "w"); if (g_csv) fprintf(g_csv, "t_us,source,kind,data\n"); }

    HidStats hs; XiStats xs;
    g_t0 = qpc();
    std::thread th, tx;
    if (wantHid) th = std::thread([&] { if (high) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); hid_thread(sel, &hs); });
    if (wantXi) tx = std::thread([&] { if (high) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); xi_thread((DWORD)slot, periodUs, spin, &xs); });

    printf("Running %.0f s. Move the sticks continuously and press buttons one at a time. Ctrl+C to stop.\n", secs);
    uint64_t lastHr = 0, lastHc = 0, lastXp = 0, lastXk = 0;
    int64_t end = g_t0 + (int64_t)(secs * g_qpf.QuadPart);
    while (qpc() < end && WaitForSingleObject(g_stop, 1000) == WAIT_TIMEOUT) {
        printf("  [%5.0fs]", ticks_to_us(qpc() - g_t0) / 1e6);
        if (wantHid) { printf(" hid %6llu rpt/s (%6llu changed)", (unsigned long long)(hs.reports - lastHr), (unsigned long long)(hs.changed - lastHc)); lastHr = hs.reports; lastHc = hs.changed; }
        if (wantXi) { printf(" | xi %6llu polls/s %6llu pkts/s", (unsigned long long)(xs.polls - lastXp), (unsigned long long)(xs.packets - lastXk)); lastXp = xs.polls; lastXk = xs.packets; }
        printf("\n");
    }
    SetEvent(g_stop);
    if (th.joinable()) th.join();
    if (tx.joinable()) tx.join();
    int64_t wall = qpc() - g_t0;
    double el = ticks_to_us(wall) / 1e6;

    printf("\n===== results (%.1f s) =====\n", el);
    if (wantHid) print_hid_stats(hs, el);
    if (wantXi) print_xi_stats(xs, el, periodUs);
    if (wantHid && wantXi) print_correlation();
    printf("Process CPU: %.2f%% of one core\n", cpu_percent(wall));
    if (g_csv) fclose(g_csv);
    return 0;
}
