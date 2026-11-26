#pragma once

#include <windows.h>
#include <prsht.h>
#include <string>
#include <vector>
#include <memory>

namespace rk {

struct RKDockOptions;
class RKDockItem;

// Menu command IDs matching original binary (0x8003..0x8022, 0x9c43)
enum DockCommandId : UINT {
    IDM_DOCK_QUIT            = 0x8003,
    IDM_DOCK_ADD_SEPARATOR   = 0x8004,
    IDM_DOCK_ADD_DOCKLET     = 0x8005,
    IDM_DOCK_SETTINGS        = 0x8006,
    IDM_DOCK_LANGUAGE        = 0x8007,
    IDM_DOCK_MAGNIFY         = 0x8008,
    IDM_DOCK_ALWAYSONTOP     = 0x8009,
    IDM_DOCK_LOCKITEMS       = 0x800b,
    IDM_POS_BOTTOM_LEFT      = 0x800d,
    IDM_POS_BOTTOM_CENTER    = 0x800e,
    IDM_POS_BOTTOM_RIGHT     = 0x800f,
    IDM_POS_LEFT_TOP         = 0x8010,
    IDM_POS_LEFT_CENTER      = 0x8011,
    IDM_POS_LEFT_BOTTOM      = 0x8012,
    IDM_POS_RIGHT_TOP        = 0x8013,
    IDM_POS_RIGHT_CENTER     = 0x8014,
    IDM_POS_RIGHT_BOTTOM     = 0x8015,
    IDM_POS_TOP_LEFT         = 0x8016,
    IDM_POS_TOP_CENTER       = 0x8017,
    IDM_POS_TOP_RIGHT        = 0x8018,
    IDM_ITEM_PROPERTIES      = 0x8019,
    IDM_ITEM_DELETE_DOCKLET  = 0x801a,
    IDM_ITEM_REMOVE          = 0x801b,
    IDM_DOCK_DEL_SEPARATOR   = 0x801c,
    IDM_ITEM_CLOSE_WINDOW    = 0x801d,
    IDM_DOCK_AUTOHIDE        = 0x8021,
    IDM_DOCK_MINIMIZE_TO_DOCK= 0x8022,
    IDM_DOCK_ABOUT           = 0x8030,
    IDM_DOCK_ADD_ITEM        = 0x9c43,
};

// Resource & .lang manager supporting external 32-bit/64-bit .lang DLLs as DATAFILE
// and falling back seamlessly to embedded module resources.
class RKLangManager {
public:
    static RKLangManager& Instance();

    bool LoadLanguage(const std::wstring& langFileName);
    void Unload();

    std::wstring LoadStr(UINT id, const wchar_t* defFallback = L"");
    HMENU LoadMenuTemplate(UINT menuId);
    INT_PTR RunModalDialog(UINT dlgId, HWND hwndParent, DLGPROC proc, LPARAM initParam = 0);
    HPROPSHEETPAGE CreatePage(UINT dlgId, DLGPROC proc, LPARAM initParam, const std::wstring& titleFallback);

    static std::vector<std::wstring> EnumerateLanguages();

private:
    RKLangManager() = default;
    ~RKLangManager();

    HMODULE hLang_ = nullptr;
    std::wstring currentFile_;

    const DLGTEMPLATE* FindDialogTemplate(UINT dlgId, HINSTANCE* outInst);
};

// High-level UI dialogs invoked from RKDockWnd
void ShowDockContextMenu(HWND hwndDock, POINT screenPt, const RKDockOptions& opts,
                         RKDockItem* hitItem, bool isSeparatorHit);

using SettingsApplyFn = void (*)(const RKDockOptions&, void*);
bool ShowSettingsPropertySheet(HWND hwndDock, RKDockOptions& opts,
                               SettingsApplyFn onApply = nullptr, void* onApplyCtx = nullptr);
bool ShowItemPropertiesDialog(HWND hwndDock, RKDockItem* item);
bool ShowAddDockletDialog(HWND hwndDock, std::wstring& outDllRelPath, std::wstring& outLabel);
bool ShowImageLoadDialog(HWND hwndParent, std::wstring& inOutImagePath);
bool ShowLanguageDialog(HWND hwndDock, RKDockOptions& opts);
void ShowAboutDialog(HWND hwndParent);

} // namespace rk
