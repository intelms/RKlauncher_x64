#include "rk_dialogs.h"
#include "rk_common.h"
#include "rk_options.h"
#include "rk_theme.h"
#include "rk_items.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <prsht.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <vector>

namespace rk {

// ============================================================================
// RKLangManager
// ============================================================================

RKLangManager& RKLangManager::Instance() {
    static RKLangManager inst;
    return inst;
}

RKLangManager::~RKLangManager() {
    Unload();
}

void RKLangManager::Unload() {
    if (hLang_) {
        FreeLibrary(hLang_);
        hLang_ = nullptr;
    }
    currentFile_.clear();
}

bool RKLangManager::LoadLanguage(const std::wstring& langFileName) {
    Unload();
    if (langFileName.empty()) return false;

    std::wstring candidate = ResolveAppPath(L"lang\\" + langFileName);
    if (!FileExists(candidate)) candidate = ResolveAppPath(langFileName);
    if (!FileExists(candidate)) {
        DebugLog(L"Language file not found (%s); using built-in English fallback.", langFileName.c_str());
        return false;
    }

    hLang_ = LoadLibraryExW(candidate.c_str(), nullptr,
                            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!hLang_) {
        DebugLog(L"LoadLibraryExW failed (%lu) on %s", GetLastError(), candidate.c_str());
        return false;
    }
    currentFile_ = langFileName;
    return true;
}

static bool ReadStringFromModule(HMODULE hMod, UINT id, std::wstring& out) {
    if (!hMod) return false;
    UINT blockId = (id >> 4) + 1;
    UINT index = id & 0x0F;
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(blockId), RT_STRING);
    if (!hRes) return false;
    HGLOBAL hMem = LoadResource(hMod, hRes);
    if (!hMem) return false;
    DWORD bytes = SizeofResource(hMod, hRes);
    const uint16_t* p = static_cast<const uint16_t*>(LockResource(hMem));
    if (!p || bytes < 2) return false;
    const uint16_t* end = p + bytes / 2;
    for (UINT i = 0; i <= index && p < end; ++i) {
        uint16_t len = *p++;
        if (i == index) {
            if (len > 0 && p + len <= end) {
                out.assign(reinterpret_cast<const wchar_t*>(p), len);
                return true;
            }
            return false;
        }
        p += len;
    }
    return false;
}

std::wstring RKLangManager::LoadStr(UINT id, const wchar_t* defFallback) {
    std::wstring res;
    if (hLang_ && ReadStringFromModule(hLang_, id, res) && !res.empty()) return res;
    if (ReadStringFromModule(GetModuleHandleW(nullptr), id, res) && !res.empty()) return res;
    return defFallback ? defFallback : L"";
}

HMENU RKLangManager::LoadMenuTemplate(UINT menuId) {
    auto loadFrom = [&](HMODULE hMod) -> HMENU {
        if (!hMod) return nullptr;
        HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(menuId), RT_MENU);
        if (!hRes) return nullptr;
        HGLOBAL hMem = LoadResource(hMod, hRes);
        if (!hMem) return nullptr;
        const void* tmpl = LockResource(hMem);
        return tmpl ? LoadMenuIndirectW(tmpl) : nullptr;
    };
    if (hLang_) {
        if (HMENU m = loadFrom(hLang_)) return m;
    }
    return loadFrom(GetModuleHandleW(nullptr));
}

const DLGTEMPLATE* RKLangManager::FindDialogTemplate(UINT dlgId, HINSTANCE* outInst) {
    auto findFrom = [&](HMODULE hMod) -> const DLGTEMPLATE* {
        if (!hMod) return nullptr;
        HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(dlgId), RT_DIALOG);
        if (!hRes) return nullptr;
        HGLOBAL hMem = LoadResource(hMod, hRes);
        if (!hMem) return nullptr;
        if (outInst) *outInst = hMod;
        return static_cast<const DLGTEMPLATE*>(LockResource(hMem));
    };
    if (hLang_) {
        if (const DLGTEMPLATE* p = findFrom(hLang_)) return p;
    }
    return findFrom(GetModuleHandleW(nullptr));
}

INT_PTR RKLangManager::RunModalDialog(UINT dlgId, HWND hwndParent, DLGPROC proc, LPARAM initParam) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const DLGTEMPLATE* tmpl = FindDialogTemplate(dlgId, &inst);
    if (tmpl) {
        return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), tmpl, hwndParent, proc, initParam);
    }
    return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(dlgId), hwndParent, proc, initParam);
}

static std::wstring ExtractDialogCaption(const DLGTEMPLATE* tmpl, const std::wstring& fallback) {
    if (!tmpl) return fallback;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(tmpl);
    auto readU16 = [&](size_t off) -> uint16_t { return *reinterpret_cast<const uint16_t*>(p + off); };
    auto skipSz = [&](size_t off) -> std::pair<size_t, std::wstring> {
        uint16_t v = readU16(off);
        if (v == 0) return {off + 2, L""};
        if (v == 0xFFFF) return {off + 4, L""};
        std::wstring s;
        while (readU16(off) != 0) {
            s.push_back(static_cast<wchar_t>(readU16(off)));
            off += 2;
        }
        return {off + 2, s};
    };
    bool isEx = (readU16(2) == 0xFFFF);
    size_t off = isEx ? 26 : 18;
    off = skipSz(off).first;
    off = skipSz(off).first;
    std::wstring cap = skipSz(off).second;
    if (!cap.empty()) {
        // Strip Win32 mnemonic "&A" suffix cleanly if present in tab label
        size_t amp = cap.find(L"(&");
        if (amp != std::wstring::npos) cap.erase(amp);
        cap.erase(std::remove(cap.begin(), cap.end(), L'&'), cap.end());
        return cap;
    }
    return fallback;
}

HPROPSHEETPAGE RKLangManager::CreatePage(UINT dlgId, DLGPROC proc, LPARAM initParam, const std::wstring& titleFallback) {
    const DLGTEMPLATE* tmpl = FindDialogTemplate(dlgId, nullptr);
    std::wstring title = ExtractDialogCaption(tmpl, LoadStr(dlgId, titleFallback.c_str()));
    PROPSHEETPAGEW psp{};
    psp.dwSize      = sizeof(psp);
    psp.hInstance   = GetModuleHandleW(nullptr);
    psp.pfnDlgProc  = proc;
    psp.lParam      = initParam;
    psp.pszTitle    = _wcsdup(title.c_str());
    psp.dwFlags     = PSP_USETITLE;

    if (tmpl) {
        psp.dwFlags   |= PSP_DLGINDIRECT;
        psp.pResource  = tmpl;
    } else {
        psp.pszTemplate = MAKEINTRESOURCEW(dlgId);
    }
    return CreatePropertySheetPageW(&psp);
}

