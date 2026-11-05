#include "hidhide_status.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>

namespace gsm {

HidHideStatus queryHidHide() {
    HidHideStatus st;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, L"HidHide", SERVICE_QUERY_STATUS);
        if (svc) {
            st.serviceFound = true;
            SERVICE_STATUS ss{};
            if (QueryServiceStatus(svc, &ss)) st.running = ss.dwCurrentState == SERVICE_RUNNING;
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    if (!st.serviceFound) {
        st.detail = "HidHide driver service not found (not installed?).";
        return st;
    }
    st.detail = st.running ? "driver installed and running" : "driver installed, NOT running";

    // List configuration value names (read only) so the user can see something is configured.
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\HidHide\\Parameters", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        std::string names;
        for (DWORD i = 0;; i++) {
            wchar_t name[128];
            DWORD len = 128, type = 0, dataLen = 0;
            if (RegEnumValueW(key, i, name, &len, nullptr, &type, nullptr, &dataLen) != ERROR_SUCCESS) break;
            char nb[256];
            WideCharToMultiByte(CP_UTF8, 0, name, -1, nb, sizeof nb, nullptr, nullptr);
            if (!names.empty()) names += ", ";
            names += nb;
            names += "(" + std::to_string(dataLen) + "B)";
        }
        RegCloseKey(key);
        if (!names.empty()) st.detail += "; config values: " + names;
    }
    st.detail += ". Per-device hiding can't be verified from here - confirm in HidHide Configuration Client.";
    return st;
}

}  // namespace gsm
