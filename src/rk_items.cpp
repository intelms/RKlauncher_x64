#include "rk_items.h"
#include "rk_common.h"
#include <commctrl.h>
#include <commoncontrols.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace rk {

// Registry mapping hidden docklet HWND -> RKDockItemDockletOD*
static std::mutex g_dockletMapMtx;
static std::unordered_map<HWND, RKDockItemDockletOD*> g_dockletByHwnd;
static HWND g_hostDockHwnd = nullptr;
static bool g_dockletClassRegistered = false;

void RKDockItemDockletOD::RegisterHostDockHwnd(HWND hwndDock) {
    g_hostDockHwnd = hwndDock;
}

HWND RKDockItemDockletOD::HostDockHwnd() {
    return g_hostDockHwnd;
}

RKDockItemDockletOD* RKDockItemDockletOD::FindByHwnd(HWND hwndDocklet) {
    std::lock_guard<std::mutex> lk(g_dockletMapMtx);
    auto it = g_dockletByHwnd.find(hwndDocklet);
    return (it != g_dockletByHwnd.end()) ? it->second : nullptr;
}

// ============================================================================
// Bitmap / Icon helpers
// ============================================================================

std::unique_ptr<Gdiplus::Bitmap> BitmapFromHIcon(HICON hIcon) {
    if (!hIcon) return nullptr;
    ICONINFO ii{};
    if (!GetIconInfo(hIcon, &ii)) return nullptr;

    BITMAP bmColor{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bmColor), &bmColor) > 0 &&
        bmColor.bmWidth > 0 && bmColor.bmHeight > 0) {
        int w = bmColor.bmWidth;
        int h = bmColor.bmHeight;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h; // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        std::vector<uint32_t> pixels(static_cast<size_t>(w) * h, 0);
        HDC hdc = GetDC(nullptr);
        GetDIBits(hdc, ii.hbmColor, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);

        bool hasAlpha = false;
        for (uint32_t px : pixels) {
            if ((px >> 24) != 0) { hasAlpha = true; break; }
        }
        if (!hasAlpha && ii.hbmMask) {
            std::vector<uint32_t> maskPx(static_cast<size_t>(w) * h, 0);
            GetDIBits(hdc, ii.hbmMask, 0, h, maskPx.data(), &bi, DIB_RGB_COLORS);
            for (size_t i = 0; i < pixels.size(); ++i) {
                bool transparent = (maskPx[i] & 0x00FFFFFF) != 0;
                pixels[i] = transparent ? 0 : ((pixels[i] & 0x00FFFFFF) | 0xFF000000);
            }
        }
        ReleaseDC(nullptr, hdc);

        auto bmp = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppARGB);
        Gdiplus::BitmapData bd{};
        Gdiplus::Rect rc(0, 0, w, h);
        if (bmp->LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
            for (int y = 0; y < h; ++y) {
                memcpy(static_cast<uint8_t*>(bd.Scan0) + y * bd.Stride,
                       pixels.data() + static_cast<size_t>(y) * w,
                       static_cast<size_t>(w) * 4);
            }
            bmp->UnlockBits(&bd);
        }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return bmp;
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    auto raw = std::unique_ptr<Gdiplus::Bitmap>(Gdiplus::Bitmap::FromHICON(hIcon));
    if (raw && raw->GetLastStatus() == Gdiplus::Ok) return raw;
    return nullptr;
}

std::unique_ptr<Gdiplus::Bitmap> LoadBitmapAuto(const std::wstring& path) {
    if (path.empty()) return nullptr;

    wchar_t expanded[MAX_PATH * 2] = {};
    DWORD expLen = ExpandEnvironmentStringsW(path.c_str(), expanded, static_cast<DWORD>(_countof(expanded)));
    std::wstring raw = (expLen > 0 && expLen < _countof(expanded)) ? expanded : path;

    // Support "file.dll,index" / "shell32.dll,-4" before plain FileExists check
    size_t comma = raw.rfind(L',');
    if (comma != std::wstring::npos) {
        const wchar_t* idxStart = raw.c_str() + comma + 1;
        wchar_t* end = nullptr;
        wcstol(idxStart, &end, 10);
        if (end && end > idxStart && *end == L'\0') {
            std::wstring filePart = Trim(raw.substr(0, comma));
            std::wstring resolvedFile = ResolveAppPath(filePart);
            std::wstring spec = FileExists(resolvedFile)
                                    ? (resolvedFile + raw.substr(comma))
                                    : raw;
            if (auto b = ExtractIconBitmapForPath(spec, 128)) {
                return b;
            }
            raw = filePart;
        }
    }

    std::wstring resolved = ResolveAppPath(raw);
    if (!FileExists(resolved)) {
        // Also try icons/<path> fallback
        std::wstring alt = ResolveAppPath(L"icons\\" + raw);
        if (FileExists(alt)) resolved = alt;
        else return nullptr;
    }

    auto extPos = resolved.rfind(L'.');
    std::wstring ext = (extPos != std::wstring::npos) ? ToLower(resolved.substr(extPos)) : L"";
    if (ext == L".ico") {
        HICON hIcon = static_cast<HICON>(LoadImageW(nullptr, resolved.c_str(), IMAGE_ICON, 128, 128, LR_LOADFROMFILE));
        if (!hIcon) hIcon = static_cast<HICON>(LoadImageW(nullptr, resolved.c_str(), IMAGE_ICON, 48, 48, LR_LOADFROMFILE));
        if (hIcon) {
            auto b = BitmapFromHIcon(hIcon);
            DestroyIcon(hIcon);
            if (b) return b;
        }
    }
    auto bmp = std::make_unique<Gdiplus::Bitmap>(resolved.c_str(), FALSE);
    if (bmp && bmp->GetLastStatus() == Gdiplus::Ok && bmp->GetWidth() > 0) {
        return bmp;
    }
    return ExtractIconBitmapForPath(resolved, 128);
}