std::vector<std::wstring> RKLangManager::EnumerateLanguages() {
    std::vector<std::wstring> out;
    std::wstring langDir = ResolveAppPath(L"lang");
    if (DirExists(langDir)) {
        WIN32_FIND_DATAW fd{};
        HANDLE hf = FindFirstFileW((langDir + L"\\*.lang").c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    out.emplace_back(fd.cFileName);
                }
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ============================================================================
// Context Menu (Menu 130 / 0x82)
// ============================================================================

static void UpdatePositionSubmenuChecks(HMENU hSub, const RKDockOptions& opts) {
    if (!hSub) return;
    // Exact index 0..11 -> menu command ID
    static constexpr UINT kPosToCmd[12] = {
        IDM_POS_TOP_LEFT,     IDM_POS_TOP_CENTER,    IDM_POS_TOP_RIGHT,
        IDM_POS_BOTTOM_LEFT,  IDM_POS_BOTTOM_CENTER, IDM_POS_BOTTOM_RIGHT,
        IDM_POS_LEFT_TOP,     IDM_POS_LEFT_CENTER,   IDM_POS_LEFT_BOTTOM,
        IDM_POS_RIGHT_TOP,    IDM_POS_RIGHT_CENTER,  IDM_POS_RIGHT_BOTTOM,
    };
    if (opts.position >= 0 && opts.position < 12) {
        CheckMenuRadioItem(hSub, IDM_POS_BOTTOM_LEFT, IDM_POS_TOP_RIGHT,
                           kPosToCmd[opts.position], MF_BYCOMMAND);
    }
}

void ShowDockContextMenu(HWND hwndDock, POINT screenPt, const RKDockOptions& opts,
                         RKDockItem* hitItem, bool isSeparatorHit)
{
    HMENU hRoot = RKLangManager::Instance().LoadMenuTemplate(IDR_MAINMENU);
    HMENU hDockSub = hRoot ? GetSubMenu(hRoot, 0) : nullptr;
    HMENU hItemSub = hRoot ? GetSubMenu(hRoot, 1) : nullptr;

    HMENU hPopup = CreatePopupMenu();
    bool appendedItemSection = false;

    auto getMenuStr = [](HMENU subA, HMENU subB, UINT id, const wchar_t* defText) -> std::wstring {
        wchar_t buf[128] = {};
        if (subA && GetMenuStringW(subA, id, buf, 128, MF_BYCOMMAND) > 0 && *buf) return buf;
        if (subB && GetMenuStringW(subB, id, buf, 128, MF_BYCOMMAND) > 0 && *buf) return buf;
        return defText;
    };

    if (hitItem) {
        if (isSeparatorHit) {
            AppendMenuW(hPopup, MF_STRING, IDM_DOCK_DEL_SEPARATOR,
                        getMenuStr(hDockSub, hItemSub, IDM_DOCK_DEL_SEPARATOR, L"Remove Separator").c_str());
            appendedItemSection = true;
        } else if (hitItem->Type() == DockItemType::Window) {
            AppendMenuW(hPopup, MF_STRING, IDM_ITEM_CLOSE_WINDOW,
                        getMenuStr(hItemSub, hDockSub, IDM_ITEM_CLOSE_WINDOW, L"Close Window").c_str());
            appendedItemSection = true;
        } else {
            AppendMenuW(hPopup, MF_STRING, IDM_ITEM_PROPERTIES,
                        getMenuStr(hItemSub, hDockSub, IDM_ITEM_PROPERTIES, L"Dock Item Properties...").c_str());
            AppendMenuW(hPopup, MF_STRING, IDM_ITEM_REMOVE,
                        getMenuStr(hItemSub, hDockSub, IDM_ITEM_REMOVE, L"Remove from Dock").c_str());
            appendedItemSection = true;
        }
    }

    if (hDockSub && (!appendedItemSection || opts.showSettingsAllItems || isSeparatorHit)) {
        if (appendedItemSection) AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
        int cnt = GetMenuItemCount(hDockSub);
        for (int i = 0; i < cnt; ++i) {
            MENUITEMINFOW mii{};
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_STRING | MIIM_SUBMENU;
            wchar_t txt[128] = {};
            mii.dwTypeData = txt;
            mii.cch = 128;
            if (GetMenuItemInfoW(hDockSub, i, TRUE, &mii)) {
                if (mii.hSubMenu) UpdatePositionSubmenuChecks(mii.hSubMenu, opts);
                InsertMenuItemW(hPopup, GetMenuItemCount(hPopup), TRUE, &mii);
            }
        }
    } else if (appendedItemSection && hDockSub) {
        // Even when showSettingsAllItems == 0 and the dock background is transparent,
        // give the user convenient access to Position / Always on Top / Settings.
        AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
        int cnt = GetMenuItemCount(hDockSub);
        for (int i = 0; i < cnt; ++i) {
            MENUITEMINFOW mii{};
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_STRING | MIIM_SUBMENU;
            wchar_t txt[128] = {};
            mii.dwTypeData = txt;
            mii.cch = 128;
            if (GetMenuItemInfoW(hDockSub, i, TRUE, &mii)) {
                if (mii.hSubMenu || mii.wID == IDM_DOCK_MAGNIFY ||
                    mii.wID == IDM_DOCK_ALWAYSONTOP || mii.wID == IDM_DOCK_AUTOHIDE ||
                    mii.wID == IDM_DOCK_SETTINGS || mii.wID == IDM_DOCK_QUIT) {
                    if (mii.hSubMenu) UpdatePositionSubmenuChecks(mii.hSubMenu, opts);
                    InsertMenuItemW(hPopup, GetMenuItemCount(hPopup), TRUE, &mii);
                }
            }
        }
    }

    CheckMenuItem(hPopup, IDM_DOCK_MAGNIFY,
                  MF_BYCOMMAND | (opts.magnification ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hPopup, IDM_DOCK_ALWAYSONTOP,
                  MF_BYCOMMAND | (opts.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hPopup, IDM_DOCK_LOCKITEMS,
                  MF_BYCOMMAND | (opts.locked ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hPopup, IDM_DOCK_AUTOHIDE,
                  MF_BYCOMMAND | (opts.automaticHide ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hPopup, IDM_DOCK_MINIMIZE_TO_DOCK,
                  MF_BYCOMMAND | (opts.minimizeToDock ? MF_CHECKED : MF_UNCHECKED));

    SetForegroundWindow(hwndDock);
    UINT cmd = TrackPopupMenu(hPopup, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                              screenPt.x, screenPt.y, 0, hwndDock, nullptr);
    DestroyMenu(hPopup);
    if (hRoot) DestroyMenu(hRoot);
    if (cmd) {
        PostMessageW(hwndDock, WM_COMMAND, cmd, 0);
    }
}

// ============================================================================
// Controls helpers supporting both ListBox and SysListView32 seamlessly
// ============================================================================

static void InitListViewSingleColumn(HWND hCtrl) {
    if (!hCtrl) return;
    wchar_t cls[64] = {};
    GetClassNameW(hCtrl, cls, 64);
    if (_wcsicmp(cls, L"SysListView32") == 0) {
        RECT rc{};
        GetClientRect(hCtrl, &rc);
        LVCOLUMNW lvc{};
        lvc.mask = LVCF_WIDTH;
        lvc.cx = std::max(120, static_cast<int>(rc.right - rc.left) - 24);
        ListView_InsertColumn(hCtrl, 0, &lvc);
    }
}

static void AddListStringAny(HWND hCtrl, const std::wstring& text) {
    if (!hCtrl) return;
    wchar_t cls[64] = {};
    GetClassNameW(hCtrl, cls, 64);
    if (_wcsicmp(cls, L"SysListView32") == 0) {
        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = ListView_GetItemCount(hCtrl);
        lvi.pszText = const_cast<wchar_t*>(text.c_str());
        ListView_InsertItem(hCtrl, &lvi);
    } else {
        SendMessageW(hCtrl, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
}

static int GetListSelIndexAny(HWND hCtrl) {
    if (!hCtrl) return -1;
    wchar_t cls[64] = {};
    GetClassNameW(hCtrl, cls, 64);
    if (_wcsicmp(cls, L"SysListView32") == 0) {
        return ListView_GetNextItem(hCtrl, -1, LVNI_SELECTED);
    }
    return static_cast<int>(SendMessageW(hCtrl, LB_GETCURSEL, 0, 0));
}

static void ClearListAny(HWND hCtrl) {
    if (!hCtrl) return;
    wchar_t cls[64] = {};
    GetClassNameW(hCtrl, cls, 64);
    if (_wcsicmp(cls, L"SysListView32") == 0) {
        ListView_DeleteAllItems(hCtrl);
    } else {
        SendMessageW(hCtrl, LB_RESETCONTENT, 0, 0);
    }
}

static void RemoveListItemAny(HWND hCtrl, int idx) {
    if (!hCtrl || idx < 0) return;
    wchar_t cls[64] = {};
    GetClassNameW(hCtrl, cls, 64);
    if (_wcsicmp(cls, L"SysListView32") == 0) {
        ListView_DeleteItem(hCtrl, idx);
    } else {
        SendMessageW(hCtrl, LB_DELETESTRING, idx, 0);
    }
}

// ============================================================================
// About Dialog (IDD 100)
// ============================================================================

static INT_PTR CALLBACK AboutDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(hwnd, IDC_ABOUT_VERSION, L"RK Launcher 0.41 Beta (x64 Build 282)");
        SetDlgItemTextW(hwnd, IDC_ABOUT_CREDITS,
                        L"Original Copyright (C) 2004-2006 RaduKing.\r\n"
                        L"Native x64 Port (C++17 / GDI+ / ObjectDock x64).");
        SetDlgItemTextW(hwnd, IDC_ABOUT_HOMEPAGE, L"http://home.cogeco.ca/~rklauncher/");
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_ABOUT_HOMEPAGE) {
            ShellExecuteW(hwnd, L"open", L"http://home.cogeco.ca/~rklauncher/", nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hwnd, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowAboutDialog(HWND hwndParent) {
    RKLangManager::Instance().RunModalDialog(IDD_ABOUT, hwndParent, AboutDlgProc);
}

// ============================================================================
// Image Load Dialog (IDD 101)
// ============================================================================

struct ImageLoadCtx {
    std::wstring path;
    std::vector<std::wstring> files;
};

static void EnumIconsInDir(const std::wstring& dir, std::vector<std::wstring>& out) {
    if (!DirExists(dir)) return;
    WIN32_FIND_DATAW fd{};
    HANDLE hf = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring ext = ToLower(PathFindExtensionW(fd.cFileName));
        if (ext == L".png" || ext == L".ico" || ext == L".jpg" || ext == L".bmp") {
            out.emplace_back(fd.cFileName);
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    std::sort(out.begin(), out.end());
}

static INT_PTR CALLBACK ImageLoadDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ImageLoadCtx* ctx = reinterpret_cast<ImageLoadCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = reinterpret_cast<ImageLoadCtx*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        if (ctx) SetDlgItemTextW(hwnd, IDC_IMG_PATH, ctx->path.c_str());

        HWND hList = GetDlgItem(hwnd, IDC_IMG_LIST);
        if (hList && ctx) {
            InitListViewSingleColumn(hList);
            EnumIconsInDir(ResolveAppPath(L"icons"), ctx->files);
            for (const auto& f : ctx->files) AddListStringAny(hList, f);
        }
        return TRUE;
    }
    case WM_NOTIFY:
        if (ctx && reinterpret_cast<NMHDR*>(lParam)->idFrom == IDC_IMG_LIST &&
            reinterpret_cast<NMHDR*>(lParam)->code == LVN_ITEMCHANGED) {
            int sel = GetListSelIndexAny(GetDlgItem(hwnd, IDC_IMG_LIST));
            if (sel >= 0 && sel < static_cast<int>(ctx->files.size())) {
                SetDlgItemTextW(hwnd, IDC_IMG_PATH, (L"icons/" + ctx->files[sel]).c_str());
            }
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_IMG_BROWSE: {
            wchar_t file[MAX_PATH] = {};
            GetDlgItemTextW(hwnd, IDC_IMG_PATH, file, MAX_PATH);
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Images (*.png;*.ico;*.jpg;*.bmp)\0*.png;*.ico;*.jpg;*.bmp\0Executables (*.exe;*.dll)\0*.exe;*.dll\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            std::wstring iconsDir = ResolveAppPath(L"icons");
            if (DirExists(iconsDir)) ofn.lpstrInitialDir = iconsDir.c_str();
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetDlgItemTextW(hwnd, IDC_IMG_PATH, MakeRelativeIfInsideAppDir(file).c_str());
            }
            return TRUE;
        }
        case IDOK:
            if (ctx) {
                wchar_t buf[MAX_PATH] = {};
                GetDlgItemTextW(hwnd, IDC_IMG_PATH, buf, MAX_PATH);
                ctx->path = buf;
            }
            EndDialog(hwnd, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool ShowImageLoadDialog(HWND hwndParent, std::wstring& inOutImagePath) {
    ImageLoadCtx ctx{ inOutImagePath, {} };
    if (RKLangManager::Instance().RunModalDialog(IDD_IMAGE_LOAD, hwndParent, ImageLoadDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK) {
        inOutImagePath = ctx.path;
        return true;
    }
    return false;
}

// ============================================================================
// Language Dialog (IDD 103)
// ============================================================================

struct LangDlgCtx {
    std::vector<std::wstring> files;
    std::wstring selected;
};

static INT_PTR CALLBACK LanguageDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LangDlgCtx* ctx = reinterpret_cast<LangDlgCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = reinterpret_cast<LangDlgCtx*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        if (!ctx) return TRUE;
        ctx->files = RKLangManager::EnumerateLanguages();
        HWND hList = GetDlgItem(hwnd, IDC_LANG_LIST);
        InitListViewSingleColumn(hList);
        AddListStringAny(hList, L"<Built-in English (x64)>");
        for (const auto& f : ctx->files) {
            AddListStringAny(hList, f);
        }
        SetDlgItemTextW(hwnd, IDC_LANG_DESC, L"Select a .lang file from lang\\ or use Built-in English.");
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK && ctx) {
            int sel = GetListSelIndexAny(GetDlgItem(hwnd, IDC_LANG_LIST));
            if (sel <= 0) ctx->selected = L"";
            else if (sel - 1 < static_cast<int>(ctx->files.size())) ctx->selected = ctx->files[sel - 1];
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool ShowLanguageDialog(HWND hwndDock, RKDockOptions& opts) {
    LangDlgCtx ctx;
    ctx.selected = opts.langResource;
    if (RKLangManager::Instance().RunModalDialog(IDD_LANGUAGE, hwndDock, LanguageDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK) {
        opts.langResource = ctx.selected;
        return true;
    }
    return false;
}

// ============================================================================
// Item Properties Dialog (IDD 136)
// ============================================================================

struct ItemPropsCtx {
    RKDockItem* item = nullptr;
    std::wstring tempIconPath;
};

static INT_PTR CALLBACK ItemPropsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ItemPropsCtx* ctx = reinterpret_cast<ItemPropsCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = reinterpret_cast<ItemPropsCtx*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        if (!ctx || !ctx->item) return TRUE;

        SetDlgItemTextW(hwnd, IDC_PROP_NAME, ctx->item->Label().c_str());
        ctx->tempIconPath = ctx->item->IconPath();
        CheckDlgButton(hwnd, IDC_PROP_CUSTOM_ICON, !ctx->tempIconPath.empty() ? BST_CHECKED : BST_UNCHECKED);

        HWND hShow = GetDlgItem(hwnd, IDC_PROP_SHOW);
        if (hShow) {
            SendMessageW(hShow, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Normal"));
            SendMessageW(hShow, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Minimized"));
            SendMessageW(hShow, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Maximized"));
            SendMessageW(hShow, CB_SETCURSEL, 0, 0);
        }

        if (auto* sc = dynamic_cast<RKDockItemShortcut*>(ctx->item)) {
            SetDlgItemTextW(hwnd, IDC_PROP_LINK, sc->link_.c_str());
            SetDlgItemTextW(hwnd, IDC_PROP_ARGS, sc->arguments_.c_str());
            SetDlgItemTextW(hwnd, IDC_PROP_WORKDIR, sc->workFolder_.c_str());
            CheckDlgButton(hwnd, IDC_PROP_SAME_INSTANCE, sc->sameInstance_ ? BST_CHECKED : BST_UNCHECKED);
            if (hShow) {
                int sel = 0;
                if (sc->showCmd_ == L"minimized") sel = 1;
                else if (sc->showCmd_ == L"maximized") sel = 2;
                SendMessageW(hShow, CB_SETCURSEL, sel, 0);
            }
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_PROP_CHANGE_ICON:
            if (ctx) {
                if (ShowImageLoadDialog(hwnd, ctx->tempIconPath)) {
                    CheckDlgButton(hwnd, IDC_PROP_CUSTOM_ICON, !ctx->tempIconPath.empty() ? BST_CHECKED : BST_UNCHECKED);
                }
            }
            return TRUE;
        case IDC_PROP_BROWSE_LINK: {
            wchar_t file[MAX_PATH] = {};
            GetDlgItemTextW(hwnd, IDC_PROP_LINK, file, MAX_PATH);
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Programs (*.exe;*.lnk;*.bat;*.cmd)\0*.exe;*.lnk;*.bat;*.cmd\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetDlgItemTextW(hwnd, IDC_PROP_LINK, file);
            }
            return TRUE;
        }
        case IDOK:
            if (ctx && ctx->item) {
                wchar_t buf[512] = {};
                GetDlgItemTextW(hwnd, IDC_PROP_NAME, buf, 512);
                ctx->item->SetLabel(buf);
                if (IsDlgButtonChecked(hwnd, IDC_PROP_CUSTOM_ICON) == BST_UNCHECKED) {
                    ctx->item->SetIconPath(L"");
                } else {
                    ctx->item->SetIconPath(ctx->tempIconPath);
                }

                if (auto* sc = dynamic_cast<RKDockItemShortcut*>(ctx->item)) {
                    GetDlgItemTextW(hwnd, IDC_PROP_LINK, buf, 512);
                    const bool linkChanged = (sc->link_ != buf);
                    sc->link_ = buf;
                    GetDlgItemTextW(hwnd, IDC_PROP_ARGS, buf, 512);
                    sc->arguments_ = buf;
                    GetDlgItemTextW(hwnd, IDC_PROP_WORKDIR, buf, 512);
                    sc->workFolder_ = buf;
                    sc->sameInstance_ = (IsDlgButtonChecked(hwnd, IDC_PROP_SAME_INSTANCE) == BST_CHECKED);
                    int sel = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_PROP_SHOW, CB_GETCURSEL, 0, 0));
                    sc->showCmd_ = (sel == 1) ? L"minimized" : ((sel == 2) ? L"maximized" : L"normal");
                    if (linkChanged) {
                        sc->ReloadIcon();
                        sc->RefreshRuntimeState();
                    }
                }
            }
            EndDialog(hwnd, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool ShowItemPropertiesDialog(HWND hwndDock, RKDockItem* item) {
    if (!item) return false;
    ItemPropsCtx ctx{ item, item->IconPath() };
    return RKLangManager::Instance().RunModalDialog(IDD_ITEM_PROPERTIES, hwndDock, ItemPropsDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
}

// ============================================================================
// Add Docklet Dialog (IDD 139) - x64 native ObjectDock plugins only
// ============================================================================

struct DiscoveredDocklet {
    std::wstring relPath;
    std::wstring name;
    std::wstring author;
    std::wstring version;
    std::wstring notes;
};

static void ScanDockletsDir(const std::wstring& dir, const std::wstring& prefix,
                            std::vector<DiscoveredDocklet>& out, int depth)
{
    if (depth > 2) return;
    WIN32_FIND_DATAW fd{};
    HANDLE hf = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        std::wstring rel  = prefix + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDockletsDir(full, rel + L"/", out, depth + 1);
        } else {
            std::wstring ext = ToLower(PathFindExtensionW(fd.cFileName));
            if (ext == L".dll" && IsPeMachineX64(full)) {
                DiscoveredDocklet d;
                d.relPath = L"docklets/" + rel;
                d.name = fd.cFileName;
                HMODULE hm = LoadLibraryW(full.c_str());
                if (hm) {
                    using FnInfo = void (__stdcall*)(char*, char*, int*, char*);
                    auto fn = reinterpret_cast<FnInfo>(GetProcAddress(hm, "OnGetInformation"));
                    if (fn) {
                        char n[256] = {}, a[256] = {}, nt[512] = {};
                        int v = 0;
                        fn(n, a, &v, nt);
                        if (*n) d.name = Utf8ToWide(n);
                        if (*a) d.author = Utf8ToWide(a);
                        d.version = std::to_wstring(v);
                        if (*nt) d.notes = Utf8ToWide(nt);
                    }
                    FreeLibrary(hm);
                }
                out.push_back(std::move(d));
            }
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
}

struct AddDockletCtx {
    std::vector<DiscoveredDocklet> found;
    std::wstring outPath;
    std::wstring outLabel;
};

static INT_PTR CALLBACK AddDockletDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AddDockletCtx* ctx = reinterpret_cast<AddDockletCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = reinterpret_cast<AddDockletCtx*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        if (!ctx) return TRUE;
        HWND hType = GetDlgItem(hwnd, IDC_DOCKLET_TYPE);
        if (hType) {
            SendMessageW(hType, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"ObjectDock (x64)"));
            SendMessageW(hType, CB_SETCURSEL, 0, 0);
        }
        std::wstring root = ResolveAppPath(L"docklets");
        if (DirExists(root)) ScanDockletsDir(root, L"", ctx->found, 0);
        HWND hList = GetDlgItem(hwnd, IDC_DOCKLET_LIST);
        InitListViewSingleColumn(hList);
        for (const auto& d : ctx->found) {
            AddListStringAny(hList, d.name + L" (" + d.relPath + L")");
        }
        return TRUE;
    }
    case WM_NOTIFY:
        if (ctx && reinterpret_cast<NMHDR*>(lParam)->idFrom == IDC_DOCKLET_LIST &&
            reinterpret_cast<NMHDR*>(lParam)->code == LVN_ITEMCHANGED) {
            int sel = GetListSelIndexAny(GetDlgItem(hwnd, IDC_DOCKLET_LIST));
            if (sel >= 0 && sel < static_cast<int>(ctx->found.size())) {
                const auto& d = ctx->found[sel];
                SetDlgItemTextW(hwnd, IDC_DOCKLET_PATH, d.relPath.c_str());
                SetDlgItemTextW(hwnd, IDC_DOCKLET_AUTHOR, d.author.c_str());
                SetDlgItemTextW(hwnd, IDC_DOCKLET_VERSION, d.version.c_str());
                SetDlgItemTextW(hwnd, IDC_DOCKLET_NOTES, d.notes.c_str());
            }
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_DOCKLET_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE && ctx) {
                int sel = GetListSelIndexAny(GetDlgItem(hwnd, IDC_DOCKLET_LIST));
                if (sel >= 0 && sel < static_cast<int>(ctx->found.size())) {
                    const auto& d = ctx->found[sel];
                    SetDlgItemTextW(hwnd, IDC_DOCKLET_PATH, d.relPath.c_str());
                    SetDlgItemTextW(hwnd, IDC_DOCKLET_AUTHOR, d.author.c_str());
                    SetDlgItemTextW(hwnd, IDC_DOCKLET_VERSION, d.version.c_str());
                    SetDlgItemTextW(hwnd, IDC_DOCKLET_NOTES, d.notes.c_str());
                }
            }
            return TRUE;
        case IDC_DOCKLET_ADD_BTN:
            if (ctx) {
                wchar_t cur[MAX_PATH] = {};
                GetDlgItemTextW(hwnd, IDC_DOCKLET_PATH, cur, MAX_PATH);
                if (*cur) {
                    ctx->outPath = cur;
                    int sel = GetListSelIndexAny(GetDlgItem(hwnd, IDC_DOCKLET_LIST));
                    if (sel >= 0 && sel < static_cast<int>(ctx->found.size())) ctx->outLabel = ctx->found[sel].name;
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }
                wchar_t file[MAX_PATH] = {};
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"x64 Docklet DLLs (*.dll)\0*.dll\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    if (!IsPeMachineX64(file)) {
                        MessageBoxW(hwnd, L"Selected DLL is 32-bit or invalid. Only native x64 ObjectDock DLLs are supported.",
                                    L"RK Launcher x64", MB_ICONWARNING);
                    } else {
                        SetDlgItemTextW(hwnd, IDC_DOCKLET_PATH, MakeRelativeIfInsideAppDir(file).c_str());
                    }
                }
            }
            return TRUE;
        case IDOK:
            if (ctx) {
                wchar_t buf[MAX_PATH] = {};
                GetDlgItemTextW(hwnd, IDC_DOCKLET_PATH, buf, MAX_PATH);
                ctx->outPath = buf;
                int sel = GetListSelIndexAny(GetDlgItem(hwnd, IDC_DOCKLET_LIST));
                if (sel >= 0 && sel < static_cast<int>(ctx->found.size())) {
                    ctx->outLabel = ctx->found[sel].name;
                }
            }
            EndDialog(hwnd, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool ShowAddDockletDialog(HWND hwndDock, std::wstring& outDllRelPath, std::wstring& outLabel) {
    AddDockletCtx ctx;
    if (RKLangManager::Instance().RunModalDialog(IDD_ADD_DOCKLET, hwndDock, AddDockletDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK) {
        outDllRelPath = ctx.outPath;
        outLabel = ctx.outLabel;
        return !outDllRelPath.empty();
    }
    return false;
}

// ============================================================================
// Settings 7-Tab Property Sheet (131, 132, 133, 135, 140, 147, 141)
// ============================================================================

struct SettingsCtx {
    RKDockOptions opts;
    bool applied = false;
    bool committedViaApply = false;
    SettingsApplyFn onApply = nullptr;
    void* onApplyCtx = nullptr;
    HWND hwndSheet = nullptr;
    WNDPROC origSheetProc = nullptr;
    std::vector<std::wstring> themes;
    int excTypeSel = 0;
    COLORREF lblColorLabel  = RGB(255, 255, 255);
    COLORREF lblColorEdge   = RGB(0, 0, 0);
    COLORREF lblColorShadow = RGB(0, 0, 0);
    bool lblInitColors = false;
};

static const wchar_t* const kSettingsSheetCtxProp = L"RKLSettingsCtx";

static LRESULT CALLBACK SettingsSheetSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<SettingsCtx*>(GetPropW(hwnd, kSettingsSheetCtxProp));
    WNDPROC origProc = ctx ? ctx->origSheetProc : nullptr;
    if (!origProc) return DefWindowProcW(hwnd, msg, wParam, lParam);

    if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, kSettingsSheetCtxProp);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(origProc));
        ctx->hwndSheet = nullptr;
        ctx->origSheetProc = nullptr;
        return CallWindowProcW(origProc, hwnd, msg, wParam, lParam);
    }

    if (msg == WM_COMMAND && LOWORD(wParam) == 0x3021 /* ID_APPLY_NOW */ &&
        (HIWORD(wParam) == 0 || HIWORD(wParam) == 1)) {
        ctx->applied = false;
        LRESULT res = CallWindowProcW(origProc, hwnd, msg, wParam, lParam);
        if (ctx->applied && ctx->onApply) {
            ctx->opts.ClampValid();
            ctx->onApply(ctx->opts, ctx->onApplyCtx);
            ctx->committedViaApply = true;
            ctx->applied = false;
        }
        return res;
    }

    return CallWindowProcW(origProc, hwnd, msg, wParam, lParam);
}

static SettingsCtx* GetSheetCtx(HWND hwndPage, LPARAM lParam, UINT msg) {
    if (msg == WM_INITDIALOG) {
        auto* psp = reinterpret_cast<LPPROPSHEETPAGEW>(lParam);
        auto* ctx = reinterpret_cast<SettingsCtx*>(psp->lParam);
        SetWindowLongPtrW(hwndPage, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        if (ctx && !ctx->hwndSheet) {
            HWND hSheet = GetParent(hwndPage);
            if (hSheet && IsWindow(hSheet)) {
                ctx->hwndSheet = hSheet;
                SetPropW(hSheet, kSettingsSheetCtxProp, ctx);
                ctx->origSheetProc = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(hSheet, GWLP_WNDPROC,
                                      reinterpret_cast<LONG_PTR>(SettingsSheetSubclassProc)));
            }
        }
        return ctx;
    }
    return reinterpret_cast<SettingsCtx*>(GetWindowLongPtrW(hwndPage, GWLP_USERDATA));
}

static void SetupSlider(HWND hwnd, UINT idSlider, UINT idVal, int minV, int maxV, int curV) {
    HWND hs = GetDlgItem(hwnd, idSlider);
    if (hs) {
        SendMessageW(hs, TBM_SETRANGE, TRUE, MAKELONG(minV, maxV));
        SendMessageW(hs, TBM_SETPOS, TRUE, curV);
    }
    if (idVal && GetDlgItem(hwnd, idVal)) SetDlgItemInt(hwnd, idVal, curV, FALSE);
}

static INT_PTR CALLBACK PageAppearanceProc(HWND hwnd, UINT msg, WPARAM /*wParam*/, LPARAM lParam) {
    SettingsCtx* ctx = GetSheetCtx(hwnd, lParam, msg);
    if (!ctx) return FALSE;
    switch (msg) {
    case WM_INITDIALOG: {
        ctx->themes = RKDockTheme::EnumerateThemes();
        HWND hCb = GetDlgItem(hwnd, IDC_APP_THEME_COMBO);
        if (hCb) {
            int sel = 0;
            for (size_t i = 0; i < ctx->themes.size(); ++i) {
                SendMessageW(hCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ctx->themes[i].c_str()));
                if (_wcsicmp(ctx->themes[i].c_str(), ctx->opts.theme.c_str()) == 0) sel = static_cast<int>(i);
            }
            SendMessageW(hCb, CB_SETCURSEL, sel, 0);
        }

        SetupSlider(hwnd, IDC_APP_SMALL_SLIDER, IDC_APP_SMALL_VAL, 16, 128, ctx->opts.smallSize);
        SetupSlider(hwnd, IDC_APP_MAG_SLIDER,   IDC_APP_MAG_VAL,   16, 192, ctx->opts.magnifySize);
        SetupSlider(hwnd, IDC_APP_ALPHA_SLIDER, 0,                 0,  255, ctx->opts.backgroundAlpha);
        SetupSlider(hwnd, IDC_APP_DIST_SLIDER,  IDC_APP_DIST_VAL,  0,  32,  ctx->opts.itemDistance);

        SetDlgItemInt(hwnd, IDC_APP_SPAN_EDIT, ctx->opts.itemsMagnify, FALSE);
        SendDlgItemMessageW(hwnd, 1026, UDM_SETRANGE32, 3, 25);
        CheckDlgButton(hwnd, IDC_APP_MAG_CHECK, ctx->opts.magnification ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_APP_HIDE_IND,  ctx->opts.hideIndicators ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_APP_HIDE_POOF, ctx->opts.hidePoofs ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    case WM_HSCROLL:
        if (GetDlgItem(hwnd, IDC_APP_SMALL_VAL)) SetDlgItemInt(hwnd, IDC_APP_SMALL_VAL, static_cast<UINT>(SendDlgItemMessageW(hwnd, IDC_APP_SMALL_SLIDER, TBM_GETPOS, 0, 0)), FALSE);
        if (GetDlgItem(hwnd, IDC_APP_MAG_VAL))   SetDlgItemInt(hwnd, IDC_APP_MAG_VAL,   static_cast<UINT>(SendDlgItemMessageW(hwnd, IDC_APP_MAG_SLIDER,   TBM_GETPOS, 0, 0)), FALSE);
        if (GetDlgItem(hwnd, IDC_APP_DIST_VAL))  SetDlgItemInt(hwnd, IDC_APP_DIST_VAL,  static_cast<UINT>(SendDlgItemMessageW(hwnd, IDC_APP_DIST_SLIDER,  TBM_GETPOS, 0, 0)), FALSE);
        PropSheet_Changed(GetParent(hwnd), hwnd);
        return TRUE;
    case WM_COMMAND:
        PropSheet_Changed(GetParent(hwnd), hwnd);
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->code == PSN_APPLY) {
            ctx->opts.smallSize       = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_APP_SMALL_SLIDER, TBM_GETPOS, 0, 0));
            ctx->opts.magnifySize     = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_APP_MAG_SLIDER,   TBM_GETPOS, 0, 0));
            ctx->opts.backgroundAlpha = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_APP_ALPHA_SLIDER, TBM_GETPOS, 0, 0));
            if (GetDlgItem(hwnd, IDC_APP_DIST_SLIDER)) {
                ctx->opts.itemDistance = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_APP_DIST_SLIDER, TBM_GETPOS, 0, 0));
            }
            BOOL okSpan = FALSE;
            int span = static_cast<int>(GetDlgItemInt(hwnd, IDC_APP_SPAN_EDIT, &okSpan, FALSE));
            if (okSpan && span >= 3) ctx->opts.itemsMagnify = span;
            ctx->opts.magnification   = (IsDlgButtonChecked(hwnd, IDC_APP_MAG_CHECK) == BST_CHECKED);
            ctx->opts.hideIndicators  = (IsDlgButtonChecked(hwnd, IDC_APP_HIDE_IND)  == BST_CHECKED);
            ctx->opts.hidePoofs       = (IsDlgButtonChecked(hwnd, IDC_APP_HIDE_POOF) == BST_CHECKED);
            int sel = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_APP_THEME_COMBO, CB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(ctx->themes.size())) {
                ctx->opts.theme = ctx->themes[sel];
            }
            ctx->applied = true;
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// Position IDs ordered by index 0..11 matching original .lang IDD 132 (1006..1017)
static constexpr UINT kPosRadios[12] = {
    IDC_POS_RAD_TL, IDC_POS_RAD_TC, IDC_POS_RAD_TR, // 0..2: Top
    IDC_POS_RAD_BL, IDC_POS_RAD_BC, IDC_POS_RAD_BR, // 3..5: Bottom
    IDC_POS_RAD_LT, IDC_POS_RAD_LC, IDC_POS_RAD_LB, // 6..8: Left
    IDC_POS_RAD_RT, IDC_POS_RAD_RC, IDC_POS_RAD_RB, // 9..11: Right
};

static INT_PTR CALLBACK PagePositionProc(HWND hwnd, UINT msg, WPARAM /*wParam*/, LPARAM lParam) {
    SettingsCtx* ctx = GetSheetCtx(hwnd, lParam, msg);
    if (!ctx) return FALSE;
    switch (msg) {
    case WM_INITDIALOG: {
        int pos = std::clamp(ctx->opts.position, 0, 11);
        CheckRadioButton(hwnd, IDC_POS_RAD_TL, IDC_POS_RAD_LB, kPosRadios[pos]);
        SetupSlider(hwnd, IDC_POS_MARGIN_SLIDER, IDC_POS_MARGIN_VAL, 0, 100, ctx->opts.positionMargin);
        CheckDlgButton(hwnd, IDC_POS_MULTIMON, ctx->opts.multiMonitorSupport ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    case WM_HSCROLL:
        if (GetDlgItem(hwnd, IDC_POS_MARGIN_VAL)) {
            SetDlgItemInt(hwnd, IDC_POS_MARGIN_VAL, static_cast<UINT>(SendDlgItemMessageW(hwnd, IDC_POS_MARGIN_SLIDER, TBM_GETPOS, 0, 0)), FALSE);
        }
        PropSheet_Changed(GetParent(hwnd), hwnd);
        return TRUE;
    case WM_COMMAND:
        PropSheet_Changed(GetParent(hwnd), hwnd);
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->code == PSN_APPLY) {
            for (int i = 0; i < 12; ++i) {
                if (IsDlgButtonChecked(hwnd, kPosRadios[i]) == BST_CHECKED) {
                    ctx->opts.position = i;
                    break;
                }
            }
            ctx->opts.positionMargin      = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_POS_MARGIN_SLIDER, TBM_GETPOS, 0, 0));
            ctx->opts.multiMonitorSupport = (IsDlgButtonChecked(hwnd, IDC_POS_MULTIMON) == BST_CHECKED) ? 1 : 0;
            ctx->applied = true;
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void ApplyStartupOptions(bool enable, int swwRegistry, int swwUser) {
    auto updateRunKey = [&](HKEY root) {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                          0, KEY_WRITE, &hk) == ERROR_SUCCESS) {
            if (enable && swwRegistry == 0) {
                wchar_t exe[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                std::wstring val = L"\"" + std::wstring(exe) + L"\"";
                RegSetValueExW(hk, L"RK Launcher", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(val.c_str()),
                               static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
            } else {
                RegDeleteValueW(hk, L"RK Launcher");
            }
            RegCloseKey(hk);
        }
    };
    updateRunKey(HKEY_CURRENT_USER);
    if (swwUser != 0) {
        updateRunKey(HKEY_LOCAL_MACHINE);
    }

    auto removeStartupLink = [&](int csidl) {
        wchar_t dir[MAX_PATH] = {};
        if (SHGetSpecialFolderPathW(nullptr, dir, csidl, FALSE)) {
            std::wstring lnk = std::wstring(dir) + L"\\RK Launcher.lnk";
            DeleteFileW(lnk.c_str());
        }
    };
    if (!enable || swwRegistry == 0) {
        removeStartupLink(CSIDL_STARTUP);
        removeStartupLink(CSIDL_COMMON_STARTUP);
    } else {
        int targetCsidl = (swwUser != 0) ? CSIDL_COMMON_STARTUP : CSIDL_STARTUP;
        int otherCsidl  = (swwUser != 0) ? CSIDL_STARTUP : CSIDL_COMMON_STARTUP;
        removeStartupLink(otherCsidl);
        wchar_t dir[MAX_PATH] = {};
        if (SHGetSpecialFolderPathW(nullptr, dir, targetCsidl, TRUE)) {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            IShellLinkW* psl = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_IShellLinkW, reinterpret_cast<void**>(&psl))) && psl) {
                psl->SetPath(exe);
                wchar_t workDir[MAX_PATH] = {};
                wcscpy_s(workDir, exe);
                PathRemoveFileSpecW(workDir);
                psl->SetWorkingDirectory(workDir);
                IPersistFile* ppf = nullptr;
                if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf))) && ppf) {
                    std::wstring lnk = std::wstring(dir) + L"\\RK Launcher.lnk";
                    ppf->Save(lnk.c_str(), TRUE);
                    ppf->Release();
                }
                psl->Release();
            }
        }
    }
}

