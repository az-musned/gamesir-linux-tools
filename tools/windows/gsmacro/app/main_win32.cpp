// Windows host: Win32 window + Direct3D 11 (based on Dear ImGui's
// example_win32_directx11). Presents with vsync, and sleeps while minimised,
// so the GUI never competes with the engine thread or the game for a core.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <cwchar>

#include "gui.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resizeW = 0, g_resizeH = 0;
bool g_occluded = false;

// Tray icon (background mode, --autostart).
const UINT WM_TRAY = WM_APP + 1;   // notification-area callbacks
const UINT WM_SHOWMAIN = WM_APP + 2;  // a second gsmacro.exe asks the running one to show itself
const UINT ID_OPEN = 1, ID_EXIT = 2;
bool g_tray = false;
UINT g_taskbarCreated = 0;
NOTIFYICONDATAW g_nid{};

void addTrayIcon(HWND hwnd) {
    g_nid = NOTIFYICONDATAW{};
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"gsmacro (background) - double-click to open");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void showMain(HWND hwnd) {
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd);
}

void createRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void cleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                               &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU driver: WARP software renderer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap,
                                           &g_device, &got, &g_context);
    if (FAILED(hr)) return false;
    createRenderTarget();
    return true;
}

void cleanupDevice() {
    cleanupRenderTarget();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_resizeW = (UINT)LOWORD(lParam);
        g_resizeH = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
        if (g_tray) { ShowWindow(hWnd, SW_HIDE); return 0; }  // background mode: X hides, tray > Exit quits
        break;
    case WM_TRAY:
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK) showMain(hWnd);
        else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, ID_OPEN, L"Open gsmacro");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit (removes the virtual pad)");
            POINT p;
            GetCursorPos(&p);
            SetForegroundWindow(hWnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, hWnd, nullptr);
            DestroyMenu(m);
        }
        return 0;
    case WM_SHOWMAIN:
        showMain(hWnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_OPEN) { showMain(hWnd); return 0; }
        if (LOWORD(wParam) == ID_EXIT) { DestroyWindow(hWnd); return 0; }
        break;
    case WM_DESTROY:
        if (g_tray) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_tray = false; }
        PostQuitMessage(0);
        return 0;
    }
    if (g_taskbarCreated && msg == g_taskbarCreated && g_tray) {  // Explorer restarted: re-add the icon
        addTrayIcon(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const bool autostart = wcsstr(GetCommandLineW(), L"--autostart") != nullptr;
    // One gsmacro per user session: two would create two virtual pads. A second
    // start just brings the running one to the front.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\gsmacro-single-instance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"gsmacro", nullptr)) PostMessageW(other, WM_SHOWMAIN, 0, 0);
        return 0;
    }
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0L, 0L, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"gsmacro", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"gsmacro - GameSir macro tool", WS_OVERLAPPEDWINDOW, 80, 60, (int)(1480 * scale),
                              (int)(940 * scale), nullptr, nullptr, wc.hInstance, nullptr);
    if (!createDevice(hwnd)) {
        cleanupDevice();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, L"Direct3D 11 initialisation failed.", L"gsmacro", MB_ICONERROR);
        return 1;
    }
    if (!autostart) {
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        UpdateWindow(hwnd);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    {
        gsm::App app;
        app.loadFonts(scale);
        app.applyStyle(scale);
        if (autostart) {
            app.enableAutostart();
            g_tray = true;
            addTrayIcon(hwnd);
        }
        bool done = false;
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;
            app.backgroundTick();  // background mode: Live Mode, slot 0, game-based arming (no-op otherwise)
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd) ||
                (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
                Sleep(50);  // minimised / hidden in the tray: don't render
                continue;
            }
            g_occluded = false;
            if (g_resizeW != 0 && g_resizeH != 0) {
                cleanupRenderTarget();
                g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
                g_resizeW = g_resizeH = 0;
                createRenderTarget();
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            const float clear[4] = {0.078f, 0.090f, 0.110f, 1.0f};
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_context->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            HRESULT hr = g_swap->Present(1, 0);  // vsync caps the GUI frame rate
            g_occluded = hr == DXGI_STATUS_OCCLUDED;
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