std::unique_ptr<Gdiplus::Bitmap> ExtractIconBitmapForPath(const std::wstring& path, int desiredSize) {
    if (path.empty()) return nullptr;
    std::wstring fileOnly = path;
    int iconIndex = 0;
    size_t comma = path.rfind(L',');
    if (comma != std::wstring::npos) {
        const wchar_t* idxStart = path.c_str() + comma + 1;
        wchar_t* end = nullptr;
        long idx = wcstol(idxStart, &end, 10);
        if (end && end > idxStart && *end == L'\0') {
            fileOnly = Trim(path.substr(0, comma));
            iconIndex = static_cast<int>(idx);

            HICON hLarge = nullptr;
            if (ExtractIconExW(fileOnly.c_str(), iconIndex, &hLarge, nullptr, 1) > 0 && hLarge) {
                auto b = BitmapFromHIcon(hLarge);
                DestroyIcon(hLarge);
                if (b) return b;
            }
        }
    }

    SHFILEINFOW sfi{};
    DWORD_PTR ok = SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX);
    if (ok) {
        IImageList* pImgList = nullptr;
        int imgType = (desiredSize >= 64) ? SHIL_JUMBO : SHIL_EXTRALARGE;
        if (SUCCEEDED(SHGetImageList(imgType, IID_PPV_ARGS(&pImgList))) && pImgList) {
            HICON hIcon = nullptr;
            if (SUCCEEDED(pImgList->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hIcon)) && hIcon) {
                auto b = BitmapFromHIcon(hIcon);
                DestroyIcon(hIcon);
                pImgList->Release();
                if (b) return b;
            }
            pImgList->Release();
        }
    }

    SHFILEINFOW sfi2{};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi2, sizeof(sfi2), SHGFI_ICON | SHGFI_LARGEICON) && sfi2.hIcon) {
        auto b = BitmapFromHIcon(sfi2.hIcon);
        DestroyIcon(sfi2.hIcon);
        if (b) return b;
    }
    return nullptr;
}