static void PopulateEffectCombo(HWND hCombo, const std::wstring& selectedRel) {
    if (!hCombo) return;
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    auto* paths = new std::vector<std::wstring>();
    RemovePropW(hCombo, L"EffPaths");
    SetPropW(hCombo, L"EffPaths", paths);

    std::wstring pluginsRoot = ResolveAppPath(L"plugins");
    WIN32_FIND_DATAW dirFd{};
    HANDLE hDir = FindFirstFileW((pluginsRoot + L"\\*").c_str(), &dirFd);
    int selIdx = -1;
    std::wstring targetNorm = ToLower(NormalizeSlashes(selectedRel, L'/'));

    if (hDir != INVALID_HANDLE_VALUE) {
        do {
            if (!(dirFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(dirFd.cFileName, L".") == 0 || wcscmp(dirFd.cFileName, L"..") == 0) continue;
            std::wstring sub = pluginsRoot + L"\\" + dirFd.cFileName;
            WIN32_FIND_DATAW dllFd{};
            HANDLE hDll = FindFirstFileW((sub + L"\\*.dll").c_str(), &dllFd);
            if (hDll == INVALID_HANDLE_VALUE) continue;
            do {
                std::wstring full = sub + L"\\" + dllFd.cFileName;
                if (!IsPeMachineX64(full)) continue;
                HMODULE hm = LoadLibraryW(full.c_str());
                if (!hm) continue;
                using FnGetName = void (__stdcall*)(wchar_t*, int);
                auto fnName = reinterpret_cast<FnGetName>(GetProcAddress(hm, "EffectGetDisplayName"));
                if (fnName) {
                    wchar_t disp[256] = {};
                    fnName(disp, 256);
                    if (disp[0]) {
                        std::wstring rel = std::wstring(dirFd.cFileName) + L"/" + dllFd.cFileName;
                        int idx = static_cast<int>(SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(disp)));
                        if (idx >= 0) {
                            SendMessageW(hCombo, CB_SETITEMDATA, idx, static_cast<LPARAM>(paths->size()));
                            paths->push_back(rel);
                            if (ToLower(rel) == targetNorm) selIdx = idx;
                        }
                    }
                }
                FreeLibrary(hm);
            } while (FindNextFileW(hDll, &dllFd));
            FindClose(hDll);
        } while (FindNextFileW(hDir, &dirFd));
        FindClose(hDir);
    }

    if (selIdx >= 0) {
        SendMessageW(hCombo, CB_SETCURSEL, selIdx, 0);
    } else if (!paths->empty()) {
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    }
}

