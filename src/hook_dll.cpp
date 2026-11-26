#define RKHOOK_EXPORTS
#include "rk_hook.h"
#include "rk_common.h"

#pragma comment(linker, "/SECTION:.shared,RWS")
#pragma data_seg(".shared")
static HWND     g_hwndDock       = nullptr;
static HHOOK    g_hhookMouse     = nullptr;
static int      g_disableMask    = 0;
static BOOL     g_minimizeToDock = TRUE;
static DWORD    g_dockThreadId   = 0;
#pragma data_seg()

static HINSTANCE   g_hInst            = nullptr;
static UINT        g_msgMouse         = 0;
static UINT        g_msgCbt           = 0;
static UINT          g_msgShellHook     = 0;
static HWINEVENTHOOK g_winEventFg       = nullptr;
static HWINEVENTHOOK g_winEventMin      = nullptr;
static HWINEVENTHOOK g_winEventDestroy  = nullptr;
static DWORD         g_lastMouseTick    = 0;
static POINT       g_lastMousePt      = { -99999, -99999 };

static void EnsureMessagesRegistered() {
    if (!g_msgMouse)    g_msgMouse    = RegisterWindowMessageW(rk::kMsgHookMouseName);
    if (!g_msgCbt)      g_msgCbt      = RegisterWindowMessageW(rk::kMsgHookCbtName);
    if (!g_msgShellHook)g_msgShellHook= RegisterWindowMessageW(rk::kMsgShellHook);
}

static bool IsInterestingTopWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (hwnd == g_hwndDock) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;

    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, rk::kClassDock) == 0 ||
        wcscmp(cls, rk::kClassDrag) == 0 ||
        wcscmp(cls, rk::kClassEffect) == 0 ||
        wcscmp(cls, rk::kClassText) == 0 ||
        wcscmp(cls, rk::kClassDockletOD) == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 ||
        wcscmp(cls, L"Progman") == 0 ||
        wcscmp(cls, L"WorkerW") == 0) {
        return false;
    }

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;
    return true;
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && (g_disableMask & 1) == 0 && g_hwndDock && lParam) {
        EnsureMessagesRegistered();
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (wParam == WM_MOUSEMOVE) {
            DWORD now = GetTickCount();
            if (ms->pt.x != g_lastMousePt.x || ms->pt.y != g_lastMousePt.y) {
                // Throttle mouse move notifications slightly (~15ms) while never dropping clicks
                if (now - g_lastMouseTick >= 15) {
                    g_lastMouseTick = now;
                    g_lastMousePt = ms->pt;
                    PostMessageW(g_hwndDock, g_msgMouse, wParam, MAKELPARAM((short)ms->pt.x, (short)ms->pt.y));
                }
            }
        } else if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP ||
                   wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP) {
            g_lastMousePt = ms->pt;
            PostMessageW(g_hwndDock, g_msgMouse, wParam, MAKELPARAM((short)ms->pt.x, (short)ms->pt.y));
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Cross-architecture (32-bit & 64-bit) out-of-context WinEvent listener running in dock UI thread
static void CALLBACK WinEventProc(HWINEVENTHOOK /*hWinEventHook*/, DWORD event, HWND hwnd,
                                  LONG idObject, LONG idChild, DWORD /*idEventThread*/, DWORD /*dwmsEventTime*/) {
    if ((g_disableMask & 2) != 0 || !g_hwndDock || !hwnd) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    EnsureMessagesRegistered();
    switch (event) {
    case EVENT_SYSTEM_MINIMIZESTART:
        if (g_minimizeToDock && IsInterestingTopWindow(hwnd)) {
            PostMessageW(g_hwndDock, g_msgCbt, SW_SHOWMINIMIZED, reinterpret_cast<LPARAM>(hwnd));
        }
        break;
    case EVENT_SYSTEM_MINIMIZEEND:
        if (IsInterestingTopWindow(hwnd)) {
            PostMessageW(g_hwndDock, g_msgCbt, SW_RESTORE, reinterpret_cast<LPARAM>(hwnd));
        }
        break;
    case EVENT_SYSTEM_FOREGROUND:
        if (g_msgShellHook && IsInterestingTopWindow(hwnd)) {
            PostMessageW(g_hwndDock, g_msgShellHook, HSHELL_WINDOWACTIVATED, reinterpret_cast<LPARAM>(hwnd));
        }
        break;
    case EVENT_OBJECT_DESTROY:
        if (hwnd != g_hwndDock) {
            PostMessageW(g_hwndDock, g_msgCbt, rk::kCbtEventDestroy, reinterpret_cast<LPARAM>(hwnd));
        }
        break;
    default:
        break;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hInst = hModule;
        DisableThreadLibraryCalls(hModule);
        EnsureMessagesRegistered();
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (GetCurrentThreadId() == g_dockThreadId) {
            RKRemoveHookMouse(g_hwndDock);
            RKRemoveHookCBT(g_hwndDock);
        }
    }
    return TRUE;
}

extern "C" {

RKHOOK_API BOOL __stdcall RKHookDisable(int mask) {
    g_disableMask |= mask;
    return TRUE;
}

RKHOOK_API BOOL __stdcall RKHookEnable(int mask) {
    g_disableMask &= ~mask;
    return TRUE;
}

RKHOOK_API BOOL __stdcall RKSetHookMouse(HWND hwndDock) {
    if (!hwndDock) return FALSE;
    EnsureMessagesRegistered();
    g_hwndDock = hwndDock;
    if (!g_hhookMouse) {
        g_hhookMouse = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g_hInst, 0);
    }
    return g_hhookMouse != nullptr;
}

RKHOOK_API BOOL __stdcall RKRemoveHookMouse(HWND /*hwndDock*/) {
    if (g_hhookMouse) {
        UnhookWindowsHookEx(g_hhookMouse);
        g_hhookMouse = nullptr;
    }
    return TRUE;
}

RKHOOK_API BOOL __stdcall RKSetHookCBT(HWND hwndDock) {
    if (!hwndDock) return FALSE;
    EnsureMessagesRegistered();
    g_hwndDock = hwndDock;
    g_dockThreadId = GetCurrentThreadId();

    // Use narrow out-of-context WinEvent hooks for cross-arch (x86/x64) coverage without event storms
    if (!g_winEventFg) {
        g_winEventFg = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    if (!g_winEventMin) {
        g_winEventMin = SetWinEventHook(
            EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    if (!g_winEventDestroy) {
        g_winEventDestroy = SetWinEventHook(
            EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    return (g_winEventFg != nullptr || g_winEventMin != nullptr || g_winEventDestroy != nullptr);
}

RKHOOK_API BOOL __stdcall RKRemoveHookCBT(HWND /*hwndDock*/) {
    if (g_winEventFg) {
        UnhookWinEvent(g_winEventFg);
        g_winEventFg = nullptr;
    }
    if (g_winEventMin) {
        UnhookWinEvent(g_winEventMin);
        g_winEventMin = nullptr;
    }
    if (g_winEventDestroy) {
        UnhookWinEvent(g_winEventDestroy);
        g_winEventDestroy = nullptr;
    }
    return TRUE;
}

RKHOOK_API BOOL __stdcall RKSetMinimize(BOOL enable) {
    g_minimizeToDock = enable ? TRUE : FALSE;
    return TRUE;
}

} // extern "C"
