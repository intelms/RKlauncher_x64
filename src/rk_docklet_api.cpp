#include "rk_items.h"
#include "rk_common.h"
#include <commdlg.h>
#include <shlwapi.h>

extern "C" {

__declspec(dllexport) const char* __stdcall RKLauncherGetVersion() {
    return "RK Launcher 0.41 Beta";
}

__declspec(dllexport) BOOL __stdcall DockletIsVisible(HWND hwndDocklet) {
    auto* item = rk::RKDockItemDockletOD::FindByHwnd(hwndDocklet);
    if (!item || item->markedForRemoval) return FALSE;
    HWND hwndDock = rk::RKDockItemDockletOD::HostDockHwnd();
    if (!hwndDock || !IsWindowVisible(hwndDock)) return FALSE;
    return TRUE;
}

__declspec(dllexport) BOOL __stdcall DockletGetRect(HWND hwndDocklet, RECT* rcDocklet) {
    if (!rcDocklet) return FALSE;
    auto* item = rk::RKDockItemDockletOD::FindByHwnd(hwndDocklet);
    if (!item) return FALSE;
    *rcDocklet = item->screenRect;
    return TRUE;
}

__declspec(dllexport) int __stdcall DockletGetLabel(HWND hwndDocklet, char* szLabel) {
    auto* item = rk::RKDockItemDockletOD::FindByHwnd(hwndDocklet);
    if (!item) return 0;
    std::string u8 = rk::WideToUtf8(item->Label());
    if (szLabel) {
        strcpy_s(szLabel, 256, u8.c_str());
    }
    return static_cast<int>(u8.size());
}

__declspec(dllexport) int __stdcall DockletSetLabel(HWND hwndDocklet, const char* szLabel) {
    auto* item = rk::RKDockItemDockletOD::FindByHwnd(hwndDocklet);
    if (!item || !szLabel) return 0;
    item->SetLabel(rk::Utf8ToWide(szLabel));
    HWND hwndDock = rk::RKDockItemDockletOD::HostDockHwnd();
    if (hwndDock) PostMessageW(hwndDock, rk::WM_RK_ITEM_CHANGED, 0, reinterpret_cast<LPARAM>(item));
    return static_cast<int>(item->Label().size());
}

__declspec(dllexport) Gdiplus::Bitmap* __stdcall DockletLoadGDIPlusImage(const char* szImage) {
    if (!szImage || !*szImage) return nullptr;
    std::wstring rel = rk::Utf8ToWide(szImage);
    auto bmp = rk::LoadBitmapAuto(rel);
    return bmp.release();
}

static Gdiplus::Bitmap* CloneGdiplusBitmap(Gdiplus::Image* img) {
    if (!img) return nullptr;
    int w = static_cast<int>(img->GetWidth());
    int h = static_cast<int>(img->GetHeight());
    if (w <= 0 || h <= 0) return nullptr;
    auto* cloned = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(cloned);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    g.DrawImage(img, 0, 0, w, h);
    return cloned;
}

__declspec(dllexport) void __stdcall DockletSetImage(HWND hwndDocklet, Gdiplus::Image* lpImageNew, BOOL bAutomaticallyDeleteImage) {
    auto* item = rk::RKDockItemDockletOD::FindByHwnd(hwndDocklet);
    if (!item) {
        if (bAutomaticallyDeleteImage && lpImageNew) delete lpImageNew;
        return;
    }
    Gdiplus::Bitmap* bmp = dynamic_cast<Gdiplus::Bitmap*>(lpImageNew);
    if (!lpImageNew) {
        item->SetCustomBitmap(nullptr, false);
    } else if (!bAutomaticallyDeleteImage || !bmp) {
        // Clone into host CRT heap so non-owned pointers stay valid and plugins can delete locally
        Gdiplus::Bitmap* cloned = CloneGdiplusBitmap(lpImageNew);
        if (bAutomaticallyDeleteImage) delete lpImageNew;
        item->SetCustomBitmap(cloned, cloned != nullptr);
    } else {
        item->SetCustomBitmap(bmp, true);
    }
    HWND hwndDock = rk::RKDockItemDockletOD::HostDockHwnd();
    if (hwndDock) PostMessageW(hwndDock, rk::WM_RK_ITEM_CHANGED, 0, 0);
}

__declspec(dllexport) void __stdcall DockletSetImageFile(HWND hwndDocklet, const char* szImage) {
    auto* item = rk::RKDockItemDockletOD::FindByHwnd(hwndDocklet);
    if (!item || !szImage) return;
    item->SetIconPath(rk::Utf8ToWide(szImage));
    HWND hwndDock = rk::RKDockItemDockletOD::HostDockHwnd();
    if (hwndDock) PostMessageW(hwndDock, rk::WM_RK_ITEM_CHANGED, 0, 0);
}

__declspec(dllexport) void __stdcall DockletSetImageOverlay(HWND hwndDocklet, Gdiplus::Image* lpImageNew, BOOL bAutomaticallyDeleteImage) {
    auto* item = rk::RKDockItemDockletOD::FindByHwnd(hwndDocklet);
    if (!item) {
        if (bAutomaticallyDeleteImage && lpImageNew) delete lpImageNew;
        return;
    }
    Gdiplus::Bitmap* bmp = dynamic_cast<Gdiplus::Bitmap*>(lpImageNew);
    if (!lpImageNew) {
        item->SetOverlayBitmap(nullptr, false);
    } else if (!bAutomaticallyDeleteImage || !bmp) {
        Gdiplus::Bitmap* cloned = CloneGdiplusBitmap(lpImageNew);
        if (bAutomaticallyDeleteImage) delete lpImageNew;
        item->SetOverlayBitmap(cloned, cloned != nullptr);
    } else {
        item->SetOverlayBitmap(bmp, true);
    }
    HWND hwndDock = rk::RKDockItemDockletOD::HostDockHwnd();
    if (hwndDock) PostMessageW(hwndDock, rk::WM_RK_ITEM_CHANGED, 0, 0);
}

__declspec(dllexport) void __stdcall DockletSetIndicator(HWND hwndDocklet, BOOL bEnable) {
    auto* item = rk::RKDockItemDockletOD::FindByHwnd(hwndDocklet);
    if (!item) return;
    item->SetShowIndicator(bEnable != FALSE);
    HWND hwndDock = rk::RKDockItemDockletOD::HostDockHwnd();
    if (hwndDock) PostMessageW(hwndDock, rk::WM_RK_ITEM_CHANGED, 0, 0);
}

__declspec(dllexport) void __stdcall DockletLockMouseEffect(HWND hwndDocklet, BOOL bLock) {
    HWND hwndDock = rk::RKDockItemDockletOD::HostDockHwnd();
    if (hwndDock) {
        PostMessageW(hwndDock, rk::WM_RK_DOCKLET_LOCK,
                     bLock ? 1 : 0, reinterpret_cast<LPARAM>(hwndDocklet));
    }
}

__declspec(dllexport) void __stdcall DockletDoAttentionAnimation(HWND hwndDocklet) {
    auto* item = rk::RKDockItemDockletOD::FindByHwnd(hwndDocklet);
    if (!item) return;
    item->StartBounce(3);
    HWND hwndDock = rk::RKDockItemDockletOD::HostDockHwnd();
    if (hwndDock) PostMessageW(hwndDock, rk::WM_RK_ATTENTION, 0, reinterpret_cast<LPARAM>(item));
}

__declspec(dllexport) void __stdcall DockletGetRootFolder(HWND /*hwndDocklet*/, char* szFolder) {
    if (!szFolder) return;
    std::wstring dir = rk::NormalizeSlashes(rk::GetExeDir(), L'/');
    if (!dir.empty() && dir.back() != L'/') dir.push_back(L'/');
    std::string u8 = rk::WideToUtf8(dir);
    strcpy_s(szFolder, MAX_PATH, u8.c_str());
}

__declspec(dllexport) void __stdcall DockletGetRelativeFolder(HWND hwndDocklet, char* szFolder) {
    if (!szFolder) return;
    szFolder[0] = '\0';
    auto* item = rk::RKDockItemDockletOD::FindByHwnd(hwndDocklet);
    if (!item) return;
    wchar_t dirBuf[MAX_PATH] = {};
    wcsncpy_s(dirBuf, rk::ResolveAppPath(item->DllPath()).c_str(), _TRUNCATE);
    PathRemoveFileSpecW(dirBuf);
    std::wstring rel = rk::NormalizeSlashes(rk::MakeRelativeIfInsideAppDir(dirBuf), L'/');
    if (!rel.empty() && rel.back() != L'/') rel.push_back(L'/');
    std::string u8 = rk::WideToUtf8(rel);
    strcpy_s(szFolder, MAX_PATH, u8.c_str());
}

__declspec(dllexport) int __stdcall DockletQueryDockEdge(HWND /*hwndDocklet*/) {
    HWND hwndDock = rk::RKDockItemDockletOD::HostDockHwnd();
    if (!hwndDock) return rk::DOCK_EDGE_BOTTOM;
    int pos = static_cast<int>(SendMessageW(hwndDock, rk::WM_RK_LAYOUT_CHANGED, 0xFFFF, 0));
    return rk::PositionToEdge(pos);
}

__declspec(dllexport) int __stdcall DockletSetDockEdge(HWND /*hwndDocklet*/, int iNewEdge) {
    HWND hwndDock = rk::RKDockItemDockletOD::HostDockHwnd();
    if (!hwndDock) return iNewEdge;
    UINT cmd = 0;
    if (iNewEdge == rk::DOCK_EDGE_BOTTOM)      cmd = 0x800e; // BottomCenter (pos 4)
    else if (iNewEdge == rk::DOCK_EDGE_TOP)    cmd = 0x8017; // TopCenter    (pos 1)
    else if (iNewEdge == rk::DOCK_EDGE_LEFT)   cmd = 0x8011; // LeftCenter   (pos 7)
    else if (iNewEdge == rk::DOCK_EDGE_RIGHT)  cmd = 0x8014; // RightCenter  (pos 10)
    if (cmd) SendMessageW(hwndDock, WM_COMMAND, cmd, 0);
    return iNewEdge;
}

__declspec(dllexport) int __stdcall DockletQueryDockAlign(HWND /*hwndDocklet*/) {
    return 1; // Center aligned (matching 0x421b50)
}

__declspec(dllexport) int __stdcall DockletSetDockAlign(HWND /*hwndDocklet*/, int /*iNewAlign*/) {
    return 1;
}

__declspec(dllexport) BOOL __stdcall DockletDefaultConfigDialog(HWND hwndDocklet, void* /*pData*/) {
    auto* item = rk::RKDockItemDockletOD::FindByHwnd(hwndDocklet);
    HWND hwndDock = rk::RKDockItemDockletOD::HostDockHwnd();
    if (!item || !hwndDock) return FALSE;
    PostMessageW(hwndDock, rk::WM_RK_SHOW_PROPERTIES, 0, reinterpret_cast<LPARAM>(item));
    return TRUE;
}

__declspec(dllexport) BOOL __stdcall DockletBrowseForImage(HWND hwndParent, char* szImage, const char* /*szAlternateRelativeRoot*/) {
    if (!szImage) return FALSE;
    wchar_t file[MAX_PATH] = {};
    if (*szImage) {
        wcsncpy_s(file, rk::ResolveAppPath(rk::Utf8ToWide(szImage)).c_str(), _TRUNCATE);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwndParent ? hwndParent : rk::RKDockItemDockletOD::HostDockHwnd();
    ofn.lpstrFilter = L"Image Files (*.png;*.ico;*.jpg;*.bmp)\0*.png;*.ico;*.jpg;*.bmp\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        std::wstring rel = rk::NormalizeSlashes(rk::MakeRelativeIfInsideAppDir(file), L'/');
        std::string u8 = rk::WideToUtf8(rel);
        strcpy_s(szImage, MAX_PATH, u8.c_str());
        return TRUE;
    }
    return FALSE;
}

} // extern "C"