static void UpdateBehaviorEnableStates(HWND hwnd) {
    HWND hLv = GetDlgItem(hwnd, IDC_BEH_LIST);
    auto enableCtrl = [&](UINT id, bool on) {
        if (HWND h = GetDlgItem(hwnd, id)) EnableWindow(h, on ? TRUE : FALSE);
    };

    bool autoHideOn = (hLv && ListView_GetItemCount(hLv) > 1) ? (ListView_GetCheckState(hLv, 1) != FALSE) : false;
    bool hotspotOn  = (hLv && ListView_GetItemCount(hLv) > 8) ? (ListView_GetCheckState(hLv, 8) != FALSE) : false;
    bool swwOn      = (hLv && ListView_GetItemCount(hLv) > 11) ? (ListView_GetCheckState(hLv, 11) != FALSE) : false;

    enableCtrl(IDC_BEH_AUTOHIDE_LBL,   autoHideOn);
    enableCtrl(IDC_BEH_AUTOHIDE_DELAY, autoHideOn);
    enableCtrl(IDC_BEH_AUTOHIDE_SPIN,  autoHideOn);
    enableCtrl(IDC_BEH_AUTOHIDE_MS,    autoHideOn);

    enableCtrl(IDC_BEH_HOTSPOT_LBL,    hotspotOn);
    enableCtrl(IDC_BEH_HOTSPOT_DELAY,  hotspotOn);
    enableCtrl(IDC_BEH_HOTSPOT_SPIN,   hotspotOn);
    enableCtrl(IDC_BEH_HOTSPOT_MS,     hotspotOn);
    enableCtrl(IDC_BEH_HOTSPOT_FULL,   hotspotOn);

    enableCtrl(IDC_BEH_SWW_USER_LBL,   swwOn);
    enableCtrl(IDC_BEH_SWW_USER,       swwOn);
    enableCtrl(IDC_BEH_SWW_REG,        swwOn);
    enableCtrl(IDC_BEH_SWW_STARTUP,    swwOn);

    bool animOn  = (IsDlgButtonChecked(hwnd, IDC_BEH_ANIMATION) == BST_CHECKED);
    bool animNew = (IsDlgButtonChecked(hwnd, IDC_BEH_ANIM_NEW) == BST_CHECKED);
    enableCtrl(IDC_BEH_ANIM_COMBO,     animOn || animNew);
    enableCtrl(IDC_BEH_ANIM_COUNT_LBL, animOn || animNew);
    enableCtrl(IDC_BEH_ANIM_COUNT,     animOn || animNew);
    enableCtrl(IDC_BEH_ANIM_SPIN,      animOn || animNew);

    bool useEff = (IsDlgButtonChecked(hwnd, IDC_BEH_USE_EFFECT) == BST_CHECKED);
    enableCtrl(IDC_BEH_EFFECT_COMBO,   useEff);
}

