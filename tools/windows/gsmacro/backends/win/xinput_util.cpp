#include "xinput_util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <mutex>

namespace gsm {
namespace xi {

namespace {

struct XGamepad { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XState { DWORD dwPacketNumber; XGamepad Gamepad; };
struct XVibration { WORD wLeftMotorSpeed, wRightMotorSpeed; };
struct XCaps { BYTE Type, SubType; WORD Flags; XGamepad Gamepad; XVibration Vibration; };
struct XCapsEx { XCaps Capabilities; WORD VendorId, ProductId, ProductVersion, unk1; DWORD unk2; };

typedef DWORD(WINAPI* PFN_GetState)(DWORD, XState*);
typedef DWORD(WINAPI* PFN_SetState)(DWORD, XVibration*);
typedef DWORD(WINAPI* PFN_GetCapsEx)(DWORD, DWORD, DWORD, XCapsEx*);

std::once_flag g_once;
std::string g_err;
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

void doLoad() {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) { g_err = "xinput1_4.dll not found"; return; }
    g_getState = proc<PFN_GetState>(m, (LPCSTR)(ULONG_PTR)100);
    g_setState = proc<PFN_SetState>(m, "XInputSetState");
    g_getCapsEx = proc<PFN_GetCapsEx>(m, (LPCSTR)(ULONG_PTR)108);
    if (!g_getState || !g_getCapsEx) {
        g_getState = nullptr;
        g_err = "xinput1_4.dll lacks XInputGetStateEx/GetCapabilitiesEx (needed to identify the GameSir pad safely)";
    }
}

}  // namespace

bool load(std::string* err) {
    std::call_once(g_once, doLoad);
    if (!g_getState) { if (err) *err = g_err; return false; }
    return true;
}

bool getState(int slot, uint32_t* packet, GamepadState* out) {
    if (!load(nullptr) || slot < 0 || slot > 3) return false;
    XState st{};
    if (g_getState((DWORD)slot, &st) != ERROR_SUCCESS) return false;
    if (packet) *packet = st.dwPacketNumber;
    out->buttons = st.Gamepad.wButtons;
    out->lt = st.Gamepad.bLeftTrigger;
    out->rt = st.Gamepad.bRightTrigger;
    out->lx = st.Gamepad.sThumbLX;
    out->ly = st.Gamepad.sThumbLY;
    out->rx = st.Gamepad.sThumbRX;
    out->ry = st.Gamepad.sThumbRY;
    return true;
}

bool setVibration(int slot, uint16_t large, uint16_t smallMotor) {
    if (!load(nullptr) || !g_setState || slot < 0 || slot > 3) return false;
    XVibration v{large, smallMotor};
    return g_setState((DWORD)slot, &v) == ERROR_SUCCESS;
}

std::vector<SlotInfo> slots() {
    std::vector<SlotInfo> v;
    if (!load(nullptr)) return v;
    for (int i = 0; i < 4; i++) {
        SlotInfo s;
        s.slot = i;
        XCapsEx c{};
        if (g_getCapsEx(1, (DWORD)i, 0, &c) == ERROR_SUCCESS) {
            s.connected = true;
            s.vid = c.VendorId;
            s.pid = c.ProductId;
        }
        v.push_back(s);
    }
    return v;
}

int findSlot(uint16_t vid, uint16_t pid) {
    for (auto& s : slots())
        if (s.connected && s.vid == vid && (!pid || s.pid == pid)) return s.slot;
    return -1;
}

}  // namespace xi
}  // namespace gsm
