#include "xinput_source.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

namespace gsm {

namespace {

struct XGamepad { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XState { DWORD dwPacketNumber; XGamepad Gamepad; };
struct XVibration { WORD wLeftMotorSpeed, wRightMotorSpeed; };
struct XCaps { BYTE Type, SubType; WORD Flags; XGamepad Gamepad; XVibration Vibration; };
struct XCapsEx { XCaps Capabilities; WORD VendorId, ProductId, ProductVersion, unk1; DWORD unk2; };

typedef DWORD(WINAPI* PFN_GetState)(DWORD, XState*);
typedef DWORD(WINAPI* PFN_SetState)(DWORD, XVibration*);
typedef DWORD(WINAPI* PFN_GetCapsEx)(DWORD, DWORD, DWORD, XCapsEx*);

HMODULE g_xi = nullptr;
PFN_GetState g_getState = nullptr;
PFN_SetState g_setState = nullptr;
PFN_GetCapsEx g_getCapsEx = nullptr;

template <class F>
F proc(HMODULE m, LPCSTR name) {
    FARPROC p = GetProcAddress(m, name);
    F f;
    std::memcpy(&f, &p, sizeof f);
    return f;
}

bool loadXInput(std::string* err) {
    if (g_xi) return true;
    g_xi = LoadLibraryW(L"xinput1_4.dll");
    if (!g_xi) { *err = "xinput1_4.dll not found"; return false; }
    g_getState = proc<PFN_GetState>(g_xi, (LPCSTR)(ULONG_PTR)100);  // XInputGetStateEx
    g_setState = proc<PFN_SetState>(g_xi, "XInputSetState");
    g_getCapsEx = proc<PFN_GetCapsEx>(g_xi, (LPCSTR)(ULONG_PTR)108);  // XInputGetCapabilitiesEx
    if (!g_getState || !g_getCapsEx) {
        *err = "xinput1_4.dll lacks GetStateEx/GetCapabilitiesEx (needed to identify the GameSir pad safely)";
        return false;
    }
    return true;
}

}  // namespace

bool XInputSource::open(std::string* err) {
    if (!loadXInput(err)) return false;
    slot_ = findSlot();
    lastScan_ = clock_.now();
    return true;  // a missing pad is not an error: read() reports disconnected and rescans
}

void XInputSource::close() {
    setRumble(0, 0);
    slot_ = -1;
}

int XInputSource::findSlot() {
    for (DWORD i = 0; i < 4; i++) {
        XCapsEx c{};
        if (g_getCapsEx(1, i, 0, &c) == ERROR_SUCCESS && c.VendorId == vid_) return (int)i;
    }
    return -1;
}

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
    XState st{};
    if (g_getState((DWORD)slot, &st) != ERROR_SUCCESS) {
        out->connected = false;
        slot_ = -1;
        lastScan_ = clock_.now();
        return;
    }
    out->connected = true;
    out->seq = st.dwPacketNumber;
    out->state.buttons = st.Gamepad.wButtons;
    out->state.lt = st.Gamepad.bLeftTrigger;
    out->state.rt = st.Gamepad.bRightTrigger;
    out->state.lx = st.Gamepad.sThumbLX;
    out->state.ly = st.Gamepad.sThumbLY;
    out->state.rx = st.Gamepad.sThumbRX;
    out->state.ry = st.Gamepad.sThumbRY;
}

void XInputSource::setRumble(uint8_t large, uint8_t small) {
    int slot = slot_.load();
    if (slot < 0 || !g_setState) return;
    XVibration v{(WORD)(large * 257), (WORD)(small * 257)};
    g_setState((DWORD)slot, &v);
}

}  // namespace gsm