static INT_PTR CALLBACK PageBehaviorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsCtx* ctx = GetSheetCtx(hwnd, lParam, msg);
    if (!ctx) return FALSE;
    switch (msg) {
    case WM_INITDIALOG: {
        auto& lm = RKLangManager::Instance();
        HWND hLv = GetDlgItem(hwnd, IDC_BEH_LIST);
        if (hLv) {
            ListView_SetExtendedListViewStyle(hLv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
            InitListViewSingleColumn(hLv);
            struct Row { UINT strId; const wchar_t* defTxt; bool val; } rows[13] = {
                { IDS_BEH_ALWAYSONTOP,    L"Always on Top",              ctx->opts.alwaysOnTop },
                { IDS_BEH_AUTOHIDE,       L"Automatic Hide",             ctx->opts.automaticHide },
                { IDS_BEH_LOCKED,         L"Lock add/remove/drag",       ctx->opts.locked },
                { IDS_BEH_MINIMIZETODOCK, L"Minimize windows to Dock",   ctx->opts.minimizeToDock },
                { IDS_BEH_SAVEONEXIT,     L"Save settings on exit",      ctx->opts.saveOnExit },
                { IDS_BEH_SAVEONCHANGE,   L"Save item settings on change",ctx->opts.saveOnChange },
                { IDS_BEH_SAVEITEMS,      L"Save dock items on exit",    ctx->opts.saveItems },
                { IDS_BEH_NEWAPPS,        L"Show new applications",      ctx->opts.newApplications },
                { IDS_BEH_HOTSPOT,        L"Use margin hotspot",         ctx->opts.useMarginHotspot },
                { IDS_BEH_HIDETASKBAR,    L"Hide Windows taskbar",       ctx->opts.hideTaskbar },
                { IDS_BEH_NEVERTAKEFOCUS, L"Never take focus",           ctx->opts.neverTakeFocus },
                { IDS_BEH_STARTWITHWIN,   L"Start with Windows",         ctx->opts.startWithWindows },
                { IDS_BEH_SHOWMINICON,    L"Show minimized window icon", ctx->opts.showMinimizedWndIcon },
            };
            SetPropW(hwnd, L"BehInit", reinterpret_cast<HANDLE>(1));
            for (int i = 0; i < 13; ++i) {
                std::wstring t = lm.LoadStr(rows[i].strId, rows[i].defTxt);
                LVITEMW lvi{};
                lvi.mask = LVIF_TEXT;
                lvi.iItem = i;
                lvi.pszText = const_cast<wchar_t*>(t.c_str());
                ListView_InsertItem(hLv, &lvi);
                ListView_SetCheckState(hLv, i, rows[i].val ? TRUE : FALSE);
            }
            RemovePropW(hwnd, L"BehInit");
        }
        if (HWND hUser = GetDlgItem(hwnd, IDC_BEH_SWW_USER)) {
            SendMessageW(hUser, CB_RESETCONTENT, 0, 0);
            std::wstring curUsr = lm.LoadStr(IDS_BEH_CURRENTUSER, L"Current User");
            std::wstring allUsr = lm.LoadStr(IDS_BEH_ALLUSERS, L"All Users");
            SendMessageW(hUser, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(curUsr.c_str()));
            SendMessageW(hUser, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(allUsr.c_str()));
            SendMessageW(hUser, CB_SETCURSEL, std::clamp(ctx->opts.swwUser, 0, 1), 0);
        }
        if (GetDlgItem(hwnd, IDC_BEH_SWW_REG) && GetDlgItem(hwnd, IDC_BEH_SWW_STARTUP)) {
            CheckRadioButton(hwnd, IDC_BEH_SWW_REG, IDC_BEH_SWW_STARTUP,
                             (ctx->opts.swwRegistry != 0) ? IDC_BEH_SWW_STARTUP : IDC_BEH_SWW_REG);
        }
        if (HWND hAnimCombo = GetDlgItem(hwnd, IDC_BEH_ANIM_COMBO)) {
            SendMessageW(hAnimCombo, CB_RESETCONTENT, 0, 0);
            std::wstring bounce = lm.LoadStr(IDS_BEH_BOUNCE, L"Bounce");
            SendMessageW(hAnimCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(bounce.c_str()));
            SendMessageW(hAnimCombo, CB_SETCURSEL, 0, 0);
        }
        CheckDlgButton(hwnd, IDC_BEH_ANIMATION,    ctx->opts.itemAnimation ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_BEH_ANIM_NEW,     ctx->opts.itemAnimationNew ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_BEH_USE_EFFECT,   ctx->opts.useMinMaxEffect ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_BEH_HOTSPOT_FULL, ctx->opts.hotspotFullMargin ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(hwnd, IDC_BEH_AUTOHIDE_DELAY, ctx->opts.autoHideDelay, FALSE);
        SetDlgItemInt(hwnd, IDC_BEH_HOTSPOT_DELAY,  ctx->opts.hotspotDelay, FALSE);
        SetDlgItemInt(hwnd, IDC_BEH_ANIM_COUNT,     ctx->opts.itemAnimationCount, FALSE);
        SendDlgItemMessageW(hwnd, IDC_BEH_ANIM_SPIN,     UDM_SETRANGE32, 1, 20);
        SendDlgItemMessageW(hwnd, IDC_BEH_HOTSPOT_SPIN,  UDM_SETRANGE32, 0, 5000);
        SendDlgItemMessageW(hwnd, IDC_BEH_AUTOHIDE_SPIN, UDM_SETRANGE32, 0, 5000);
        PopulateEffectCombo(GetDlgItem(hwnd, IDC_BEH_EFFECT_COMBO), ctx->opts.minMaxEffect);
        UpdateBehaviorEnableStates(hwnd);
        return TRUE;
    }
    case WM_DESTROY: {
        HWND hCombo = GetDlgItem(hwnd, IDC_BEH_EFFECT_COMBO);
        if (hCombo) {
            delete reinterpret_cast<std::vector<std::wstring>*>(GetPropW(hCombo, L"EffPaths"));
            RemovePropW(hCombo, L"EffPaths");
        }
        break;
    }
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        if (id == IDC_BEH_ANIMATION || id == IDC_BEH_ANIM_NEW || id == IDC_BEH_USE_EFFECT) {
            UpdateBehaviorEnableStates(hwnd);
        }
        PropSheet_Changed(GetParent(hwnd), hwnd);
        break;
    }
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lParam);
        if (nm->idFrom == IDC_BEH_LIST && nm->code == LVN_ITEMCHANGED && !GetPropW(hwnd, L"BehInit")) {
            auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
            if ((nmlv->uChanged & LVIF_STATE) && ((nmlv->uNewState ^ nmlv->uOldState) & LVIS_STATEIMAGEMASK)) {
                UpdateBehaviorEnableStates(hwnd);
                PropSheet_Changed(GetParent(hwnd), hwnd);
            }
        }
        if (nm->code == PSN_APPLY) {
            int newSwwReg  = (IsDlgButtonChecked(hwnd, IDC_BEH_SWW_STARTUP) == BST_CHECKED) ? 1 : 0;
            int newSwwUser = ctx->opts.swwUser;
            if (HWND hUser = GetDlgItem(hwnd, IDC_BEH_SWW_USER)) {
                int selU = static_cast<int>(SendMessageW(hUser, CB_GETCURSEL, 0, 0));
                if (selU >= 0) newSwwUser = std::clamp(selU, 0, 1);
            }

            HWND hLv = GetDlgItem(hwnd, IDC_BEH_LIST);
            if (hLv && ListView_GetItemCount(hLv) >= 13) {
                ctx->opts.alwaysOnTop          = ListView_GetCheckState(hLv, 0) != FALSE;
                ctx->opts.automaticHide        = ListView_GetCheckState(hLv, 1) != FALSE;
                ctx->opts.locked               = ListView_GetCheckState(hLv, 2) != FALSE;
                ctx->opts.minimizeToDock       = ListView_GetCheckState(hLv, 3) != FALSE;
                ctx->opts.saveOnExit           = ListView_GetCheckState(hLv, 4) != FALSE;
                ctx->opts.saveOnChange         = ListView_GetCheckState(hLv, 5) != FALSE;
                ctx->opts.saveItems            = ListView_GetCheckState(hLv, 6) != FALSE;
                ctx->opts.newApplications      = ListView_GetCheckState(hLv, 7) != FALSE;
                ctx->opts.useMarginHotspot     = ListView_GetCheckState(hLv, 8) != FALSE;
                ctx->opts.hideTaskbar          = ListView_GetCheckState(hLv, 9) != FALSE;
                ctx->opts.neverTakeFocus       = ListView_GetCheckState(hLv, 10) != FALSE;
                bool newSww                    = ListView_GetCheckState(hLv, 11) != FALSE;
                ctx->opts.showMinimizedWndIcon = ListView_GetCheckState(hLv, 12) != FALSE;
                if (newSww != ctx->opts.startWithWindows ||
                    newSwwReg != ctx->opts.swwRegistry ||
                    newSwwUser != ctx->opts.swwUser) {
                    ctx->opts.startWithWindows = newSww;
                    ctx->opts.swwRegistry      = newSwwReg;
                    ctx->opts.swwUser          = newSwwUser;
                    ApplyStartupOptions(newSww, newSwwReg, newSwwUser);
                }
            }
            if (GetDlgItem(hwnd, IDC_BEH_ANIMATION))    ctx->opts.itemAnimation    = (IsDlgButtonChecked(hwnd, IDC_BEH_ANIMATION) == BST_CHECKED);
            if (GetDlgItem(hwnd, IDC_BEH_ANIM_NEW))     ctx->opts.itemAnimationNew = (IsDlgButtonChecked(hwnd, IDC_BEH_ANIM_NEW) == BST_CHECKED) ? 1 : 0;
            if (GetDlgItem(hwnd, IDC_BEH_USE_EFFECT))   ctx->opts.useMinMaxEffect  = (IsDlgButtonChecked(hwnd, IDC_BEH_USE_EFFECT) == BST_CHECKED);
            if (GetDlgItem(hwnd, IDC_BEH_HOTSPOT_FULL)) ctx->opts.hotspotFullMargin= (IsDlgButtonChecked(hwnd, IDC_BEH_HOTSPOT_FULL) == BST_CHECKED);
            if (GetDlgItem(hwnd, IDC_BEH_AUTOHIDE_DELAY)) ctx->opts.autoHideDelay  = static_cast<int>(GetDlgItemInt(hwnd, IDC_BEH_AUTOHIDE_DELAY, nullptr, FALSE));
            if (GetDlgItem(hwnd, IDC_BEH_HOTSPOT_DELAY))  ctx->opts.hotspotDelay   = static_cast<int>(GetDlgItemInt(hwnd, IDC_BEH_HOTSPOT_DELAY, nullptr, FALSE));
            if (GetDlgItem(hwnd, IDC_BEH_ANIM_COUNT))     ctx->opts.itemAnimationCount = static_cast<int>(GetDlgItemInt(hwnd, IDC_BEH_ANIM_COUNT, nullptr, FALSE));
            if (HWND hCombo = GetDlgItem(hwnd, IDC_BEH_EFFECT_COMBO)) {
                int sel = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
                auto* paths = reinterpret_cast<std::vector<std::wstring>*>(GetPropW(hCombo, L"EffPaths"));
                if (sel >= 0 && paths) {
                    LRESULT idx = SendMessageW(hCombo, CB_GETITEMDATA, sel, 0);
                    if (idx >= 0 && static_cast<size_t>(idx) < paths->size()) {
                        ctx->opts.minMaxEffect = (*paths)[static_cast<size_t>(idx)];
                    }
                }
            }
            ctx->applied = true;
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static bool PickColor(HWND owner, COLORREF& color) {
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc{};
    cc.lStructSize  = sizeof(cc);
    cc.hwndOwner    = owner;
    cc.rgbResult    = color;
    cc.lpCustColors = custom;
    cc.Flags        = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) {
        color = cc.rgbResult;
        return true;
    }
    return false;
}

static int CALLBACK EnumFontCb(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
    HWND hCb = reinterpret_cast<HWND>(lp);
    if (lf->lfFaceName[0] != L'@' &&
        SendMessageW(hCb, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(lf->lfFaceName)) == CB_ERR) {
        SendMessageW(hCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lf->lfFaceName));
    }
    return 1;
}

