#pragma once

#include "rk_gdip.h"
#include <shellapi.h>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "rk_ini.h"
#include "rk_options.h"

namespace rk {

enum class DockItemType {
    Shortcut = 0,
    Separator = 1,
    Window = 2,
    DockletOD = 5,
    Folder = 6,
};

struct DockletHostState; // opaque for DockletOD window registry

class RKDockItem {
public:
    virtual ~RKDockItem() = default;

    virtual DockItemType Type() const = 0;
    virtual std::wstring TypeString() const = 0;

    virtual bool LoadConfig(const IniSection& sec);
    virtual void SaveConfig(IniSection& sec) const;

    virtual void Execute(HWND hwndDock) = 0;
    virtual void OnDoubleClick(HWND hwndDock) { Execute(hwndDock); }
    virtual void OnRightClick(HWND hwndDock, POINT screenPt);
    virtual bool AcceptsDrop() const { return false; }
    virtual void OnDropFiles(HWND /*hwndDock*/, HDROP /*hDrop*/) {}

    virtual bool IsRunning() const { return false; }
    virtual HWND GetTargetHwnd() const { return nullptr; }
    virtual void RefreshRuntimeState() {}
    virtual void RefreshRuntimeStateWithSet(const std::unordered_set<std::wstring>& /*runningExes*/) { RefreshRuntimeState(); }

    // Icon management
    void ReloadIcon();
    void SetCustomBitmap(Gdiplus::Bitmap* bmp, bool takeOwnership);
    void SetOverlayBitmap(Gdiplus::Bitmap* bmp, bool takeOwnership);
    Gdiplus::Bitmap* PrimaryBitmap() const { return iconBmp_.get(); }
    Gdiplus::Bitmap* OverlayBitmap() const { return overlayBmp_.get(); }

    // Attention / launch bounce animation
    void StartBounce(int count);
    bool IsBouncing() const { return bounceLeft_ > 0; }
    float GetBounceOffset01(DWORD nowTick); // updates bounce state, returns 0..1

    const std::wstring& Label() const { return label_; }
    void SetLabel(const std::wstring& l) { label_ = l; }

    const std::wstring& IconPath() const { return iconPath_; }
    void SetIconPath(const std::wstring& p) { iconPath_ = p; ReloadIcon(); }

    bool ShowIndicator() const { return showIndicator_; }
    void SetShowIndicator(bool v) { showIndicator_ = v; }

    RECT localRect{};  // position in dock bitmap coordinates
    RECT screenRect{}; // position on screen

    bool markedForRemoval = false;

protected:
    std::wstring label_;
    std::wstring iconPath_;
    bool showIndicator_ = true;

    std::unique_ptr<Gdiplus::Bitmap> iconBmp_;
    bool ownsIconBmp_ = true;
    std::unique_ptr<Gdiplus::Bitmap> overlayBmp_;
    bool ownsOverlayBmp_ = true;

    int   bounceLeft_      = 0;
    DWORD bouncePhaseTick_ = 0;

    virtual std::unique_ptr<Gdiplus::Bitmap> ResolveDefaultIcon();
};

class RKDockItemShortcut : public RKDockItem {
public:
    DockItemType Type() const override { return DockItemType::Shortcut; }
    std::wstring TypeString() const override { return L"shortcut"; }

    bool LoadConfig(const IniSection& sec) override;
    void SaveConfig(IniSection& sec) const override;
    void Execute(HWND hwndDock) override;
    bool AcceptsDrop() const override { return !link_.empty(); }
    void OnDropFiles(HWND hwndDock, HDROP hDrop) override;
    bool IsRunning() const override;
    void RefreshRuntimeState() override;
    void RefreshRuntimeStateWithSet(const std::unordered_set<std::wstring>& runningExes) override;

    bool InitFromDroppedFile(const std::wstring& filePath);

    std::wstring link_;
    std::wstring arguments_;
    std::wstring workFolder_;
    std::wstring showCmd_ = L"normal"; // normal / minimized / maximized
    bool         sameInstance_ = true;

private:
    bool cachedRunning_ = false;
    std::wstring cachedLinkKey_;
    std::wstring cachedExeNameLower_;

protected:
    std::unique_ptr<Gdiplus::Bitmap> ResolveDefaultIcon() override;
};

std::unordered_set<std::wstring> CollectRunningExeNames();

class RKDockItemFolder : public RKDockItemShortcut {
public:
    DockItemType Type() const override { return DockItemType::Folder; }
    std::wstring TypeString() const override { return L"folder"; }

    void Execute(HWND hwndDock) override;
    void ShowFolderPopupMenu(HWND hwndDock, POINT screenPt, const RKDockOptions& opts);
};

class RKDockItemSeparator : public RKDockItem {
public:
    RKDockItemSeparator() { label_.clear(); showIndicator_ = false; }
    DockItemType Type() const override { return DockItemType::Separator; }
    std::wstring TypeString() const override { return L"separator"; }

    void Execute(HWND /*hwndDock*/) override {}
};

class RKDockItemWindow : public RKDockItem {
public:
    explicit RKDockItemWindow(HWND target = nullptr);
    ~RKDockItemWindow() override;

    DockItemType Type() const override { return DockItemType::Window; }
    std::wstring TypeString() const override { return L"window"; }

    bool LoadConfig(const IniSection& sec) override;
    void SaveConfig(IniSection& sec) const override;
    void Execute(HWND hwndDock) override;
    bool IsRunning() const override { return targetHwnd_ && IsWindow(targetHwnd_); }
    HWND GetTargetHwnd() const override { return targetHwnd_; }
    void RefreshRuntimeState() override;

