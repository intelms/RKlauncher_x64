#pragma once

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus {
using std::min;
using std::max;
}
#include <gdiplus.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commoncontrols.h>

#include <cmath>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stackdock {

enum StackMode : int {
    MODE_AUTOMATIC = 0,
    MODE_FAN       = 1,
    MODE_GRID      = 2,
};

enum StackSort : int {
    SORT_NAME          = 0,
    SORT_DATE_CREATED  = 1,
    SORT_DATE_MODIFIED = 2,
    SORT_DATE_ACCESSED = 3,
    SORT_KIND          = 4,
};

enum StackFolderType : int {
    FTYPE_FOLDER  = 0,
    FTYPE_SPECIAL = 1,
};

enum StackIconStyle : int {
    ISTYLE_WHEN_OPEN         = 0,
    ISTYLE_ALWAYS_PLUS_THREE = 1,
    ISTYLE_ALWAYS            = 2,
};

// Matches rklauncher_x64 DockletQueryDockEdge constants
enum HostDockEdge : int {
    EDGE_BOTTOM = 0,
    EDGE_TOP    = 1,
    EDGE_LEFT   = 2,
    EDGE_RIGHT  = 4,
};

struct GlobalConfig {
    std::wstring openFolderText = L"Open folder";
    std::wstring dMoreText      = L"%d more...";
    std::wstring fontName       = L"Tahoma";
    int          fontSize       = 8;
    bool         fontBold       = false;
    bool         followDock     = false;
};

struct StackEntry {
    std::wstring name;
    std::wstring fullPath;
    std::wstring kindName;
    FILETIME     ftCreation{};
    FILETIME     ftLastWrite{};
    FILETIME     ftLastAccess{};
    LPITEMIDLIST pidlAbs = nullptr;
    int          sysIconIndex = -1;
    bool         isFolder = false;
    bool         isOpenFolderAction = false;
    bool         isMoreAction = false;
    int          moreCount = 0;
    std::shared_ptr<Gdiplus::Bitmap> icon;

    StackEntry() = default;
    StackEntry(const StackEntry&) = delete;
    StackEntry& operator=(const StackEntry&) = delete;
    StackEntry(StackEntry&& o) noexcept { *this = std::move(o); }
    StackEntry& operator=(StackEntry&& o) noexcept {
        if (this != &o) {
            if (pidlAbs) CoTaskMemFree(pidlAbs);
            name = std::move(o.name);
            fullPath = std::move(o.fullPath);
            kindName = std::move(o.kindName);
            ftCreation = o.ftCreation;
            ftLastWrite = o.ftLastWrite;
            ftLastAccess = o.ftLastAccess;
            pidlAbs = o.pidlAbs;
            o.pidlAbs = nullptr;
            sysIconIndex = o.sysIconIndex;
            o.sysIconIndex = -1;
            isFolder = o.isFolder;
            isOpenFolderAction = o.isOpenFolderAction;
            isMoreAction = o.isMoreAction;
            moreCount = o.moreCount;
            icon = std::move(o.icon);
        }
        return *this;
    }
    ~StackEntry() {
        if (pidlAbs) {
            CoTaskMemFree(pidlAbs);
            pidlAbs = nullptr;
        }
    }
};

struct HostApi {
    using FnSetImage       = void (__stdcall*)(HWND, Gdiplus::Image*, BOOL);
    using FnSetImageFile   = void (__stdcall*)(HWND, const char*);
    using FnGetRect        = BOOL (__stdcall*)(HWND, RECT*);
    using FnGetLabel       = int  (__stdcall*)(HWND, char*);
    using FnSetLabel       = int  (__stdcall*)(HWND, const char*);
    using FnGetFolder      = void (__stdcall*)(HWND, char*);
    using FnQueryEdge      = int  (__stdcall*)(HWND);
    using FnLockMouse      = void (__stdcall*)(HWND, BOOL);
    using FnBrowseForImage = BOOL (__stdcall*)(HWND, char*, char*);

