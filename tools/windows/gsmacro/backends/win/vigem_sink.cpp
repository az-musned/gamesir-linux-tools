#include "vigem_sink.h"

#include "xinput_util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace gsm {

namespace {

// Mirrors ViGEmClient.h (MIT, nefarius/ViGEmClient). Verify against the
// header of the DLL you ship if ViGEmClient changes.
typedef DWORD VIGEM_ERROR;
const VIGEM_ERROR VIGEM_ERROR_NONE = 0x20000000;
const VIGEM_ERROR VIGEM_ERROR_BUS_NOT_FOUND = 0xE0000001;
const VIGEM_ERROR VIGEM_ERROR_BUS_VERSION_MISMATCH = 0xE0000008;
const VIGEM_ERROR VIGEM_ERROR_BUS_ACCESS_FAILED = 0xE0000003;

struct XUSB_REPORT {
    USHORT wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};

typedef VOID(CALLBACK* PFN_X360_NOTIFICATION)(void* client, void* target, UCHAR large, UCHAR smallMotor, UCHAR led, LPVOID user);
typedef void*(__cdecl* PFN_alloc)();
typedef VIGEM_ERROR(__cdecl* PFN_connect)(void*);
typedef void(__cdecl* PFN_disconnect)(void*);
typedef void(__cdecl* PFN_free)(void*);
typedef void*(__cdecl* PFN_target_x360_alloc)();
typedef VIGEM_ERROR(__cdecl* PFN_target_add)(void*, void*);
typedef VIGEM_ERROR(__cdecl* PFN_target_remove)(void*, void*);
typedef void(__cdecl* PFN_target_free)(void*);
typedef VIGEM_ERROR(__cdecl* PFN_target_x360_update)(void*, void*, XUSB_REPORT);
typedef VIGEM_ERROR(__cdecl* PFN_target_x360_register_notification)(void*, void*, PFN_X360_NOTIFICATION, LPVOID);
typedef void(__cdecl* PFN_target_x360_unregister_notification)(void*);
typedef VIGEM_ERROR(__cdecl* PFN_target_x360_get_user_index)(void*, void*, PULONG);

struct Api {
    HMODULE dll = nullptr;
    PFN_alloc alloc = nullptr;
    PFN_connect connect = nullptr;
    PFN_disconnect disconnect = nullptr;
    PFN_free free_ = nullptr;
    PFN_target_x360_alloc x360Alloc = nullptr;
    PFN_target_add add = nullptr;
    PFN_target_remove remove = nullptr;
    PFN_target_free targetFree = nullptr;
    PFN_target_x360_update update = nullptr;
    PFN_target_x360_register_notification regNotif = nullptr;
    PFN_target_x360_unregister_notification unregNotif = nullptr;
    PFN_target_x360_get_user_index userIndex = nullptr;
} g;

template <class F>
bool load(F* out, const char* name) {
    FARPROC p = GetProcAddress(g.dll, name);
    std::memcpy(out, &p, sizeof *out);
    return p != nullptr;
}

bool loadApi(std::string* err) {
    if (g.dll) return true;
    g.dll = LoadLibraryW(L"ViGEmClient.dll");
    if (!g.dll) { *err = "ViGEmClient.dll not found next to gsmacro.exe (see README: build it from nefarius/ViGEmClient)"; return false; }
    bool ok = load(&g.alloc, "vigem_alloc") && load(&g.connect, "vigem_connect") && load(&g.disconnect, "vigem_disconnect") &&
              load(&g.free_, "vigem_free") && load(&g.x360Alloc, "vigem_target_x360_alloc") && load(&g.add, "vigem_target_add") &&
              load(&g.remove, "vigem_target_remove") && load(&g.targetFree, "vigem_target_free") &&
              load(&g.update, "vigem_target_x360_update");
    load(&g.regNotif, "vigem_target_x360_register_notification");      // optional (rumble)
    load(&g.unregNotif, "vigem_target_x360_unregister_notification");
    load(&g.userIndex, "vigem_target_x360_get_user_index");
    if (!ok) {
        *err = "ViGEmClient.dll is missing expected exports";
        FreeLibrary(g.dll);
        g = Api();
        return false;
    }
    return true;
}

VOID CALLBACK onNotification(void*, void*, UCHAR large, UCHAR smallMotor, UCHAR, LPVOID user) {
    static_cast<ViGEmSink*>(user)->onRumble(large, smallMotor);
}

std::string errText(const char* what, VIGEM_ERROR e) {
    char b[160];
    const char* hint = e == VIGEM_ERROR_BUS_NOT_FOUND ? " (ViGEmBus driver not installed or not running)"
                       : e == VIGEM_ERROR_BUS_VERSION_MISMATCH ? " (ViGEmBus / ViGEmClient version mismatch - update ViGEmBus)"
                       : e == VIGEM_ERROR_BUS_ACCESS_FAILED ? " (could not open the ViGEmBus device)" : "";
    std::snprintf(b, sizeof b, "%s failed: 0x%08lX%s", what, (unsigned long)e, hint);
    return b;
}

// One bus connection per process: opened on first use, never disconnected.
// vigem_disconnect() (v1.21.222.0) can deadlock: it calls CancelIoEx once on
// the DS4 output pickup thread's I/O and then waits INFINITE for that thread.
// If the thread has not issued its DeviceIoControl yet (a disconnect soon
// after vigem_connect), nothing is cancelled and both block forever. Seen on
// the target PC: the GUI thread hung in vigem_disconnect while the pickup
// thread sat in GetOverlappedResult. Windows closes the handle (and cancels
// the pending I/O) at process exit; virtual pads are still removed with
// vigem_target_remove.
std::mutex g_busLock;
void* g_bus = nullptr;

void* sharedBus(VIGEM_ERROR* e) {
    std::lock_guard<std::mutex> l(g_busLock);
    *e = VIGEM_ERROR_NONE;
    if (g_bus) return g_bus;
    void* c = g.alloc();
    if (!c) return nullptr;
    *e = g.connect(c);
    if (*e != VIGEM_ERROR_NONE) { g.free_(c); return nullptr; }  // no pickup thread exists on failure
    g_bus = c;
    return c;
}

}  // namespace