    void SetMinimizedByDock(bool v) { minimizedByDock_ = v; }
    bool WasMinimizedByDock() const { return minimizedByDock_; }
    void CloseTargetWindow();

protected:
    std::unique_ptr<Gdiplus::Bitmap> ResolveDefaultIcon() override;

private:
    HWND targetHwnd_ = nullptr;
    bool minimizedByDock_ = false;
    DWORD lastIconRefresh_ = 0;
};

class RKDockItemDockletOD : public RKDockItem {
public:
    RKDockItemDockletOD() = default;
    ~RKDockItemDockletOD() override;

    DockItemType Type() const override { return DockItemType::DockletOD; }
    std::wstring TypeString() const override { return L"docklet_od"; }

    bool LoadConfig(const IniSection& sec) override;
    void SaveConfig(IniSection& sec) const override;
    void Execute(HWND hwndDock) override;
    void OnDoubleClick(HWND hwndDock) override;
    void OnRightClick(HWND hwndDock, POINT screenPt) override;
    bool TryPluginRightClick();
    bool AcceptsDrop() const override;
    void OnDropFiles(HWND hwndDock, HDROP hDrop) override;

    bool InitializePlugin(HWND hwndDockParent, const std::wstring& dllRelOrAbs,
                          const std::wstring& iniPath, const std::wstring& iniGroup);
    void ShutdownPlugin();
    void ConfigurePlugin();

    const std::wstring& DllPath() const { return dllPath_; }

    // Lookup active docklet by its hidden HWND (used by exported Docklet* APIs)
    static RKDockItemDockletOD* FindByHwnd(HWND hwndDocklet);
    static void RegisterHostDockHwnd(HWND hwndDock);
    static HWND HostDockHwnd();

private:
    std::wstring pluginName_;
    std::wstring dllPath_;
    std::wstring configIniFile_;
    std::wstring configIniGroup_;
    std::wstring relativeFolder_;
    bool loaded_ = true;
    mutable std::vector<IniEntry> inlineIniEntries_;

    bool IsTempIniFile() const;
    void StageTempIniForLoad() const;

    HMODULE hMod_ = nullptr;
    HWND hwndDocklet_ = nullptr;
    void* pluginData_ = nullptr;

    // Standard Stardock ObjectDock SDK entry points
    using FnOnCreate          = void* (__stdcall*)(HWND hwndDocklet, HINSTANCE hInstance, char* szIni, char* szIniGroup);
    using FnOnDestroy         = void  (__stdcall*)(void* lpData, HWND hwndDocklet);
    using FnOnSave            = void  (__stdcall*)(void* lpData, char* szIni, char* szIniGroup, BOOL bIsForExport);
    using FnOnLeftClick       = BOOL  (__stdcall*)(void* lpData, POINT* ptCursor, SIZE* sizeDocklet);
    using FnOnRightClick      = BOOL  (__stdcall*)(void* lpData, POINT* ptCursor, SIZE* sizeDocklet);
    using FnOnDoubleClick     = BOOL  (__stdcall*)(void* lpData, POINT* ptCursor, SIZE* sizeDocklet);
    using FnOnLeftHeld        = BOOL  (__stdcall*)(void* lpData, POINT* ptCursor, SIZE* sizeDocklet);
    using FnOnConfigure       = void  (__stdcall*)(void* lpData);
    using FnOnAcceptDropFiles = BOOL  (__stdcall*)(void* lpData);
    using FnOnDropFiles       = void  (__stdcall*)(void* lpData, HDROP hDrop);
    using FnOnProcessMessage  = void  (__stdcall*)(void* lpData, HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    using FnOnGetInfo         = void  (__stdcall*)(char* szName, char* szAuthor, int* piVersion, char* szNotes);

    FnOnCreate          fnOnCreate_ = nullptr;
    FnOnDestroy         fnOnDestroy_ = nullptr;
    FnOnSave            fnOnSave_ = nullptr;
    FnOnLeftClick       fnOnLeftClick_ = nullptr;
    FnOnRightClick      fnOnRightClick_ = nullptr;
    FnOnDoubleClick     fnOnDoubleClick_ = nullptr;
    FnOnLeftHeld        fnOnLeftHeld_ = nullptr;
    FnOnConfigure       fnOnConfigure_ = nullptr;
    FnOnAcceptDropFiles fnOnAcceptDropFiles_ = nullptr;
    FnOnDropFiles       fnOnDropFiles_ = nullptr;
    FnOnProcessMessage  fnOnProcessMessage_ = nullptr;
    FnOnGetInfo         fnOnGetInfo_ = nullptr;

    static LRESULT CALLBACK DockletWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

// Icon / ItemList helpers
std::unique_ptr<Gdiplus::Bitmap> LoadBitmapAuto(const std::wstring& path);
std::unique_ptr<Gdiplus::Bitmap> BitmapFromHIcon(HICON hIcon);
std::unique_ptr<Gdiplus::Bitmap> ExtractIconBitmapForPath(const std::wstring& path, int desiredSize = 128);
std::unique_ptr<Gdiplus::Bitmap> CreateBuiltinFallbackIcon(const std::wstring& hint, int size = 128);

bool LoadItemListFile(const std::wstring& itemlistPath, HWND hwndDock,
                      std::vector<std::unique_ptr<RKDockItem>>& outItems);
bool SaveItemListFile(const std::wstring& itemlistPath,
                      const std::vector<std::unique_ptr<RKDockItem>>& items);
void PopulateDefaultStandaloneItems(std::vector<std::unique_ptr<RKDockItem>>& outItems);

} // namespace rk
