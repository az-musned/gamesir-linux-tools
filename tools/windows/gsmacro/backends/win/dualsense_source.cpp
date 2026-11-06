#include "dualsense_source.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <hidsdi.h>
#include <setupapi.h>

#include <string>

#include "../../core/dualsense.h"

namespace gsm {

namespace {

// First present USB DualSense / Edge game-controller collection, opened shared
// and read-only (overlapped). INVALID_HANDLE_VALUE if none.
HANDLE openDualSense() {
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA d{};
    d.cbSize = sizeof d;
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &g, i, &d); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &d, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::string mem(need, '\0');
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(&mem[0]);
        det->cbSize = sizeof *det;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &d, det, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof a;
        bool ok = HidD_GetAttributes(h, &a) && a.VendorID == kSonyVid &&
                  (a.ProductID == kDualSensePid || a.ProductID == kDualSenseEdgePid);
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (ok && HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps{};
            HidP_GetCaps(pp, &caps);
            HidD_FreePreparsedData(pp);
            // Game-controller collection with the 64-byte USB report (Bluetooth differs).
            ok = caps.UsagePage == 0x01 && caps.Usage == 0x05 && caps.InputReportByteLength == kDualSenseUsbReportSize;
        } else {
            ok = false;
        }
        if (ok) found = h;
        else CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

}  // namespace

bool DualSenseSource::open(std::string* /*err*/) {
    close();
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        std::lock_guard<std::mutex> l(m_);
        latest_ = InputSample();
    }
    thread_ = std::thread([this] { run(); });
    return true;  // a missing pad is not an error: read() reports disconnected until it appears
}

void DualSenseSource::close() {
    if (thread_.joinable()) {
        SetEvent((HANDLE)stop_);
        thread_.join();
    }
    if (stop_) {
        CloseHandle((HANDLE)stop_);
        stop_ = nullptr;
    }
    std::lock_guard<std::mutex> l(m_);
    latest_.connected = false;
}

void DualSenseSource::setWaker(std::function<void()> waker) {
    std::lock_guard<std::mutex> l(m_);
    waker_ = std::move(waker);
}

void DualSenseSource::read(InputSample* out) {
    std::lock_guard<std::mutex> l(m_);
    *out = latest_;
}

void DualSenseSource::publish(const GamepadState* s) {
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> l(m_);
        bool changed = s ? (!latest_.connected || !(latest_.state == *s)) : latest_.connected;
        if (s) {
            latest_.state = *s;
            latest_.connected = true;
        } else {
            latest_.state = GamepadState();
            latest_.connected = false;
        }
        if (!changed) return;
        latest_.seq++;
        wake = waker_;
    }
    if (wake) wake();
}

void DualSenseSource::run() {
    HANDLE stop = (HANDLE)stop_;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    uint8_t buf[kDualSenseUsbReportSize];
    while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        HANDLE h = openDualSense();
        if (h == INVALID_HANDLE_VALUE) {
            publish(nullptr);
            if (WaitForSingleObject(stop, 1000) != WAIT_TIMEOUT) break;  // rescan once a second
            continue;
        }
        for (;;) {
            OVERLAPPED ov{};
            ov.hEvent = ev;
            ResetEvent(ev);
            DWORD n = 0;
            if (!ReadFile(h, buf, sizeof buf, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
            HANDLE w[2] = {ev, stop};
            DWORD r = WaitForMultipleObjects(2, w, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0) {  // stopping
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &n, TRUE);
                CloseHandle(h);
                publish(nullptr);
                CloseHandle(ev);
                return;
            }
            if (!GetOverlappedResult(h, &ov, &n, FALSE)) break;  // unplugged / device error
            GamepadState s;
            if (parseDualSenseUsb(buf, n, &s)) publish(&s);
        }
        CloseHandle(h);
        publish(nullptr);  // disconnected: the engine cancels macros and outputs neutral
        if (WaitForSingleObject(stop, 200) != WAIT_TIMEOUT) break;
    }
    CloseHandle(ev);
}

}  // namespace gsm