static void DrawColorSwatchButton(const DRAWITEMSTRUCT* dis, COLORREF color) {
    if (!dis) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    // Outer combobox-like frame matching 32-bit swatch button
    HBRUSH bgBr = GetSysColorBrush(disabled ? COLOR_BTNFACE : COLOR_WINDOW);
    FillRect(hdc, &rc, bgBr);
    HPEN borderPen = CreatePen(PS_SOLID, 1, pressed ? RGB(60, 127, 177) : RGB(127, 157, 185));
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    HGDIOBJ oldBr  = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    // Right dropdown button strip (15px wide) with crisp downward triangle
    const int btnW = std::min(15, static_cast<int>((rc.right - rc.left) / 3));
    RECT btnRc{ rc.right - btnW - 1, rc.top + 1, rc.right - 1, rc.bottom - 1 };
    HBRUSH arrowBg = CreateSolidBrush(pressed ? RGB(205, 224, 240) : RGB(240, 240, 240));
    FillRect(hdc, &btnRc, arrowBg);
    DeleteObject(arrowBg);

    HPEN divPen = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
    oldPen = SelectObject(hdc, divPen);
    MoveToEx(hdc, btnRc.left, btnRc.top, nullptr);
    LineTo(hdc, btnRc.left, btnRc.bottom);
    SelectObject(hdc, oldPen);
    DeleteObject(divPen);

    int cx = (btnRc.left + btnRc.right) / 2 + (pressed ? 1 : 0);
    int cy = (btnRc.top + btnRc.bottom) / 2 + (pressed ? 1 : 0);
    POINT tri[3] = { { cx - 3, cy - 1 }, { cx + 4, cy - 1 }, { cx, cy + 3 } };
    HBRUSH triBr = CreateSolidBrush(disabled ? GetSysColor(COLOR_GRAYTEXT) : RGB(0, 0, 0));
    HPEN   triPen = CreatePen(PS_SOLID, 1, disabled ? GetSysColor(COLOR_GRAYTEXT) : RGB(0, 0, 0));
    oldPen = SelectObject(hdc, triPen);
    oldBr  = SelectObject(hdc, triBr);
    Polygon(hdc, tri, 3);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(triPen);
    DeleteObject(triBr);

    // Inner color swatch rectangle with crisp 1px black border
    RECT swRc{ rc.left + 3, rc.top + 3, btnRc.left - 3, rc.bottom - 3 };
    if (swRc.right > swRc.left + 2 && swRc.bottom > swRc.top + 2) {
        HBRUSH swBr = CreateSolidBrush(color);
        FillRect(hdc, &swRc, swBr);
        DeleteObject(swBr);
        HBRUSH frameBr = CreateSolidBrush(RGB(0, 0, 0));
        FrameRect(hdc, &swRc, frameBr);
        DeleteObject(frameBr);
    }

    if ((dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT)) {
        RECT focRc{ rc.left + 2, rc.top + 2, btnRc.left - 2, rc.bottom - 2 };
        DrawFocusRect(hdc, &focRc);
    }
}

static HBITMAP CreateColorMenuBitmap(HDC refDc, COLORREF color) {
    HDC mem = CreateCompatibleDC(refDc);
    HBITMAP bmp = CreateCompatibleBitmap(refDc, 12, 12);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT r{ 0, 0, 12, 12 };
    HBRUSH br = CreateSolidBrush(color);
    FillRect(mem, &r, br);
    DeleteObject(br);
    HBRUSH fr = CreateSolidBrush(RGB(64, 64, 64));
    FrameRect(mem, &r, fr);
    DeleteObject(fr);
    SelectObject(mem, old);
    DeleteDC(mem);
    return bmp;
}