std::unique_ptr<Gdiplus::Bitmap> CreateBuiltinFallbackIcon(const std::wstring& hint, int size) {
    auto bmp = std::make_unique<Gdiplus::Bitmap>(size, size, PixelFormat32bppARGB);
    Gdiplus::Graphics g(bmp.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    std::wstring low = ToLower(hint);
    Gdiplus::Color top(245, 76, 150, 235), bot(245, 38, 92, 185);
    wchar_t glyph = L'R';
    if (low.find(L"folder") != std::wstring::npos || low.find(L"doc") != std::wstring::npos || low.find(L"explorer") != std::wstring::npos) {
        top = Gdiplus::Color(245, 250, 200, 75);
        bot = Gdiplus::Color(245, 218, 145, 30);
        glyph = L'F';
    } else if (low.find(L"setting") != std::wstring::npos || low.find(L"control") != std::wstring::npos) {
        top = Gdiplus::Color(245, 130, 145, 165);
        bot = Gdiplus::Color(245, 75, 88, 108);
        glyph = L'S';
    } else if (low.find(L"comput") != std::wstring::npos || low.find(L"pc") != std::wstring::npos) {
        top = Gdiplus::Color(245, 92, 180, 240);
        bot = Gdiplus::Color(245, 42, 110, 185);
        glyph = L'C';
    } else if (!hint.empty()) {
        glyph = static_cast<wchar_t>(towupper(hint[0]));
    }

    float pad = size * 0.08f;
    float w = size - pad * 2.0f;
    float r = w * 0.22f;
    Gdiplus::GraphicsPath gp;
    gp.AddArc(pad, pad, r * 2, r * 2, 180, 90);
    gp.AddArc(pad + w - r * 2, pad, r * 2, r * 2, 270, 90);
    gp.AddArc(pad + w - r * 2, pad + w - r * 2, r * 2, r * 2, 0, 90);
    gp.AddArc(pad, pad + w - r * 2, r * 2, r * 2, 90, 90);
    gp.CloseFigure();

    Gdiplus::LinearGradientBrush bg(Gdiplus::PointF(0, pad), Gdiplus::PointF(0, pad + w), top, bot);
    g.FillPath(&bg, &gp);
    Gdiplus::Pen rim(Gdiplus::Color(160, 255, 255, 255), std::max(1.0f, size * 0.025f));
    g.DrawPath(&rim, &gp);

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, size * 0.44f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    wchar_t str[2] = { glyph, 0 };
    Gdiplus::SolidBrush textBr(Gdiplus::Color(245, 255, 255, 255));
    g.DrawString(str, 1, &font, Gdiplus::RectF(0, 0, (float)size, (float)size), &sf, &textBr);
    return bmp;
}

// ============================================================================
// RKDockItem Base
// ============================================================================

bool RKDockItem::LoadConfig(const IniSection& sec) {
    label_         = sec.GetString(L"name", label_);
    iconPath_      = sec.GetString(L"icon", iconPath_);
    showIndicator_ = sec.GetBool(L"showIndicator", true);
    return true;
}

void RKDockItem::SaveConfig(IniSection& sec) const {
    sec.SetString(L"type", TypeString());
    if (!label_.empty()) sec.SetString(L"name", label_);
    if (!iconPath_.empty()) sec.SetString(L"icon", MakeRelativeIfInsideAppDir(iconPath_));
}

void RKDockItem::OnRightClick(HWND hwndDock, POINT screenPt) {
    SendMessageW(hwndDock, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(hwndDock),
                 MAKELPARAM(screenPt.x, screenPt.y));
}

void RKDockItem::ReloadIcon() {
    if (!ownsIconBmp_) iconBmp_.release();
    ownsIconBmp_ = true;
    iconBmp_.reset();

    if (!iconPath_.empty()) {
        std::wstring spec = iconPath_;
        if (spec.rfind(L"DockletPath:", 0) == 0) {
            spec = L"docklets\\" + spec.substr(12);
        }
        iconBmp_ = LoadBitmapAuto(spec);
    }
    if (!iconBmp_) {
        iconBmp_ = ResolveDefaultIcon();
    }
}

std::unique_ptr<Gdiplus::Bitmap> RKDockItem::ResolveDefaultIcon() {
    if (Type() == DockItemType::Separator) return nullptr;
    return CreateBuiltinFallbackIcon(label_.empty() ? TypeString() : label_);
}

void RKDockItem::SetCustomBitmap(Gdiplus::Bitmap* bmp, bool takeOwnership) {
    if (!ownsIconBmp_) iconBmp_.release();
    iconBmp_.reset(bmp);
    ownsIconBmp_ = takeOwnership;
}

void RKDockItem::SetOverlayBitmap(Gdiplus::Bitmap* bmp, bool takeOwnership) {
    if (!ownsOverlayBmp_) overlayBmp_.release();
    overlayBmp_.reset(bmp);
    ownsOverlayBmp_ = takeOwnership;
}

void RKDockItem::StartBounce(int count) {
    if (count <= 0) count = 1;
    bounceLeft_ = count;
    bouncePhaseTick_ = GetTickCount();
}

float RKDockItem::GetBounceOffset01(DWORD nowTick) {
    if (bounceLeft_ <= 0) return 0.0f;
    constexpr DWORD kCycleMs = 460;
    DWORD elapsed = nowTick - bouncePhaseTick_;
    while (elapsed >= kCycleMs && bounceLeft_ > 0) {
        elapsed -= kCycleMs;
        bouncePhaseTick_ += kCycleMs;
        --bounceLeft_;
    }
    if (bounceLeft_ <= 0) return 0.0f;
    float p = static_cast<float>(elapsed) / static_cast<float>(kCycleMs);
    // Smooth parabolic bounce: sin(pi * p)
    return std::sin(p * 3.14159265f);
}

// ============================================================================
// RKDockItemShortcut
// ============================================================================

bool RKDockItemShortcut::LoadConfig(const IniSection& sec) {
    RKDockItem::LoadConfig(sec);
    link_         = sec.GetString(L"link", link_);
    arguments_    = sec.GetString(L"arguments", arguments_);
    workFolder_   = sec.GetString(L"workFolder", workFolder_);
    showCmd_      = ToLower(sec.GetString(L"show", L"normal"));
    sameInstance_ = sec.GetBool(L"sameInstance", true);
    ReloadIcon();
    return true;
}

void RKDockItemShortcut::SaveConfig(IniSection& sec) const {
    sec.SetString(L"type", TypeString());
    sec.SetString(L"name", label_);
    if (!iconPath_.empty()) sec.SetString(L"icon", MakeRelativeIfInsideAppDir(iconPath_));
    if (!link_.empty()) sec.SetString(L"link", MakeRelativeIfInsideAppDir(link_));
    if (!arguments_.empty()) sec.SetString(L"arguments", arguments_);
    if (!workFolder_.empty()) sec.SetString(L"workFolder", MakeRelativeIfInsideAppDir(workFolder_));
    sec.SetString(L"show", showCmd_.empty() ? L"normal" : showCmd_);
    sec.SetBool(L"sameInstance", sameInstance_);
}

std::unique_ptr<Gdiplus::Bitmap> RKDockItemShortcut::ResolveDefaultIcon() {
    if (!link_.empty()) {
        std::wstring resolved = ResolveAppPath(link_);
        if (FileExists(resolved) || DirExists(resolved)) {
            if (auto b = ExtractIconBitmapForPath(resolved, 128)) return b;
        }
        // Shell namespace or command like explorer.exe / cmd.exe
        if (auto b = ExtractIconBitmapForPath(link_, 128)) return b;
    }
    return CreateBuiltinFallbackIcon(label_.empty() ? L"App" : label_);
}

static bool ActivateExistingWindowForExe(const std::wstring& link) {
    if (link.empty()) return false;
    std::wstring resolved = ResolveAppPath(link);
    const wchar_t* fnamePtr = PathFindFileNameW(resolved.c_str());
    std::wstring targetExe = ToLower(fnamePtr ? fnamePtr : resolved);
    if (targetExe.empty()) return false;

    struct Ctx { std::wstring exe; HWND found = nullptr; } ctx{ targetExe, nullptr };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(hwnd) && !IsIconic(hwnd)) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            wchar_t path[MAX_PATH] = {};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, path, &sz)) {
                const wchar_t* pfn = PathFindFileNameW(path);
                if (pfn && ToLower(pfn) == c->exe) {
                    c->found = hwnd;
                    CloseHandle(hProc);
                    return FALSE;
                }
            }
            CloseHandle(hProc);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found) {
        if (IsIconic(ctx.found)) ShowWindow(ctx.found, SW_RESTORE);
        ShowWindow(ctx.found, SW_SHOW);
        SetForegroundWindow(ctx.found);
        return true;
    }
    return false;
}

std::unordered_set<std::wstring> CollectRunningExeNames() {
    std::unordered_set<std::wstring> running;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.szExeFile[0]) {
                    CharLowerW(pe.szExeFile);
                    running.emplace(pe.szExeFile);
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    return running;
}