bool ViGEmSink::open(std::string* err) {
    close();
    userIndex_ = -1;
    if (!loadApi(err)) return false;
    VIGEM_ERROR e = VIGEM_ERROR_NONE;
    client_ = sharedBus(&e);
    if (!client_) { *err = e == VIGEM_ERROR_NONE ? "vigem_alloc failed" : errText("vigem_connect", e); return false; }
    // Remember which XInput slots already hold an Xbox 360 identity (another
    // remapper, a real 360 pad) so userIndex() can tell ours apart.
    preexistingX360_ = 0;
    std::string xerr;
    if (xi::load(&xerr))
        for (auto& s : xi::slots())
            if (s.connected && s.vid == 0x045E && s.pid == 0x028E) preexistingX360_ |= 1u << s.slot;
    target_ = g.x360Alloc();
    e = g.add(client_, target_);
    if (e != VIGEM_ERROR_NONE) {
        *err = errText("vigem_target_add", e);
        g.targetFree(target_);
        target_ = nullptr;
        client_ = nullptr;  // shared connection stays open
        return false;
    }
    if (g.regNotif) notifRegistered_ = g.regNotif(client_, target_, onNotification, this) == VIGEM_ERROR_NONE;
    return true;
}

void ViGEmSink::close() {
    if (target_) {
        if (notifRegistered_ && g.unregNotif) g.unregNotif(target_);
        notifRegistered_ = false;
        XUSB_REPORT neutral{};
        g.update(client_, target_, neutral);  // leave nothing held
        g.remove(client_, target_);
        g.targetFree(target_);
        target_ = nullptr;
    }
    client_ = nullptr;  // shared bus connection: never vigem_disconnect (see sharedBus)
}

bool ViGEmSink::submit(const GamepadState& s) {
    if (!target_) return false;
    XUSB_REPORT r;
    r.wButtons = s.buttons;
    r.bLeftTrigger = s.lt;
    r.bRightTrigger = s.rt;
    r.sThumbLX = s.lx;
    r.sThumbLY = s.ly;
    r.sThumbRX = s.rx;
    r.sThumbRY = s.ry;
    return g.update(client_, target_, r) == VIGEM_ERROR_NONE;
}

// The XInput slot is found by looking for the X360 slot that appeared when our
// pad was plugged in. vigem_target_x360_get_user_index is NOT used: on the
// target PC (ViGEmBus 1.21.442) it returned success with index 0 for the whole
// session while XInput had the virtual pad in slot 1 and the GameSir in slot 0,
// so the self-test and readback were reading the physical pad.
int ViGEmSink::userIndex() {
    if (userIndex_ >= 0 || !target_) return userIndex_;
    int found = -1, count = 0;
    for (auto& s : xi::slots())
        if (s.connected && s.vid == 0x045E && s.pid == 0x028E && !(preexistingX360_ & (1u << s.slot))) { found = s.slot; count++; }
    if (count == 1) userIndex_ = found;  // ambiguous or not there yet: unknown, never a guess
    return userIndex_;
}

ViGEmProbe probeViGEm() {
    ViGEmProbe p;
    std::string err;
    if (!loadApi(&err)) { p.text = err; return p; }
    p.dllLoaded = true;
    VIGEM_ERROR e = VIGEM_ERROR_NONE;
    if (sharedBus(&e)) {
        p.busConnected = true;
        p.text = "ViGEmClient.dll loaded; ViGEmBus driver reachable";
    } else if (e == VIGEM_ERROR_NONE) {
        p.text = "ViGEmClient.dll loaded, but vigem_alloc failed";
    } else {
        p.text = "ViGEmClient.dll loaded; " + errText("connecting to ViGEmBus", e);
    }
    return p;
}

void ViGEmSink::setRumbleHandler(std::function<void(uint8_t, uint8_t)> h) {
    std::lock_guard<std::mutex> l(rumbleLock_);
    rumble_ = std::move(h);
}

void ViGEmSink::onRumble(uint8_t large, uint8_t smallMotor) {
    std::lock_guard<std::mutex> l(rumbleLock_);
    if (rumble_) rumble_(large, smallMotor);
}

}  // namespace gsm