static bool PickLabelColorMenu(HWND hwndDlg, UINT btnId, COLORREF defaultColor, COLORREF& color) {
    struct SwatchPreset { COLORREF c; const wchar_t* label; };
    static const SwatchPreset kPresets[] = {
        { RGB(255, 255, 255), L"#FFFFFF" },
        { RGB(0,   0,   0),   L"#000000" },
        { RGB(128, 128, 128), L"#808080" },
        { RGB(0,   120, 215), L"#0078D7" },
        { RGB(220, 50,  47),  L"#DC322F" },
        { RGB(38,  139, 210), L"#268BD2" },
        { RGB(42,  161, 152), L"#2AA198" },
        { RGB(230, 180, 0),   L"#E6B400" },
    };

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return PickColor(hwndDlg, color);

    auto& lm = RKLangManager::Instance();
    HDC hdc = GetDC(hwndDlg);
    std::vector<HBITMAP> bmps;

    auto appendWithSwatch = [&](UINT id, COLORREF c, const std::wstring& txt) {
        HBITMAP hb = CreateColorMenuBitmap(hdc, c);
        bmps.push_back(hb);
        AppendMenuW(hMenu, MF_STRING, id, txt.c_str());
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_BITMAP;
        mii.hbmpItem = hb;
        SetMenuItemInfoW(hMenu, id, FALSE, &mii);
    };

    appendWithSwatch(1, defaultColor, lm.LoadStr(IDS_LBL_COLOR_DEFAULT, L"Default"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < _countof(kPresets); ++i) {
        appendWithSwatch(static_cast<UINT>(10 + i), kPresets[i].c, kPresets[i].label);
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 2, lm.LoadStr(IDS_LBL_COLOR_MORE, L"More colors...").c_str());
    ReleaseDC(hwndDlg, hdc);

    RECT br{};
    GetWindowRect(GetDlgItem(hwndDlg, btnId), &br);
    UINT cmd = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              br.left, br.bottom, 0, hwndDlg, nullptr);
    DestroyMenu(hMenu);
    for (HBITMAP hb : bmps) DeleteObject(hb);

    if (cmd == 1) {
        if (color != defaultColor) { color = defaultColor; return true; }
        return false;
    }
    if (cmd == 2) {
        return PickColor(hwndDlg, color);
    }
    if (cmd >= 10 && cmd < 10 + _countof(kPresets)) {
        COLORREF picked = kPresets[cmd - 10].c;
        if (color != picked) { color = picked; return true; }
    }
    return false;
}

static bool ReadComboSelectionText(HWND hCombo, std::wstring& out) {
    if (!hCombo) return false;
    LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR) {
        LRESULT len = SendMessageW(hCombo, CB_GETLBTEXTLEN, static_cast<WPARAM>(sel), 0);
        if (len >= 0 && len < 256) {
            std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
            SendMessageW(hCombo, CB_GETLBTEXT, static_cast<WPARAM>(sel), reinterpret_cast<LPARAM>(buf.data()));
            buf.resize(wcslen(buf.c_str()));
            out = std::move(buf);
            return !out.empty();
        }
    }
    wchar_t txt[256] = {};
    GetWindowTextW(hCombo, txt, 256);
    out = txt;
    return !out.empty();
}

static RKDockOptions BuildLabelOptionsFromPage(HWND hwnd, const SettingsCtx* ctx) {
    RKDockOptions o = ctx->opts;
    o.hideLabel   = (IsDlgButtonChecked(hwnd, IDC_LBL_HIDE) == BST_CHECKED);
    o.fadeLabel   = (IsDlgButtonChecked(hwnd, IDC_LBL_FADE) == BST_CHECKED);
    o.bold        = (IsDlgButtonChecked(hwnd, IDC_LBL_BOLD) == BST_CHECKED);
    o.italic      = (IsDlgButtonChecked(hwnd, IDC_LBL_ITALIC) == BST_CHECKED);

    std::wstring fontTxt;
    if (ReadComboSelectionText(GetDlgItem(hwnd, IDC_LBL_FONT), fontTxt)) {
        o.font = fontTxt;
    }
    std::wstring szTxt;
    if (ReadComboSelectionText(GetDlgItem(hwnd, IDC_LBL_SIZE), szTxt)) {
        int sz = _wtoi(szTxt.c_str());
        if (sz > 0) o.fontSize = sz;
    }

    o.colorLabel  = ctx->lblColorLabel;
    o.colorEdge   = ctx->lblColorEdge;
    o.colorShadow = ctx->lblColorShadow;
    if (GetDlgItem(hwnd, IDC_LBL_ALPHA_TEXT))
        o.alphaLabel  = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_LBL_ALPHA_TEXT,   TBM_GETPOS, 0, 0));
    if (GetDlgItem(hwnd, IDC_LBL_ALPHA_EDGE))
        o.alphaEdge   = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_LBL_ALPHA_EDGE,   TBM_GETPOS, 0, 0));
    if (GetDlgItem(hwnd, IDC_LBL_ALPHA_SHADOW))
        o.alphaShadow = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_LBL_ALPHA_SHADOW, TBM_GETPOS, 0, 0));
    return o;
}

static void InvalidateLabelPreview(HWND hwnd) {
    if (HWND hPrev = GetDlgItem(hwnd, IDC_LBL_PREVIEW_CANVAS)) {
        InvalidateRect(hPrev, nullptr, FALSE);
    }
}