void RKDockItemShortcut::RefreshRuntimeStateWithSet(const std::unordered_set<std::wstring>& runningExes) {
    cachedRunning_ = false;
    if (link_.empty()) return;

    if (cachedLinkKey_ != link_) {
        cachedLinkKey_ = link_;
        cachedExeNameLower_.clear();
        std::wstring resolved = ResolveAppPath(link_);
        const wchar_t* fname = PathFindFileNameW(resolved.c_str());
        if (fname && *fname) {
            std::wstring exeLow = ToLower(fname);
            if (exeLow.size() >= 4 && exeLow.substr(exeLow.size() - 4) == L".exe") {
                cachedExeNameLower_ = std::move(exeLow);
            }
        }
    }
    if (cachedExeNameLower_.empty()) return;
    cachedRunning_ = (runningExes.count(cachedExeNameLower_) != 0);
}

void RKDockItemShortcut::RefreshRuntimeState() {
    auto runningExes = CollectRunningExeNames();
    RefreshRuntimeStateWithSet(runningExes);
}

bool RKDockItemShortcut::IsRunning() const {
    return cachedRunning_;
}

void RKDockItemShortcut::Execute(HWND /*hwndDock*/) {
    if (link_.empty()) return;
    if (sameInstance_ && ActivateExistingWindowForExe(link_)) {
        return;
    }

    int nShow = SW_SHOWNORMAL;
    if (showCmd_ == L"maximized") nShow = SW_SHOWMAXIMIZED;
    else if (showCmd_ == L"minimized") nShow = SW_SHOWMINIMIZED;

    std::wstring resolved = link_;
    std::wstring candidate = ResolveAppPath(link_);
    if (FileExists(candidate) || DirExists(candidate)) resolved = candidate;

    std::wstring workDir = workFolder_;
    if (!workDir.empty()) workDir = ResolveAppPath(workDir);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = L"open";
    sei.lpFile       = resolved.c_str();
    sei.lpParameters = arguments_.empty() ? nullptr : arguments_.c_str();
    sei.lpDirectory  = workDir.empty() ? nullptr : workDir.c_str();
    sei.nShow        = nShow;
    ShellExecuteExW(&sei);
}

void RKDockItemShortcut::OnDropFiles(HWND /*hwndDock*/, HDROP hDrop) {
    if (link_.empty()) return;
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    std::wstring args;
    for (UINT i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(hDrop, i, path, MAX_PATH)) {
            if (!args.empty()) args += L' ';
            args += L"\"" + std::wstring(path) + L"\"";
        }
    }
    if (!args.empty()) {
        std::wstring resolved = ResolveAppPath(link_);
        if (!FileExists(resolved)) resolved = link_;
        ShellExecuteW(nullptr, L"open", resolved.c_str(), args.c_str(),
                      workFolder_.empty() ? nullptr : workFolder_.c_str(), SW_SHOWNORMAL);
    }
}

bool RKDockItemShortcut::InitFromDroppedFile(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    std::wstring ext = ToLower(PathFindExtensionW(filePath.c_str()));
    if (ext == L".lnk") {
        IShellLinkW* psl = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl))) && psl) {
            IPersistFile* ppf = nullptr;
            if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARGS(&ppf))) && ppf) {
                if (SUCCEEDED(ppf->Load(filePath.c_str(), STGM_READ))) {
                    wchar_t target[MAX_PATH] = {}, args[512] = {}, dir[MAX_PATH] = {}, icon[MAX_PATH] = {};
                    int iIdx = 0, show = SW_SHOWNORMAL;
                    psl->GetPath(target, MAX_PATH, nullptr, 0);
                    psl->GetArguments(args, 512);
                    psl->GetWorkingDirectory(dir, MAX_PATH);
                    psl->GetIconLocation(icon, MAX_PATH, &iIdx);
                    psl->GetShowCmd(&show);
                    link_ = *target ? target : filePath;
                    arguments_ = args;
                    workFolder_ = dir;
                    if (show == SW_SHOWMAXIMIZED) showCmd_ = L"maximized";
                    else if (show == SW_SHOWMINIMIZED) showCmd_ = L"minimized";
                    else showCmd_ = L"normal";
                    if (*icon) iconPath_ = std::wstring(icon) + L"," + std::to_wstring(iIdx);
                }
                ppf->Release();
            }
            psl->Release();
        }
    }
    if (link_.empty()) link_ = filePath;

    wchar_t titleBuf[MAX_PATH] = {};
    wcsncpy_s(titleBuf, PathFindFileNameW(filePath.c_str()), _TRUNCATE);
    PathRemoveExtensionW(titleBuf);
    label_ = *titleBuf ? titleBuf : filePath;
    ReloadIcon();
    return true;
}

// ============================================================================
// RKDockItemFolder
// ============================================================================

struct FolderMenuEntry {
    std::wstring fullPath;
    bool isDir = false;
};

static void PopulateFolderHMenu(HMENU hMenu, const std::wstring& dirPath,
                                std::vector<FolderMenuEntry>& entries,
                                const RKDockOptions& opts, bool topLevel, int depth)
{
    if (depth > 3) return;
    if (topLevel && opts.showInExplorer) {
        UINT id = 10000 + static_cast<UINT>(entries.size());
        entries.push_back({dirPath, true});
        AppendMenuW(hMenu, MF_STRING, id, opts.showInExplorerLabel.c_str());
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    }

    std::vector<std::wstring> dirs;
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd{};
    std::wstring search = dirPath;
    if (!search.empty() && search.back() != L'\\') search.push_back(L'\\');
    search += L'*';
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.emplace_back(fd.cFileName);
            else files.emplace_back(fd.cFileName);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());

    bool addedAnything = false;
    for (const auto& d : dirs) {
        std::wstring subPath = dirPath + L"\\" + d;
        HMENU sub = CreatePopupMenu();
        PopulateFolderHMenu(sub, subPath, entries, opts, false, depth + 1);
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), d.c_str());
        addedAnything = true;
    }

    for (const auto& f : files) {
        if (entries.size() > 600) break;
        std::wstring ext = PathFindExtensionW(f.c_str());
        if (opts.onlyRegistered && !ext.empty()) {
            HKEY hk = nullptr;
            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, ext.c_str(), 0, KEY_READ, &hk) != ERROR_SUCCESS) {
                continue;
            }
            RegCloseKey(hk);
        }
        std::wstring disp = f;
        if (opts.hideFileExtension) {
            wchar_t tmp[MAX_PATH] = {};
            wcsncpy_s(tmp, f.c_str(), _TRUNCATE);
            PathRemoveExtensionW(tmp);
            disp = tmp;
        }
        UINT id = 10000 + static_cast<UINT>(entries.size());
        entries.push_back({dirPath + L"\\" + f, false});
        AppendMenuW(hMenu, MF_STRING, id, disp.c_str());
        addedAnything = true;
    }

    if (!addedAnything) {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"(Empty)");
    }
}