    FnSetImage       setImage       = nullptr;
    FnSetImageFile   setImageFile   = nullptr;
    FnGetRect        getRect        = nullptr;
    FnGetLabel       getLabel       = nullptr;
    FnSetLabel       setLabel       = nullptr;
    FnGetFolder      getRelativeFolder = nullptr;
    FnGetFolder      getRootFolder     = nullptr;
    FnQueryEdge      queryDockEdge     = nullptr;
    FnLockMouse      lockMouseEffect   = nullptr;
    FnBrowseForImage browseForImage    = nullptr;
};

struct ScopedCom {
    bool shouldUninit = false;
    ScopedCom() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        shouldUninit = (hr == S_OK || hr == S_FALSE);
    }
    ~ScopedCom() {
        if (shouldUninit) CoUninitialize();
    }
};

class StackPopupWindow;

struct StackDockletInstance {
    HWND hwndDocklet = nullptr;
    HMODULE hPlugin  = nullptr;
    HostApi host{};

    std::wstring dllDir;
    std::wstring rootDir;

    std::wstring folder;
    std::wstring iconPath;
    std::wstring title;
    int mode          = MODE_AUTOMATIC;
    int sortMode      = SORT_NAME;
    bool hideOpenFolder = false;
    int folderType    = FTYPE_FOLDER;
    int iconStyle     = ISTYLE_WHEN_OPEN;

    bool popupOpen = false;
    ULONGLONG lastPopupCloseTick = 0;
    std::vector<StackEntry> items;
    StackPopupWindow* activePopup = nullptr;

    // Icon composition cache to skip redundant disk PNG loads and 256x256 host image clones
    bool cachedBaseIconAttempted = false;
    std::wstring cachedBaseIconKey;
    std::wstring cachedBaseIconRoot;
    std::shared_ptr<Gdiplus::Bitmap> cachedBaseIcon;

    bool hasCompositedIcon = false;
    bool cachedDrawItems   = false;
    bool cachedDrawTray    = false;
    std::weak_ptr<Gdiplus::Bitmap> cachedCompBasePtr;
    size_t cachedTopCount  = 0;
    std::weak_ptr<Gdiplus::Bitmap> cachedTopIcons[3];

    ~StackDockletInstance();
};

inline void AddRoundedRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d <= 0.5f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// Module & shared utilities (stack_docklet.cpp)
HMODULE PluginModule();
std::wstring DecodeHostString(const char* s);
std::string  EncodeUtf8String(const std::wstring& w);
std::wstring ResolvePathRelative(const StackDockletInstance* inst, const std::wstring& relOrAbs);
std::wstring MakeRelativePathIfInsideRoot(const StackDockletInstance* inst, const std::wstring& absPath);
std::shared_ptr<Gdiplus::Bitmap> LoadBitmapFileCopy(const std::wstring& path);
std::shared_ptr<Gdiplus::Bitmap> LoadImageAsset(const StackDockletInstance* inst, const wchar_t* imageName);

struct SpecialFolderInfo {
    const wchar_t* name;
    int csidl;
    const KNOWNFOLDERID* knownId;
};
const std::vector<SpecialFolderInfo>& GetSpecialFolderList();
LPITEMIDLIST ResolveSpecialFolderPidl(const std::wstring& name, std::wstring* outFsPath, std::wstring* outDisplayName);

void LoadGlobalConfig(GlobalConfig& cfg);
void SaveGlobalConfig(const GlobalConfig& cfg);
void RefreshStackItems(StackDockletInstance* inst);
void UpdateDockletIcon(StackDockletInstance* inst);
void UpdateDockletLabelFromConfig(StackDockletInstance* inst, bool forceIfNoCustomTitle);
bool ExecuteStackEntry(const StackDockletInstance* inst, const StackEntry& entry);
bool OpenFolderTarget(const StackDockletInstance* inst);

// UI & Popup (stack_docklet_popup.cpp)
bool ToggleOrShowStackPopup(StackDockletInstance* inst, const POINT* cursorPt, const SIZE* dockletSize);
void CloseStackPopup(StackDockletInstance* inst, bool immediate);
bool ShowStackConfigureDialog(StackDockletInstance* inst);

} // namespace stackdock