static INT_PTR CALLBACK PageLabelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsCtx* ctx = GetSheetCtx(hwnd, lParam, msg);
    if (!ctx) return FALSE;
    switch (msg) {
    case WM_INITDIALOG: {
        if (!ctx->lblInitColors) {
            ctx->lblColorLabel  = ctx->opts.colorLabel;
            ctx->lblColorEdge   = ctx->opts.colorEdge;
            ctx->lblColorShadow = ctx->opts.colorShadow;
            ctx->lblInitColors  = true;
        }
        CheckDlgButton(hwnd, IDC_LBL_HIDE,   ctx->opts.hideLabel ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_LBL_FADE,   ctx->opts.fadeLabel ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_LBL_BOLD,   ctx->opts.bold      ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_LBL_ITALIC, ctx->opts.italic    ? BST_CHECKED : BST_UNCHECKED);

        HWND hFont = GetDlgItem(hwnd, IDC_LBL_FONT);
        if (hFont) {
            HDC hdc = GetDC(hwnd);
            LOGFONTW lf{};
            lf.lfCharSet = DEFAULT_CHARSET;
            EnumFontFamiliesExW(hdc, &lf, EnumFontCb, reinterpret_cast<LPARAM>(hFont), 0);
            ReleaseDC(hwnd, hdc);
            int idx = static_cast<int>(SendMessageW(hFont, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                                    reinterpret_cast<LPARAM>(ctx->opts.font.c_str())));
            if (idx != CB_ERR) SendMessageW(hFont, CB_SETCURSEL, idx, 0);
            else SetWindowTextW(hFont, ctx->opts.font.c_str());
        }

        HWND hSz = GetDlgItem(hwnd, IDC_LBL_SIZE);
        if (hSz) {
            for (int sz = 7; sz < 72; ++sz) {
                SendMessageW(hSz, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(sz).c_str()));
            }
            std::wstring curSz = std::to_wstring(ctx->opts.fontSize);
            int idx = static_cast<int>(SendMessageW(hSz, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                                    reinterpret_cast<LPARAM>(curSz.c_str())));
            if (idx == CB_ERR) {
                idx = static_cast<int>(SendMessageW(hSz, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(curSz.c_str())));
            }
            if (idx != CB_ERR) SendMessageW(hSz, CB_SETCURSEL, idx, 0);
            else SetWindowTextW(hSz, curSz.c_str());
        }

        HWND hOrder = GetDlgItem(hwnd, IDC_LBL_ORDER);
        if (hOrder) {
            std::wstring noneStr = RKLangManager::Instance().LoadStr(IDS_LBL_NONE, L"None");
            int idx = static_cast<int>(SendMessageW(hOrder, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(noneStr.c_str())));
            if (idx != CB_ERR) SendMessageW(hOrder, CB_SETCURSEL, idx, 0);
        }

        SetupSlider(hwnd, IDC_LBL_ALPHA_TEXT,   0, 0, 255, ctx->opts.alphaLabel);
        SetupSlider(hwnd, IDC_LBL_ALPHA_EDGE,   0, 0, 255, ctx->opts.alphaEdge);
        SetupSlider(hwnd, IDC_LBL_ALPHA_SHADOW, 0, 0, 255, ctx->opts.alphaShadow);

        // Create crisp owner-drawn preview surface inside the Preview groupbox (1045)
        if (HWND hPrevGrp = GetDlgItem(hwnd, IDC_LBL_PREVIEW)) {
            if (!GetDlgItem(hwnd, IDC_LBL_PREVIEW_CANVAS)) {
                RECT pr{};
                GetWindowRect(hPrevGrp, &pr);
                MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&pr), 2);
                int cw = std::max(40, static_cast<int>((pr.right - pr.left) - 14));
                int ch = std::max(24, static_cast<int>((pr.bottom - pr.top) - 22));
                HWND hCanvas = CreateWindowExW(
                    0, L"STATIC", L"",
                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_OWNERDRAW,
                    pr.left + 7, pr.top + 15, cw, ch,
                    hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LBL_PREVIEW_CANVAS)),
                    GetModuleHandleW(nullptr), nullptr);
                if (hCanvas) {
                    SetWindowPos(hCanvas, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
        }
        return TRUE;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!dis) break;
        if (dis->CtlID == IDC_LBL_COLOR_TEXT) {
            DrawColorSwatchButton(dis, ctx->lblColorLabel);
            return TRUE;
        }
        if (dis->CtlID == IDC_LBL_COLOR_EDGE) {
            DrawColorSwatchButton(dis, ctx->lblColorEdge);
            return TRUE;
        }
        if (dis->CtlID == IDC_LBL_COLOR_SHADOW) {
            DrawColorSwatchButton(dis, ctx->lblColorShadow);
            return TRUE;
        }
        if (dis->CtlID == IDC_LBL_PREVIEW_CANVAS) {
            int w = std::max(1, static_cast<int>(dis->rcItem.right - dis->rcItem.left));
            int h = std::max(1, static_cast<int>(dis->rcItem.bottom - dis->rcItem.top));
            Gdiplus::Bitmap off(w, h, PixelFormat32bppPARGB);
            Gdiplus::Graphics gOff(&off);
            gOff.Clear(Gdiplus::Color(255, 255, 255, 255));
            RKDockOptions liveOpts = BuildLabelOptionsFromPage(hwnd, ctx);
            if (!liveOpts.hideLabel) {
                std::wstring sample = RKLangManager::Instance().LoadStr(IDS_LBL_PREVIEW_TEXT, L"RK Launcher Label");
                RKDockTheme::RenderStyledLabel(
                    gOff, sample,
                    Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)),
                    liveOpts, 1.0f);
            }
            Gdiplus::Graphics gScreen(dis->hDC);
            gScreen.DrawImage(&off, dis->rcItem.left, dis->rcItem.top, 0, 0, w, h, Gdiplus::UnitPixel);
            return TRUE;
        }
        break;
    }
    case WM_HSCROLL:
        InvalidateLabelPreview(hwnd);
        PropSheet_Changed(GetParent(hwnd), hwnd);
        return TRUE;
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        const UINT code = HIWORD(wParam);
        bool changed = false;
        if (id == IDC_LBL_COLOR_TEXT && code == BN_CLICKED) {
            changed = PickLabelColorMenu(hwnd, IDC_LBL_COLOR_TEXT, RGB(255, 255, 255), ctx->lblColorLabel);
            if (changed) InvalidateRect(GetDlgItem(hwnd, IDC_LBL_COLOR_TEXT), nullptr, FALSE);
        } else if (id == IDC_LBL_COLOR_EDGE && code == BN_CLICKED) {
            changed = PickLabelColorMenu(hwnd, IDC_LBL_COLOR_EDGE, RGB(0, 0, 0), ctx->lblColorEdge);
            if (changed) InvalidateRect(GetDlgItem(hwnd, IDC_LBL_COLOR_EDGE), nullptr, FALSE);
        } else if (id == IDC_LBL_COLOR_SHADOW && code == BN_CLICKED) {
            changed = PickLabelColorMenu(hwnd, IDC_LBL_COLOR_SHADOW, RGB(0, 0, 0), ctx->lblColorShadow);
            if (changed) InvalidateRect(GetDlgItem(hwnd, IDC_LBL_COLOR_SHADOW), nullptr, FALSE);
        } else if ((id == IDC_LBL_HIDE || id == IDC_LBL_FADE || id == IDC_LBL_BOLD || id == IDC_LBL_ITALIC) &&
                   code == BN_CLICKED) {
            changed = true;
        } else if ((id == IDC_LBL_FONT || id == IDC_LBL_SIZE || id == IDC_LBL_ORDER) &&
                   (code == CBN_SELCHANGE || code == CBN_EDITCHANGE)) {
            changed = true;
        }
        if (changed) {
            InvalidateLabelPreview(hwnd);
            PropSheet_Changed(GetParent(hwnd), hwnd);
        }
        break;
    }
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->code == PSN_APPLY) {
            ctx->opts = BuildLabelOptionsFromPage(hwnd, ctx);
            ctx->applied = true;
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void PopulateExcList(HWND hwnd, SettingsCtx* ctx) {
    HWND hList = GetDlgItem(hwnd, IDC_EXC_LIST);
    if (!hList) return;
    ClearListAny(hList);
    const auto& vec = (ctx->excTypeSel == 1) ? ctx->opts.excludedMinimize : ctx->opts.excludedApplications;
    for (const auto& s : vec) AddListStringAny(hList, s);
}

static INT_PTR CALLBACK PageExclusionsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsCtx* ctx = GetSheetCtx(hwnd, lParam, msg);
    if (!ctx) return FALSE;
    switch (msg) {
    case WM_INITDIALOG: {
        HWND hType = GetDlgItem(hwnd, IDC_EXC_TYPE_COMBO);
        if (hType) {
            auto& lm = RKLangManager::Instance();
            SendMessageW(hType, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lm.LoadStr(IDS_EXC_NEWAPPS, L"Never show on RK Launcher").c_str()));
            SendMessageW(hType, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lm.LoadStr(IDS_EXC_MINIMIZE, L"Never minimize to RK Launcher").c_str()));
            SendMessageW(hType, CB_SETCURSEL, 0, 0);
        }
        ctx->excTypeSel = 0;
        PopulateExcList(hwnd, ctx);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_EXC_TYPE_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                ctx->excTypeSel = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_EXC_TYPE_COMBO, CB_GETCURSEL, 0, 0));
                PopulateExcList(hwnd, ctx);
            }
            return TRUE;
        case IDC_EXC_ADD: {
            std::wstring entry;
            if (GetDlgItem(hwnd, IDC_EXC_EDIT)) {
                wchar_t buf[260] = {};
                GetDlgItemTextW(hwnd, IDC_EXC_EDIT, buf, 260);
                entry = Trim(buf);
            }
            if (entry.empty()) {
                wchar_t file[MAX_PATH] = {};
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    const wchar_t* fn = PathFindFileNameW(file);
                    entry = fn ? fn : file;
                }
            }
            if (!entry.empty()) {
                auto& vec = (ctx->excTypeSel == 1) ? ctx->opts.excludedMinimize : ctx->opts.excludedApplications;
                vec.push_back(entry);
                AddListStringAny(GetDlgItem(hwnd, IDC_EXC_LIST), entry);
                if (GetDlgItem(hwnd, IDC_EXC_EDIT)) SetDlgItemTextW(hwnd, IDC_EXC_EDIT, L"");
                PropSheet_Changed(GetParent(hwnd), hwnd);
            }
            return TRUE;
        }
        case IDC_EXC_DEL: {
            HWND h = GetDlgItem(hwnd, IDC_EXC_LIST);
            int sel = GetListSelIndexAny(h);
            auto& vec = (ctx->excTypeSel == 1) ? ctx->opts.excludedMinimize : ctx->opts.excludedApplications;
            if (sel >= 0 && sel < static_cast<int>(vec.size())) {
                vec.erase(vec.begin() + sel);
                RemoveListItemAny(h, sel);
                PropSheet_Changed(GetParent(hwnd), hwnd);
            }
            return TRUE;
        }
        }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->code == PSN_APPLY) {
            ctx->applied = true;
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK PageMenuProc(HWND hwnd, UINT msg, WPARAM /*wParam*/, LPARAM lParam) {
    SettingsCtx* ctx = GetSheetCtx(hwnd, lParam, msg);
    if (!ctx) return FALSE;
    switch (msg) {
    case WM_INITDIALOG:
        CheckDlgButton(hwnd, IDC_MNU_SHOW_EXPLORER,     ctx->opts.showInExplorer ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(hwnd, IDC_MNU_EXPLORER_LABEL,   ctx->opts.showInExplorerLabel.c_str());
        CheckDlgButton(hwnd, IDC_MNU_HIDE_EXT,          ctx->opts.hideFileExtension ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_MNU_ONLY_REG,          ctx->opts.onlyRegistered ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_MNU_SHOW_SETTINGS_ALL, ctx->opts.showSettingsAllItems ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    case WM_COMMAND:
        PropSheet_Changed(GetParent(hwnd), hwnd);
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->code == PSN_APPLY) {
            ctx->opts.showInExplorer = (IsDlgButtonChecked(hwnd, IDC_MNU_SHOW_EXPLORER) == BST_CHECKED);
            wchar_t buf[256] = {};
            GetDlgItemTextW(hwnd, IDC_MNU_EXPLORER_LABEL, buf, 256);
            if (*buf) ctx->opts.showInExplorerLabel = buf;
            ctx->opts.hideFileExtension    = (IsDlgButtonChecked(hwnd, IDC_MNU_HIDE_EXT) == BST_CHECKED);
            ctx->opts.onlyRegistered       = (IsDlgButtonChecked(hwnd, IDC_MNU_ONLY_REG) == BST_CHECKED);
            ctx->opts.showSettingsAllItems = (IsDlgButtonChecked(hwnd, IDC_MNU_SHOW_SETTINGS_ALL) == BST_CHECKED);
            ctx->applied = true;
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK PagePerfProc(HWND hwnd, UINT msg, WPARAM /*wParam*/, LPARAM lParam) {
    SettingsCtx* ctx = GetSheetCtx(hwnd, lParam, msg);
    if (!ctx) return FALSE;
    switch (msg) {
    case WM_INITDIALOG: {
        HWND hQ = GetDlgItem(hwnd, IDC_PERF_QUALITY);
        if (hQ) {
            SendMessageW(hQ, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Low"));
            SendMessageW(hQ, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Normal"));
            SendMessageW(hQ, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"High"));
            int sel = 1;
            if (_wcsicmp(ctx->opts.quality.c_str(), L"Low") == 0) sel = 0;
            else if (_wcsicmp(ctx->opts.quality.c_str(), L"High") == 0) sel = 2;
            SendMessageW(hQ, CB_SETCURSEL, sel, 0);
        }
        SetupSlider(hwnd, IDC_PERF_IN_SLIDER,  0, 1, 20, ctx->opts.magnifyTransitionIn);
        SetupSlider(hwnd, IDC_PERF_OUT_SLIDER, 0, 1, 20, ctx->opts.magnifyTransitionOut);
        return TRUE;
    }
    case WM_HSCROLL:
        PropSheet_Changed(GetParent(hwnd), hwnd);
        return TRUE;
    case WM_COMMAND:
        PropSheet_Changed(GetParent(hwnd), hwnd);
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->code == PSN_APPLY) {
            int sel = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_PERF_QUALITY, CB_GETCURSEL, 0, 0));
            if (sel == 0) ctx->opts.quality = L"Low";
            else if (sel == 2) ctx->opts.quality = L"High";
            else ctx->opts.quality = L"Normal";
            ctx->opts.magnifyTransitionIn  = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_PERF_IN_SLIDER,  TBM_GETPOS, 0, 0));
            ctx->opts.magnifyTransitionOut = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_PERF_OUT_SLIDER, TBM_GETPOS, 0, 0));
            ctx->applied = true;
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool ShowSettingsPropertySheet(HWND hwndDock, RKDockOptions& opts,
                               SettingsApplyFn onApply, void* onApplyCtx) {
    static bool s_open = false;
    if (s_open) return false;
    s_open = true;

    SettingsCtx ctx;
    ctx.opts = opts;
    ctx.onApply = onApply;
    ctx.onApplyCtx = onApplyCtx;
    auto& lm = RKLangManager::Instance();
    HPROPSHEETPAGE pages[7] = {
        lm.CreatePage(IDD_PAGE_APPEARANCE,  PageAppearanceProc,  reinterpret_cast<LPARAM>(&ctx), L"Appearance"),
        lm.CreatePage(IDD_PAGE_POSITION,    PagePositionProc,    reinterpret_cast<LPARAM>(&ctx), L"Position"),
        lm.CreatePage(IDD_PAGE_BEHAVIOR,    PageBehaviorProc,    reinterpret_cast<LPARAM>(&ctx), L"Behavior"),
        lm.CreatePage(IDD_PAGE_LABEL,       PageLabelProc,       reinterpret_cast<LPARAM>(&ctx), L"Label"),
        lm.CreatePage(IDD_PAGE_EXCLUSIONS,  PageExclusionsProc,  reinterpret_cast<LPARAM>(&ctx), L"Exclusions"),
        lm.CreatePage(IDD_PAGE_MENU,        PageMenuProc,        reinterpret_cast<LPARAM>(&ctx), L"Menu"),
        lm.CreatePage(IDD_PAGE_PERFORMANCE, PagePerfProc,        reinterpret_cast<LPARAM>(&ctx), L"Performance"),
    };

    std::wstring caption = lm.LoadStr(IDS_SETTINGS_TITLE, L"RK Launcher Settings");
    PROPSHEETHEADERW psh{};
    psh.dwSize      = sizeof(psh);
    psh.dwFlags     = PSH_USEICONID;
    psh.hwndParent  = hwndDock;
    psh.hInstance   = GetModuleHandleW(nullptr);
    psh.pszIcon     = MAKEINTRESOURCEW(IDI_MAIN);
    psh.pszCaption  = caption.c_str();
    psh.nPages      = 7;
    psh.phpage      = pages;
    INT_PTR psRes = PropertySheetW(&psh);

    s_open = false;
    if (ctx.applied && (psRes > 0 || !ctx.committedViaApply)) {
        opts = ctx.opts;
        return true;
    }
    return false;
}

} // namespace rk