void RKDockItemFolder::Execute(HWND hwndDock) {
    // Ask DockWnd to show popup folder menu at screenRect center using current options
    POINT pt{ (screenRect.left + screenRect.right) / 2, (screenRect.top + screenRect.bottom) / 2 };
    SendMessageW(hwndDock, WM_RK_ITEM_CHANGED, 1, reinterpret_cast<LPARAM>(this));
    (void)pt;
}

void RKDockItemFolder::ShowFolderPopupMenu(HWND hwndDock, POINT screenPt, const RKDockOptions& opts) {
    std::wstring resolved = ResolveAppPath(link_);
    if (!DirExists(resolved)) {
        RKDockItemShortcut::Execute(hwndDock);
        return;
    }
    HMENU hMenu = CreatePopupMenu();
    std::vector<FolderMenuEntry> entries;
    PopulateFolderHMenu(hMenu, resolved, entries, opts, true, 0);

    SetForegroundWindow(hwndDock);
    UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                              screenPt.x, screenPt.y, 0, hwndDock, nullptr);
    DestroyMenu(hMenu);

    if (cmd >= 10000) {
        size_t idx = cmd - 10000;
        if (idx < entries.size()) {
            ShellExecuteW(nullptr, L"open", entries[idx].fullPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

// ============================================================================
// RKDockItemWindow
// ============================================================================

RKDockItemWindow::RKDockItemWindow(HWND target) : targetHwnd_(target) {
    showIndicator_ = false;
    RefreshRuntimeState();
    ReloadIcon();
}

RKDockItemWindow::~RKDockItemWindow() = default;

bool RKDockItemWindow::LoadConfig(const IniSection& sec) {
    RKDockItem::LoadConfig(sec);
    ReloadIcon();
    return true;
}

void RKDockItemWindow::SaveConfig(IniSection& sec) const {
    // Window items are runtime-only; do not persist unless needed
    (void)sec;
}

void RKDockItemWindow::RefreshRuntimeState() {
    if (!targetHwnd_ || !IsWindow(targetHwnd_)) {
        markedForRemoval = true;
        return;
    }
    wchar_t title[256] = {};
    if (GetWindowTextW(targetHwnd_, title, 256) > 0) {
        label_ = title;
    }
}

std::unique_ptr<Gdiplus::Bitmap> RKDockItemWindow::ResolveDefaultIcon() {
    if (targetHwnd_ && IsWindow(targetHwnd_)) {
        HICON hIcon = nullptr;
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(targetHwnd_, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 60, &res) && res) {
            hIcon = reinterpret_cast<HICON>(res);
        }
        if (!hIcon) {
            hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(targetHwnd_, GCLP_HICON));
        }
        if (!hIcon) {
            if (SendMessageTimeoutW(targetHwnd_, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 60, &res) && res) {
                hIcon = reinterpret_cast<HICON>(res);
            }
        }
        if (hIcon) {
            if (auto bmp = BitmapFromHIcon(hIcon)) return bmp;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(targetHwnd_, &pid);
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            wchar_t path[MAX_PATH] = {};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, path, &sz)) {
                CloseHandle(hProc);
                if (auto bmp = ExtractIconBitmapForPath(path, 128)) return bmp;
            } else {
                CloseHandle(hProc);
            }
        }
    }
    return CreateBuiltinFallbackIcon(label_.empty() ? L"W" : label_);
}

void RKDockItemWindow::Execute(HWND /*hwndDock*/) {
    if (!targetHwnd_ || !IsWindow(targetHwnd_)) {
        markedForRemoval = true;
        return;
    }
    if (minimizedByDock_) {
        minimizedByDock_ = false;
        ShowWindow(targetHwnd_, SW_SHOW);
    }
    if (IsIconic(targetHwnd_)) {
        ShowWindow(targetHwnd_, SW_RESTORE);
    }
    SetForegroundWindow(targetHwnd_);
}

