#pragma once

#include <windows.h>

#ifdef RKHOOK_EXPORTS
#define RKHOOK_API __declspec(dllexport)
#else
#define RKHOOK_API __declspec(dllimport)
#endif

extern "C" {
    RKHOOK_API BOOL __stdcall RKHookDisable(int mask);
    RKHOOK_API BOOL __stdcall RKHookEnable(int mask);
    RKHOOK_API BOOL __stdcall RKSetHookMouse(HWND hwndDock);
    RKHOOK_API BOOL __stdcall RKSetHookCBT(HWND hwndDock);
    RKHOOK_API BOOL __stdcall RKRemoveHookCBT(HWND hwndDock);
    RKHOOK_API BOOL __stdcall RKRemoveHookMouse(HWND hwndDock);
    RKHOOK_API BOOL __stdcall RKSetMinimize(BOOL enable);
}
