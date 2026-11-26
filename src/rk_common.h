#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace rk {

// Original registered message names used between RKLauncher.exe and RKLauncher.dll
inline constexpr const wchar_t* kMsgHookMouseName    = L"RKHOOK-{B30856F0-D3DD-11d4-B00B-006067718D01}";
inline constexpr const wchar_t* kMsgHookCbtName      = L"RKHOOK-{B30856F0-D3DD-11d4-B00B-006067718D02}";
inline constexpr const wchar_t* kMsgTaskbarCreated   = L"TaskbarCreated";
inline constexpr const wchar_t* kMsgShellHook        = L"SHELLHOOK";

// Custom hook wParam subcodes sent over kMsgHookCbtName
inline constexpr WPARAM kCbtEventDestroy = 0x1002;

// Internal private dock window messages (0x52c..0x538)
inline constexpr UINT WM_RK_LAYOUT_CHANGED  = WM_USER + 0x12c; // 0x52c
inline constexpr UINT WM_RK_ITEM_CHANGED    = WM_USER + 0x12e; // 0x52e
inline constexpr UINT WM_RK_SAVE_ITEMS      = WM_USER + 0x12f; // 0x52f
inline constexpr UINT WM_RK_SHOW_PROPERTIES = WM_USER + 0x130; // 0x530
inline constexpr UINT WM_RK_DOCKLET_LOCK    = WM_USER + 0x133; // 0x533
inline constexpr UINT WM_RK_ATTENTION       = WM_USER + 0x138; // 0x538

// Window class names matching original binary
inline constexpr const wchar_t* kClassDock       = L"RKDockClass";
inline constexpr const wchar_t* kClassDrag       = L"RKDockDragClass";
inline constexpr const wchar_t* kClassEffect     = L"RKDockEffectClass";
inline constexpr const wchar_t* kClassText       = L"RKDockTextClass";
inline constexpr const wchar_t* kClassDockletOD  = L"RKDockDockletClassOD";
inline constexpr const wchar_t* kTitleDockletWnd = L"RKDockletWnd";
inline constexpr const wchar_t* kSingleInstMutex = L"RKLauncherSingleInstanceMutex_x64";

// Dock positions (0..11, exact match with original RKLauncher.conf & MENU 130)
enum DockPosition : int {
    POS_TOP_LEFT      = 0,
    POS_TOP_CENTER    = 1,
    POS_TOP_RIGHT     = 2,
    POS_BOTTOM_LEFT   = 3,
    POS_BOTTOM_CENTER = 4,
    POS_BOTTOM_RIGHT  = 5,
    POS_LEFT_TOP      = 6,
    POS_LEFT_CENTER   = 7,
    POS_LEFT_BOTTOM   = 8,
    POS_RIGHT_TOP     = 9,
    POS_RIGHT_CENTER  = 10,
    POS_RIGHT_BOTTOM  = 11,
};

// Dock edges returned by DockletQueryDockEdge (observed in rk_unpacked_mem.bin 0x44c700)
enum DockEdge : int {
    DOCK_EDGE_BOTTOM = 0,
    DOCK_EDGE_TOP    = 1,
    DOCK_EDGE_LEFT   = 2,
    DOCK_EDGE_RIGHT  = 4,
};

inline bool IsHorizontalPos(int pos) {
    return (pos >= POS_TOP_LEFT && pos <= POS_BOTTOM_RIGHT);
}

inline int PositionToEdge(int pos) {
    if (pos >= 3 && pos <= 5) return DOCK_EDGE_BOTTOM;
    if (pos >= 0 && pos <= 2) return DOCK_EDGE_TOP;
    if (pos >= 6 && pos <= 8) return DOCK_EDGE_LEFT;
    if (pos >= 9 && pos <= 11) return DOCK_EDGE_RIGHT;
    return DOCK_EDGE_BOTTOM;
}

inline bool IsTopEdge(int pos)    { return pos >= 0 && pos <= 2; }
inline bool IsBottomEdge(int pos) { return pos >= 3 && pos <= 5; }
inline bool IsLeftEdge(int pos)   { return pos >= 6 && pos <= 8; }
inline bool IsRightEdge(int pos)  { return pos >= 9 && pos <= 11; }

// Common path & string utilities (implemented in rk_common.cpp)
std::wstring GetExeDir();
std::wstring ResolveAppPath(const std::wstring& relOrAbs);
std::wstring MakeRelativeIfInsideAppDir(const std::wstring& path);
bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
std::wstring NormalizeSlashes(std::wstring s, wchar_t sep = L'/');
std::wstring ToLower(std::wstring s);
std::wstring Trim(const std::wstring& s);
std::string WideToUtf8(const std::wstring& ws);
std::wstring Utf8ToWide(const std::string& s);

// Inspect PE header before LoadLibraryW so 32-bit DLLs are skipped safely
bool IsPeMachineX64(const std::wstring& dllPath, uint16_t* outMachine = nullptr);

// Global debug flag (-debug on cmdline)
extern bool g_debugLog;
void DebugLog(const wchar_t* fmt, ...);

} // namespace rk