void RKDockItemWindow::CloseTargetWindow() {
    if (targetHwnd_ && IsWindow(targetHwnd_)) {
        PostMessageW(targetHwnd_, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
}

// ============================================================================
// RKDockItemDockletOD
// ============================================================================

RKDockItemDockletOD::~RKDockItemDockletOD() {
    ShutdownPlugin();
}

LRESULT CALLBACK RKDockItemDockletOD::DockletWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RKDockItemDockletOD* self = FindByHwnd(hwnd);
    if (self && self->fnOnProcessMessage_ && self->pluginData_) {
        self->fnOnProcessMessage_(self->pluginData_, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static std::wstring ResolveDockletDllPath(const std::wstring& dllRelOrAbs) {
    if (dllRelOrAbs.empty()) return {};
    std::wstring spec = NormalizeSlashes(dllRelOrAbs, L'\\');
    if (spec.rfind(L"DockletPath:", 0) == 0) {
        spec = L"docklets\\" + spec.substr(12);
    }
    std::wstring primary = ResolveAppPath(spec);
    if (FileExists(primary)) return primary;
    if (PathIsRelativeW(spec.c_str())) {
        std::wstring underDocklets = ResolveAppPath(L"docklets\\" + spec);
        if (FileExists(underDocklets)) return underDocklets;
    }
    return primary;
}

bool RKDockItemDockletOD::InitializePlugin(HWND hwndDockParent, const std::wstring& dllRelOrAbs,
                                           const std::wstring& iniPath, const std::wstring& iniGroup)
{
    ShutdownPlugin();

    std::wstring resolvedDll = ResolveDockletDllPath(dllRelOrAbs);
    if (!FileExists(resolvedDll)) {
        DebugLog(L"Docklet DLL not found: %s", resolvedDll.c_str());
        return false;
    }

    uint16_t machine = 0;
    if (!IsPeMachineX64(resolvedDll, &machine)) {
        DebugLog(L"Skipping non-x64 docklet DLL (machine=0x%04x): %s", machine, resolvedDll.c_str());
        return false;
    }

    hMod_ = LoadLibraryW(resolvedDll.c_str());
    if (!hMod_) {
        DebugLog(L"LoadLibraryW failed (%lu) for %s", GetLastError(), resolvedDll.c_str());
        return false;
    }

    dllPath_        = MakeRelativeIfInsideAppDir(resolvedDll);
    configIniFile_  = iniPath.empty() ? L"docklets\\docklets.ini" : iniPath;
    configIniGroup_ = iniGroup.empty() ? L"DockletOD" : iniGroup;

    // Derive relative folder relative to exeDir
    wchar_t dirBuf[MAX_PATH] = {};
    wcsncpy_s(dirBuf, resolvedDll.c_str(), _TRUNCATE);
    PathRemoveFileSpecW(dirBuf);
    relativeFolder_ = MakeRelativeIfInsideAppDir(dirBuf);

    fnOnCreate_          = reinterpret_cast<FnOnCreate>(GetProcAddress(hMod_, "OnCreate"));
    fnOnDestroy_         = reinterpret_cast<FnOnDestroy>(GetProcAddress(hMod_, "OnDestroy"));
    fnOnSave_            = reinterpret_cast<FnOnSave>(GetProcAddress(hMod_, "OnSave"));
    fnOnLeftClick_       = reinterpret_cast<FnOnLeftClick>(GetProcAddress(hMod_, "OnLeftButtonClick"));
    fnOnRightClick_      = reinterpret_cast<FnOnRightClick>(GetProcAddress(hMod_, "OnRightButtonClick"));
    fnOnDoubleClick_     = reinterpret_cast<FnOnDoubleClick>(GetProcAddress(hMod_, "OnDoubleClick"));
    fnOnLeftHeld_        = reinterpret_cast<FnOnLeftHeld>(GetProcAddress(hMod_, "OnLeftButtonHeld"));
    fnOnConfigure_       = reinterpret_cast<FnOnConfigure>(GetProcAddress(hMod_, "OnConfigure"));
    fnOnAcceptDropFiles_ = reinterpret_cast<FnOnAcceptDropFiles>(GetProcAddress(hMod_, "OnAcceptDropFiles"));
    fnOnDropFiles_       = reinterpret_cast<FnOnDropFiles>(GetProcAddress(hMod_, "OnDropFiles"));
    fnOnProcessMessage_  = reinterpret_cast<FnOnProcessMessage>(GetProcAddress(hMod_, "OnProcessMessage"));
    fnOnGetInfo_         = reinterpret_cast<FnOnGetInfo>(GetProcAddress(hMod_, "OnGetInformation"));

    if (!g_dockletClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DockletWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassDockletOD;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            g_dockletClassRegistered = true;
        }
    }

    hwndDocklet_ = CreateWindowExW(0, kClassDockletOD, kTitleDockletWnd, WS_CHILD,
                                   0, 0, 1, 1, hwndDockParent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwndDocklet_) {
        hwndDocklet_ = CreateWindowExW(0, kClassDockletOD, kTitleDockletWnd, WS_OVERLAPPED,
                                       -100, -100, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    if (hwndDocklet_) {
        std::lock_guard<std::mutex> lk(g_dockletMapMtx);
        g_dockletByHwnd[hwndDocklet_] = this;
    }

    if (fnOnGetInfo_) {
        char nameBuf[256] = {}, authorBuf[256] = {}, notesBuf[512] = {};
        int ver = 0;
        fnOnGetInfo_(nameBuf, authorBuf, &ver, notesBuf);
        if (pluginName_.empty() && *nameBuf) pluginName_ = Utf8ToWide(nameBuf);
        if (label_.empty() && *nameBuf) label_ = Utf8ToWide(nameBuf);
    }

    if (fnOnCreate_ && hwndDocklet_) {
        std::wstring absIni = ResolveAppPath(configIniFile_);
        std::string iniA = WideToUtf8(absIni);
        std::string grpA = WideToUtf8(configIniGroup_);
        pluginData_ = fnOnCreate_(hwndDocklet_, hMod_,
                                  const_cast<char*>(iniA.c_str()),
                                  const_cast<char*>(grpA.c_str()));
    }

    if (!iconBmp_) ReloadIcon();
    return hwndDocklet_ != nullptr;
}

void RKDockItemDockletOD::ShutdownPlugin() {
    if (fnOnDestroy_ && hwndDocklet_) {
        fnOnDestroy_(pluginData_, hwndDocklet_);
    }
    pluginData_ = nullptr;
    if (hwndDocklet_) {
        {
            std::lock_guard<std::mutex> lk(g_dockletMapMtx);
            g_dockletByHwnd.erase(hwndDocklet_);
        }
        DestroyWindow(hwndDocklet_);
        hwndDocklet_ = nullptr;
    }
    if (hMod_) {
        FreeLibrary(hMod_);
        hMod_ = nullptr;
    }
    fnOnCreate_ = nullptr;
    fnOnDestroy_ = nullptr;
    fnOnSave_ = nullptr;
    fnOnLeftClick_ = nullptr;
    fnOnRightClick_ = nullptr;
    fnOnDoubleClick_ = nullptr;
    fnOnLeftHeld_ = nullptr;
    fnOnConfigure_ = nullptr;
    fnOnAcceptDropFiles_ = nullptr;
    fnOnDropFiles_ = nullptr;
    fnOnProcessMessage_ = nullptr;
    fnOnGetInfo_ = nullptr;
}

static bool IsHostDockletKey(const std::wstring& key) {
    return _wcsicmp(key.c_str(), L"type") == 0 ||
           _wcsicmp(key.c_str(), L"name") == 0 ||
           _wcsicmp(key.c_str(), L"label") == 0 ||
           _wcsicmp(key.c_str(), L"loaded") == 0 ||
           _wcsicmp(key.c_str(), L"show") == 0 ||
           _wcsicmp(key.c_str(), L"path") == 0 ||
           _wcsicmp(key.c_str(), L"configIniFile") == 0 ||
           _wcsicmp(key.c_str(), L"configIniGroup") == 0;
}

bool RKDockItemDockletOD::IsTempIniFile() const {
    if (configIniFile_.empty()) return false;
    const wchar_t* fname = PathFindFileNameW(configIniFile_.c_str());
    return fname && _wcsicmp(fname, L"dockletstemp.ini") == 0;
}

void RKDockItemDockletOD::StageTempIniForLoad() const {
    if (!IsTempIniFile()) return;
    IniFile tmp;
    tmp.SetEncoding(IniFile::Encoding::Utf16LE);
    IniSection& dstSec = tmp.AddSection(configIniGroup_.empty() ? L"DockletOD" : configIniGroup_);
    for (const auto& e : inlineIniEntries_) {
        dstSec.SetString(e.key, e.value);
    }
    tmp.Save(ResolveAppPath(configIniFile_), L"; Temporary file generated by RK Launcher");
}

bool RKDockItemDockletOD::LoadConfig(const IniSection& sec) {
    RKDockItem::LoadConfig(sec);
    pluginName_     = sec.GetString(L"name", pluginName_);
    std::wstring lbl = sec.GetString(L"label", L"");
    if (!lbl.empty()) {
        label_ = lbl;
    } else if (std::wstring ttl = sec.GetString(L"Title", L""); !ttl.empty()) {
        label_ = ttl;
    }
    loaded_         = sec.GetBool(L"loaded", true);
    dllPath_        = sec.GetString(L"path", dllPath_);
    configIniFile_  = sec.GetString(L"configIniFile", L"dockletstemp.ini");
    if (configIniFile_.empty()) configIniFile_ = L"dockletstemp.ini";
    configIniGroup_ = sec.GetString(L"configIniGroup", L"DockletOD");
    if (configIniGroup_.empty()) configIniGroup_ = L"DockletOD";

    inlineIniEntries_.clear();
    IniSection dedup;
    for (const auto& e : sec.entries) {
        if (!IsHostDockletKey(e.key)) {
            dedup.SetString(e.key, e.value);
        }
    }
    inlineIniEntries_ = std::move(dedup.entries);

    if (g_hostDockHwnd && !dllPath_.empty()) {
        StageTempIniForLoad();
        return InitializePlugin(g_hostDockHwnd, dllPath_, configIniFile_, configIniGroup_);
    }
    return !dllPath_.empty();
}

void RKDockItemDockletOD::SaveConfig(IniSection& sec) const {
    sec.SetString(L"type", TypeString());
    if (!pluginName_.empty()) sec.SetString(L"name", pluginName_);
    else if (!label_.empty()) sec.SetString(L"name", label_);
    if (!label_.empty()) sec.SetString(L"label", label_);
    sec.SetBool(L"loaded", loaded_);
    if (!dllPath_.empty()) sec.SetString(L"path", MakeRelativeIfInsideAppDir(dllPath_));
    if (!configIniFile_.empty()) sec.SetString(L"configIniFile", MakeRelativeIfInsideAppDir(configIniFile_));
    if (!configIniGroup_.empty()) sec.SetString(L"configIniGroup", configIniGroup_);

    if (fnOnSave_ && pluginData_) {
        std::wstring absIni = ResolveAppPath(configIniFile_);
        std::wstring grpW = configIniGroup_.empty() ? L"DockletOD" : configIniGroup_;
        if (IsTempIniFile()) {
            IniFile cleanTmp;
            cleanTmp.SetEncoding(IniFile::Encoding::Utf16LE);
            cleanTmp.AddSection(grpW);
            cleanTmp.Save(absIni, L"; Temporary file generated by RK Launcher");
        } else {
            wchar_t dirBuf[MAX_PATH] = {};
            wcsncpy_s(dirBuf, absIni.c_str(), _TRUNCATE);
            PathRemoveFileSpecW(dirBuf);
            if (*dirBuf) SHCreateDirectoryExW(nullptr, dirBuf, nullptr);
        }
        std::string iniA = WideToUtf8(absIni);
        std::string grpA = WideToUtf8(grpW);
        fnOnSave_(pluginData_, const_cast<char*>(iniA.c_str()), const_cast<char*>(grpA.c_str()), FALSE);

        if (IsTempIniFile()) {
            IniFile savedTmp;
            if (savedTmp.Load(absIni)) {
                if (const IniSection* savedSec = savedTmp.FindSection(grpW)) {
                    inlineIniEntries_ = savedSec->entries;
                }
            }
        }
    }
    for (const auto& e : inlineIniEntries_) {
        if (!IsHostDockletKey(e.key)) {
            sec.SetString(e.key, e.value);
        }
    }
}

void RKDockItemDockletOD::Execute(HWND /*hwndDock*/) {
    if (fnOnLeftClick_ && pluginData_) {
        POINT pt{0, 0};
        SIZE sz{ localRect.right - localRect.left, localRect.bottom - localRect.top };
        fnOnLeftClick_(pluginData_, &pt, &sz);
    }
}

void RKDockItemDockletOD::OnDoubleClick(HWND hwndDock) {
    if (fnOnDoubleClick_ && pluginData_) {
        POINT pt{0, 0};
        SIZE sz{ localRect.right - localRect.left, localRect.bottom - localRect.top };
        if (fnOnDoubleClick_(pluginData_, &pt, &sz)) return;
    }
    Execute(hwndDock);
}

bool RKDockItemDockletOD::TryPluginRightClick() {
    if (fnOnRightClick_ && pluginData_) {
        POINT pt{0, 0};
        SIZE sz{ localRect.right - localRect.left, localRect.bottom - localRect.top };
        return fnOnRightClick_(pluginData_, &pt, &sz) != FALSE;
    }
    return false;
}

void RKDockItemDockletOD::OnRightClick(HWND hwndDock, POINT screenPt) {
    if (TryPluginRightClick()) return;
    RKDockItem::OnRightClick(hwndDock, screenPt);
}

bool RKDockItemDockletOD::AcceptsDrop() const {
    return fnOnAcceptDropFiles_ && pluginData_ && fnOnAcceptDropFiles_(pluginData_);
}

void RKDockItemDockletOD::OnDropFiles(HWND /*hwndDock*/, HDROP hDrop) {
    if (fnOnDropFiles_ && pluginData_) {
        fnOnDropFiles_(pluginData_, hDrop);
    }
}

void RKDockItemDockletOD::ConfigurePlugin() {
    if (fnOnConfigure_ && pluginData_) {
        fnOnConfigure_(pluginData_);
    }
}

// ============================================================================
// itemlist.conf load / save & standalone default items
// ============================================================================

bool LoadItemListFile(const std::wstring& itemlistPath, HWND hwndDock,
                      std::vector<std::unique_ptr<RKDockItem>>& outItems)
{
    RKDockItemDockletOD::RegisterHostDockHwnd(hwndDock);
    outItems.clear();
    IniFile ini;
    if (!ini.Load(itemlistPath)) return false;

    for (const auto& sec : ini.Sections()) {
        if (_wcsicmp(sec.name.c_str(), L"RKDockItem") != 0) continue;
        std::wstring typeStr = ToLower(sec.GetString(L"type", L"shortcut"));
        std::unique_ptr<RKDockItem> item;

        if (typeStr == L"shortcut") {
            item = std::make_unique<RKDockItemShortcut>();
        } else if (typeStr == L"separator") {
            item = std::make_unique<RKDockItemSeparator>();
        } else if (typeStr == L"folder") {
            item = std::make_unique<RKDockItemFolder>();
        } else if (typeStr == L"docklet_od") {
            auto od = std::make_unique<RKDockItemDockletOD>();
            if (od->LoadConfig(sec)) {
                item = std::move(od);
            }
            if (item) outItems.push_back(std::move(item));
            continue;
        } else if (typeStr == L"docklet_yz") {
            // Explicitly excluded per user preference ("YzDocklet.dll可以不要支持了")
            DebugLog(L"Ignoring Y'z Docklet entry per configuration: %s", sec.GetString(L"path").c_str());
            continue;
        } else if (typeStr == L"window" || typeStr == L"application") {
            continue;
        } else {
            item = std::make_unique<RKDockItemShortcut>();
        }

        if (item) {
            item->LoadConfig(sec);
            outItems.push_back(std::move(item));
        }
    }
    return true;
}

bool SaveItemListFile(const std::wstring& itemlistPath,
                      const std::vector<std::unique_ptr<RKDockItem>>& items)
{
    IniFile ini;
    for (const auto& it : items) {
        if (!it || it->Type() == DockItemType::Window) continue;
        IniSection& sec = ini.AddSection(L"RKDockItem");
        it->SaveConfig(sec);
    }
    return ini.Save(itemlistPath, L"; RK Launcher dock items");
}

void PopulateDefaultStandaloneItems(std::vector<std::unique_ptr<RKDockItem>>& outItems) {
    outItems.clear();

    auto makeShortcut = [](const std::wstring& name, const std::wstring& link, const std::wstring& args = L"") {
        auto s = std::make_unique<RKDockItemShortcut>();
        s->SetLabel(name);
        s->link_ = link;
        s->arguments_ = args;
        s->ReloadIcon();
        return s;
    };

    outItems.push_back(makeShortcut(L"File Explorer", L"explorer.exe"));
    outItems.push_back(makeShortcut(L"Command Prompt", L"cmd.exe"));
    outItems.push_back(std::make_unique<RKDockItemSeparator>());
    outItems.push_back(makeShortcut(L"Notepad", L"notepad.exe"));
    outItems.push_back(makeShortcut(L"Calculator", L"calc.exe"));
    outItems.push_back(std::make_unique<RKDockItemSeparator>());

    wchar_t docsPath[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docsPath))) {
        auto f = std::make_unique<RKDockItemFolder>();
        f->SetLabel(L"Documents");
        f->link_ = docsPath;
        f->ReloadIcon();
        outItems.push_back(std::move(f));
    }
}

} // namespace rk
