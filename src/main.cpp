#include "rk_gdip.h"
#include <commctrl.h>
#include <ole2.h>
#include <shellapi.h>

#include "rk_common.h"
#include "rk_dockwnd.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
    if (lpCmdLine && wcsstr(lpCmdLine, L"-debug") != nullptr) {
        rk::g_debugLog = true;
    }

    // Smoke-test automation flag (--smoke-exit-ms=N) allows headless verification
    int smokeExitMs = 0;
    if (lpCmdLine) {
        const wchar_t* p = wcsstr(lpCmdLine, L"--smoke-exit-ms=");
        if (p) smokeExitMs = _wtoi(p + 16);
    }

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, rk::kSingleInstMutex);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(rk::kClassDock, nullptr);
        if (existing) {
            ShowWindow(existing, SW_SHOWNOACTIVATE);
            PostMessageW(existing, rk::WM_RK_ATTENTION, 1, 0);
            SetForegroundWindow(existing);
        }
        CloseHandle(hMutex);
        return 0;
    }

    if (FAILED(OleInitialize(nullptr))) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdipIn;
    ULONG_PTR gdipToken = 0;
    if (Gdiplus::GdiplusStartup(&gdipToken, &gdipIn, nullptr) != Gdiplus::Ok) {
        if (hMutex) CloseHandle(hMutex);
        OleUninitialize();
        return 1;
    }

    rk::DebugLog(L"RK Launcher x64 starting up...");
    int exitCode = 0;
    {
        rk::RKDockWnd dock;
        if (!dock.Create()) {
            MessageBoxW(nullptr, L"Failed to initialize RK Launcher x64 window.", L"RK Launcher x64", MB_ICONERROR);
            exitCode = 1;
        } else {
            if (smokeExitMs > 0) {
                SetTimer(dock.Hwnd(), 0x77, static_cast<UINT>(smokeExitMs), nullptr);
            }

            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (smokeExitMs > 0 && msg.message == WM_TIMER && msg.hwnd == dock.Hwnd() && msg.wParam == 0x77) {
                    PostMessageW(dock.Hwnd(), WM_CLOSE, 0, 0);
                    continue;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            dock.Destroy();
        }
    }

    Gdiplus::GdiplusShutdown(gdipToken);
    OleUninitialize();
    if (hMutex) CloseHandle(hMutex);
    return exitCode;
}
