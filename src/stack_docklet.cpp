#include "stack_docklet_internal.h"
#include <knownfolders.h>
#include <propkey.h>
#include <cstdio>

namespace stackdock {

namespace {

HMODULE g_hModule = nullptr;
std::mutex g_gdipMtx;
ULONG_PTR g_gdipToken = 0;
int g_gdipRefCount = 0;

void AcquireGdiplus() {
    std::lock_guard<std::mutex> lk(g_gdipMtx);
    if (g_gdipRefCount == 0) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&g_gdipToken, &input, nullptr) != Gdiplus::Ok) {
            g_gdipToken = 0;
        }
    }
    ++g_gdipRefCount;
}

void ReleaseGdiplus() {
    std::lock_guard<std::mutex> lk(g_gdipMtx);
    if (g_gdipRefCount > 0) {
        --g_gdipRefCount;
        if (g_gdipRefCount == 0 && g_gdipToken != 0) {
            Gdiplus::GdiplusShutdown(g_gdipToken);
            g_gdipToken = 0;
        }
    }
}

std::wstring TrimStr(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

std::wstring GetModuleDirectory(HMODULE hMod) {
    wchar_t buf[MAX_PATH] = {};
    if (GetModuleFileNameW(hMod, buf, MAX_PATH)) {
        PathRemoveFileSpecW(buf);
        return buf;
    }
    return {};
}

void EnsureDirTrailingBackslashRemoved(std::wstring& p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
}

std::wstring ReadIniSectionValue(const std::wstring& path, const std::wstring& section, const std::wstring& key, const std::wstring& defVal) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return defVal;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        return defVal;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(sz));
    size_t got = fread(raw.data(), 1, raw.size(), fp);
    fclose(fp);
    raw.resize(got);

    std::wstring text;
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        size_t chars = (raw.size() - 2) / 2;
        text.resize(chars);
        memcpy(text.data(), raw.data() + 2, chars * 2);
    } else if (raw.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        size_t chars = (raw.size() - 2) / 2;
        text.resize(chars);
        for (size_t i = 0; i < chars; ++i) {
            text[i] = static_cast<wchar_t>((raw[2 + i * 2] << 8) | raw[3 + i * 2]);
        }
    } else if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        std::string s(reinterpret_cast<const char*>(raw.data() + 3), raw.size() - 3);
        text = DecodeHostString(s.c_str());
    } else {
        std::string s(reinterpret_cast<const char*>(raw.data()), raw.size());
        text = DecodeHostString(s.c_str());
    }

    bool inTarget = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = TrimStr(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']' && line.size() >= 2) {
            std::wstring secName = TrimStr(line.substr(1, line.size() - 2));
            inTarget = (_wcsicmp(secName.c_str(), section.c_str()) == 0);
            continue;
        }
        if (!inTarget) continue;
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) {
            std::wstring k = TrimStr(line.substr(0, eq));
            if (_wcsicmp(k.c_str(), key.c_str()) == 0) {
                return TrimStr(line.substr(eq + 1));
            }
        }
    }
    return defVal;
}

int ReadIniSectionInt(const std::wstring& path, const std::wstring& section, const std::wstring& key, int defVal) {
    std::wstring s = ReadIniSectionValue(path, section, key, L"");
    if (s.empty()) return defVal;
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    return (end != s.c_str()) ? static_cast<int>(v) : defVal;
}

void EnsureIniFileUtf16BomIfMissing(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 0) {
        return;
    }
    wchar_t dirBuf[MAX_PATH] = {};
    wcsncpy_s(dirBuf, path.c_str(), _TRUNCATE);
    PathRemoveFileSpecW(dirBuf);
    if (*dirBuf) SHCreateDirectoryExW(nullptr, dirBuf, nullptr);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") == 0 && fp) {
        uint16_t bom = 0xFEFF;
        fwrite(&bom, 2, 1, fp);
        fclose(fp);
    }
}

void ResolveHostApi(HostApi& api) {
    HMODULE hHost = GetModuleHandleW(nullptr);
    if (!hHost) return;
    api.setImage          = reinterpret_cast<HostApi::FnSetImage>(GetProcAddress(hHost, "DockletSetImage"));
    api.setImageFile      = reinterpret_cast<HostApi::FnSetImageFile>(GetProcAddress(hHost, "DockletSetImageFile"));
    api.getRect           = reinterpret_cast<HostApi::FnGetRect>(GetProcAddress(hHost, "DockletGetRect"));
    api.getLabel          = reinterpret_cast<HostApi::FnGetLabel>(GetProcAddress(hHost, "DockletGetLabel"));
    api.setLabel          = reinterpret_cast<HostApi::FnSetLabel>(GetProcAddress(hHost, "DockletSetLabel"));
    api.getRelativeFolder = reinterpret_cast<HostApi::FnGetFolder>(GetProcAddress(hHost, "DockletGetRelativeFolder"));
    api.getRootFolder     = reinterpret_cast<HostApi::FnGetFolder>(GetProcAddress(hHost, "DockletGetRootFolder"));
    api.queryDockEdge     = reinterpret_cast<HostApi::FnQueryEdge>(GetProcAddress(hHost, "DockletQueryDockEdge"));
    api.lockMouseEffect   = reinterpret_cast<HostApi::FnLockMouse>(GetProcAddress(hHost, "DockletLockMouseEffect"));
    api.browseForImage    = reinterpret_cast<HostApi::FnBrowseForImage>(GetProcAddress(hHost, "DockletBrowseForImage"));
}

bool HasSparseTopLeftContent(Gdiplus::Bitmap* bmp) {
    if (!bmp) return true;
    UINT w = bmp->GetWidth(), h = bmp->GetHeight();
    if (w < 128 || h < 128) return false;
    Gdiplus::Rect r(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    Gdiplus::BitmapData bd{};
    if (bmp->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok) {
        return false;
    }
    UINT maxX = 0, maxY = 0;
    const BYTE* base = static_cast<const BYTE*>(bd.Scan0);
    for (UINT y = 0; y < h; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(y) * bd.Stride);
        for (UINT x = 0; x < w; ++x) {
            if ((row[x] >> 24) > 8) {
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
            }
        }
    }
    bmp->UnlockBits(&bd);
    return (maxX <= 64 && maxY <= 64);
}

std::shared_ptr<Gdiplus::Bitmap> ConvertHIconToBitmap(HICON hIcon) {
    if (!hIcon) return nullptr;
    ICONINFO ii{};
    if (!GetIconInfo(hIcon, &ii)) return nullptr;

    BITMAP bm{};
    if (!GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm)) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return nullptr;
    }
    int w = bm.bmWidth;
    int h = ii.hbmColor ? bm.bmHeight : (bm.bmHeight / 2);
    if (w <= 0 || h <= 0) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return nullptr;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint32_t> colorPx(static_cast<size_t>(w) * h, 0);
    HDC hdc = GetDC(nullptr);
    bool gotColor = false;
    if (ii.hbmColor) {
        gotColor = GetDIBits(hdc, ii.hbmColor, 0, static_cast<UINT>(h), colorPx.data(), &bi, DIB_RGB_COLORS) != 0;
    }
    ReleaseDC(nullptr, hdc);

    bool hasAlpha = false;
    if (gotColor) {
        for (uint32_t px : colorPx) {
            if ((px >> 24) != 0) {
                hasAlpha = true;
                break;
            }
        }
    }

    std::shared_ptr<Gdiplus::Bitmap> result;
    if (gotColor && hasAlpha) {
        // Straight ARGB source -> copy & premultiply into independent PARGB bitmap
        Gdiplus::Bitmap srcWrap(w, h, w * 4, PixelFormat32bppARGB, reinterpret_cast<BYTE*>(colorPx.data()));
        auto out = std::make_shared<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
        Gdiplus::Graphics g(out.get());
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.DrawImage(&srcWrap, 0, 0, w, h);
        result = std::move(out);
    } else {
        // Legacy icon with AND mask
        auto out = std::make_shared<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
        HDC memDC = CreateCompatibleDC(nullptr);
        void* dibBits = nullptr;
        HBITMAP hDib = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        if (hDib && dibBits) {
            HGDIOBJ old = SelectObject(memDC, hDib);
            // Draw over black and white to reconstruct alpha mask cleanly
            std::vector<uint32_t> onBlack(static_cast<size_t>(w) * h);
            std::vector<uint32_t> onWhite(static_cast<size_t>(w) * h);
            RECT rc{0, 0, w, h};
            FillRect(memDC, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            DrawIconEx(memDC, 0, 0, hIcon, w, h, 0, nullptr, DI_NORMAL);
            memcpy(onBlack.data(), dibBits, onBlack.size() * 4);

            FillRect(memDC, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            DrawIconEx(memDC, 0, 0, hIcon, w, h, 0, nullptr, DI_NORMAL);
            memcpy(onWhite.data(), dibBits, onWhite.size() * 4);
            SelectObject(memDC, old);

            std::vector<uint32_t> finalPx(static_cast<size_t>(w) * h);
            for (size_t i = 0; i < finalPx.size(); ++i) {
                uint32_t b = onBlack[i], wt = onWhite[i];
                int dr = static_cast<int>((wt >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF);
                int dg = static_cast<int>((wt >> 8) & 0xFF) - static_cast<int>((b >> 8) & 0xFF);
                int db = static_cast<int>(wt & 0xFF) - static_cast<int>(b & 0xFF);
                int alpha = 255 - (std::max)({dr, dg, db});
                alpha = std::clamp(alpha, 0, 255);
                finalPx[i] = (static_cast<uint32_t>(alpha) << 24) | (b & 0x00FFFFFF);
            }
            Gdiplus::Bitmap srcWrap(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(finalPx.data()));
            Gdiplus::Graphics g(out.get());
            g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            g.DrawImage(&srcWrap, 0, 0, w, h);
            result = std::move(out);
            DeleteObject(hDib);
        }
        DeleteDC(memDC);
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return result;
}

std::shared_ptr<Gdiplus::Bitmap> ExtractIconBySysIndex(int sysIconIdx) {
    if (sysIconIdx < 0) return nullptr;

    auto tryList = [&](int imageListType) -> std::shared_ptr<Gdiplus::Bitmap> {
        IImageList* piml = nullptr;
        if (FAILED(SHGetImageList(imageListType, IID_IImageList, reinterpret_cast<void**>(&piml))) || !piml) {
            return nullptr;
        }
        HICON hIcon = nullptr;
        std::shared_ptr<Gdiplus::Bitmap> res;
        if (SUCCEEDED(piml->GetIcon(sysIconIdx, ILD_TRANSPARENT, &hIcon)) && hIcon) {
            res = ConvertHIconToBitmap(hIcon);
            DestroyIcon(hIcon);
        }
        piml->Release();
        return res;
    };

    if (auto jumbo = tryList(SHIL_JUMBO)) {
        if (!HasSparseTopLeftContent(jumbo.get())) return jumbo;
    }
    return tryList(SHIL_EXTRALARGE);
}

std::shared_ptr<Gdiplus::Bitmap> ExtractIconForFilePath(const std::wstring& path, std::wstring* outTypeName, std::wstring* outDisplayName) {
    SHFILEINFOW sfi{};
    UINT flags = SHGFI_SYSICONINDEX | SHGFI_TYPENAME;
    if (outDisplayName) flags |= SHGFI_DISPLAYNAME;
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags)) {
        if (outTypeName) *outTypeName = sfi.szTypeName;
        if (outDisplayName && *sfi.szDisplayName) *outDisplayName = sfi.szDisplayName;
        if (auto bmp = ExtractIconBySysIndex(sfi.iIcon)) return bmp;
    }
    SHFILEINFOW sfiFallback{};
    if (SHGetFileInfoW(path.c_str(), 0, &sfiFallback, sizeof(sfiFallback), SHGFI_ICON | SHGFI_LARGEICON) && sfiFallback.hIcon) {
        auto bmp = ConvertHIconToBitmap(sfiFallback.hIcon);
        DestroyIcon(sfiFallback.hIcon);
        return bmp;
    }
    return nullptr;
}

std::shared_ptr<Gdiplus::Bitmap> ExtractIconForPidl(LPITEMIDLIST pidl, std::wstring* outTypeName) {
    if (!pidl) return nullptr;
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfi, sizeof(sfi),
                       SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_TYPENAME)) {
        if (outTypeName) *outTypeName = sfi.szTypeName;
        if (auto bmp = ExtractIconBySysIndex(sfi.iIcon)) return bmp;
    }
    SHFILEINFOW sfiFallback{};
    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfiFallback, sizeof(sfiFallback),
                       SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON) && sfiFallback.hIcon) {
        auto bmp = ConvertHIconToBitmap(sfiFallback.hIcon);
        DestroyIcon(sfiFallback.hIcon);
        return bmp;
    }
    return nullptr;
}

void DrawFallbackFolderGlyph(Gdiplus::Graphics& g, int size) {
    float pad = size * 0.12f;
    float w = size - pad * 2.0f;
    Gdiplus::GraphicsPath tabPath;
    AddRoundedRectPath(tabPath, pad, pad + w * 0.05f, w * 0.42f, w * 0.24f, 6.0f);
    Gdiplus::SolidBrush tabBrush(Gdiplus::Color(240, 224, 165, 58));
    g.FillPath(&tabBrush, &tabPath);

    Gdiplus::GraphicsPath body;
    AddRoundedRectPath(body, pad, pad + w * 0.16f, w, w * 0.72f, 8.0f);
    Gdiplus::LinearGradientBrush bg(
        Gdiplus::PointF(0, pad + w * 0.16f),
        Gdiplus::PointF(0, pad + w * 0.88f),
        Gdiplus::Color(245, 250, 206, 98),
        Gdiplus::Color(245, 212, 146, 38));
    g.FillPath(&bg, &body);
    Gdiplus::Pen rim(Gdiplus::Color(150, 255, 255, 255), 1.5f);
    g.DrawPath(&rim, &body);
}

void SortStackEntries(std::vector<StackEntry>& entries, int sortMode) {
    auto cmpTimeDesc = [](const FILETIME& a, const FILETIME& b) -> int {
        LONG c = CompareFileTime(&a, &b);
        if (c > 0) return -1;
        if (c < 0) return 1;
        return 0;
    };
    std::sort(entries.begin(), entries.end(), [&](const StackEntry& a, const StackEntry& b) {
        if (sortMode == SORT_DATE_CREATED) {
            int c = cmpTimeDesc(a.ftCreation, b.ftCreation);
            if (c != 0) return c < 0;
        } else if (sortMode == SORT_DATE_MODIFIED) {
            int c = cmpTimeDesc(a.ftLastWrite, b.ftLastWrite);
            if (c != 0) return c < 0;
        } else if (sortMode == SORT_DATE_ACCESSED) {
            int c = cmpTimeDesc(a.ftLastAccess, b.ftLastAccess);
            if (c != 0) return c < 0;
        } else if (sortMode == SORT_KIND) {
            int c = StrCmpLogicalW(a.kindName.c_str(), b.kindName.c_str());
            if (c != 0) return c < 0;
        }
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });
}

struct CachedIconInfo {
    std::wstring path;
    FILETIME ftWrite{};
    std::wstring kindName;
    std::shared_ptr<Gdiplus::Bitmap> icon;
};

const CachedIconInfo* FindCachedIcon(const std::vector<CachedIconInfo>& cache,
                                     const std::wstring& fullPath, const FILETIME& ftWrite)
{
    if (cache.empty() || fullPath.empty()) return nullptr;
    auto it = std::lower_bound(cache.begin(), cache.end(), fullPath,
        [](const CachedIconInfo& a, const std::wstring& b) {
            return _wcsicmp(a.path.c_str(), b.c_str()) < 0;
        });
    if (it != cache.end() && _wcsicmp(it->path.c_str(), fullPath.c_str()) == 0 &&
        it->ftWrite.dwLowDateTime == ftWrite.dwLowDateTime &&
        it->ftWrite.dwHighDateTime == ftWrite.dwHighDateTime &&
        it->icon != nullptr) {
        return &(*it);
    }
    return nullptr;
}

void PopulateFromFilesystemDir(const std::wstring& dirPath, std::vector<StackEntry>& out,
                               const std::vector<CachedIconInfo>& iconCache)
{
    std::wstring search = dirPath;
    if (!search.empty() && search.back() != L'\\') search.push_back(L'\\');
    search += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (_wcsicmp(fd.cFileName, L"desktop.ini") == 0) continue;

        std::wstring full = dirPath;
        if (!full.empty() && full.back() != L'\\') full.push_back(L'\\');
        full += fd.cFileName;

        StackEntry entry;
        entry.fullPath     = full;
        entry.ftCreation   = fd.ftCreationTime;
        entry.ftLastWrite  = fd.ftLastWriteTime;
        entry.ftLastAccess = fd.ftLastAccessTime;
        entry.isFolder     = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        if (const CachedIconInfo* cached = FindCachedIcon(iconCache, full, fd.ftLastWriteTime)) {
            entry.icon     = cached->icon;
            entry.kindName = cached->kindName;
        } else {
            entry.icon = ExtractIconForFilePath(full, &entry.kindName, nullptr);
        }

        if (!entry.isFolder) {
            wchar_t stem[MAX_PATH] = {};
            wcsncpy_s(stem, fd.cFileName, _TRUNCATE);
            PathRemoveExtensionW(stem);
            entry.name = *stem ? stem : fd.cFileName;
        } else {
            entry.name = fd.cFileName;
        }
        if (entry.kindName.empty()) {
            entry.kindName = entry.isFolder ? L"Folder" : L"File";
        }
        out.push_back(std::move(entry));
        if (out.size() >= 256) break;
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void PopulateFromShellFolder(LPITEMIDLIST folderPidl, std::vector<StackEntry>& out,
                             const std::vector<CachedIconInfo>& iconCache,
                             const std::vector<StackEntry>& prevItems)
{
    if (!folderPidl) return;
    IShellFolder* psf = nullptr;
    if (FAILED(SHBindToObject(nullptr, folderPidl, nullptr, IID_IShellFolder, reinterpret_cast<void**>(&psf))) || !psf) {
        return;
    }
    IEnumIDList* penum = nullptr;
    if (SUCCEEDED(psf->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &penum)) && penum) {
        LPITEMIDLIST child = nullptr;
        ULONG fetched = 0;
        while (penum->Next(1, &child, &fetched) == S_OK && fetched == 1) {
            STRRET sr{};
            wchar_t disp[MAX_PATH] = {};
            if (SUCCEEDED(psf->GetDisplayNameOf(child, SHGDN_NORMAL, &sr))) {
                StrRetToBufW(&sr, child, disp, MAX_PATH);
            }
            SFGAOF attrs = SFGAO_FOLDER | SFGAO_HIDDEN;
            LPCITEMIDLIST relChild = child;
            psf->GetAttributesOf(1, &relChild, &attrs);
            LPITEMIDLIST fullPidl = ILCombine(folderPidl, child);
            CoTaskMemFree(child);
            if (!fullPidl || (attrs & SFGAO_HIDDEN)) {
                if (fullPidl) CoTaskMemFree(fullPidl);
                continue;
            }

            StackEntry entry;
            entry.name = *disp ? disp : L"Item";
            entry.pidlAbs = fullPidl;
            entry.isFolder = (attrs & SFGAO_FOLDER) != 0;
            wchar_t fsBuf[MAX_PATH] = {};
            if (SHGetPathFromIDListW(fullPidl, fsBuf)) {
                entry.fullPath = fsBuf;
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (GetFileAttributesExW(entry.fullPath.c_str(), GetFileExInfoStandard, &fad)) {
                    entry.ftCreation   = fad.ftCreationTime;
                    entry.ftLastWrite  = fad.ftLastWriteTime;
                    entry.ftLastAccess = fad.ftLastAccessTime;
                    if (const CachedIconInfo* cached = FindCachedIcon(iconCache, entry.fullPath, fad.ftLastWriteTime)) {
                        entry.icon     = cached->icon;
                        entry.kindName = cached->kindName;
                    }
                }
                if (!entry.icon) {
                    entry.icon = ExtractIconForFilePath(entry.fullPath, &entry.kindName, nullptr);
                }
            } else {
                SHFILEINFOW sfi{};
                if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(fullPidl), 0, &sfi, sizeof(sfi),
                                   SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_TYPENAME)) {
                    entry.kindName = sfi.szTypeName;
                    entry.sysIconIndex = sfi.iIcon;
                    if (sfi.iIcon >= 0) {
                        for (const auto& prev : prevItems) {
                            if (prev.fullPath.empty() && prev.icon &&
                                prev.sysIconIndex == sfi.iIcon &&
                                prev.pidlAbs && ILIsEqual(prev.pidlAbs, fullPidl)) {
                                entry.icon = prev.icon;
                                if (entry.kindName.empty()) entry.kindName = prev.kindName;
                                break;
                            }
                        }
                        if (!entry.icon) {
                            entry.icon = ExtractIconBySysIndex(sfi.iIcon);
                        }
                    }
                }
                if (!entry.icon) {
                    entry.icon = ExtractIconForPidl(fullPidl, entry.kindName.empty() ? &entry.kindName : nullptr);
                }
            }
            out.push_back(std::move(entry));
            if (out.size() >= 256) break;
        }
        penum->Release();
    }
    psf->Release();
}

} // namespace

StackDockletInstance::~StackDockletInstance() {
    CloseStackPopup(this, true);
    items.clear();
}

HMODULE PluginModule() {
    return g_hModule;
}

std::wstring DecodeHostString(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (n <= 0) {
        cp = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, s, -1, nullptr, 0);
    }
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(cp, flags, s, -1, out.data(), n);
    return out;
}

std::string EncodeUtf8String(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

static void NormalizeBackslashes(std::wstring& p) {
    for (wchar_t& ch : p) {
        if (ch == L'/') ch = L'\\';
    }
    EnsureDirTrailingBackslashRemoved(p);
}

std::wstring ResolvePathRelative(const StackDockletInstance* inst, const std::wstring& relOrAbs) {
    if (relOrAbs.empty()) return {};
    wchar_t expanded[MAX_PATH * 2] = {};
    DWORD expLen = ExpandEnvironmentStringsW(relOrAbs.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
    std::wstring path = (expLen > 0 && expLen < std::size(expanded)) ? expanded : relOrAbs;
    NormalizeBackslashes(path);
    if (!PathIsRelativeW(path.c_str())) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && inst && !inst->rootDir.empty()) {
            // Re-anchor migrated absolute paths like D:\System\RK Launcher\icons\... into current rootDir if missing
            std::wstring lower = path;
            for (wchar_t& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
            for (const wchar_t* marker : { L"\\docklets\\", L"\\icons\\" }) {
                size_t pos = lower.find(marker);
                if (pos != std::wstring::npos) {
                    std::wstring rel = inst->rootDir;
                    NormalizeBackslashes(rel);
                    if (!rel.empty() && rel.back() != L'\\') rel.push_back(L'\\');
                    rel += path.substr(pos + 1);
                    if (GetFileAttributesW(rel.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        return rel;
                    }
                }
            }
        }
        return path;
    }
    if (inst && !inst->rootDir.empty()) {
        std::wstring cand = inst->rootDir;
        NormalizeBackslashes(cand);
        if (!cand.empty() && cand.back() != L'\\') cand.push_back(L'\\');
        cand += path;
        NormalizeBackslashes(cand);
        if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return cand;
        }
    }
    if (inst && !inst->dllDir.empty()) {
        std::wstring cand = inst->dllDir;
        NormalizeBackslashes(cand);
        if (!cand.empty() && cand.back() != L'\\') cand.push_back(L'\\');
        cand += path;
        NormalizeBackslashes(cand);
        if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return cand;
        }
    }
    if (inst && !inst->rootDir.empty()) {
        std::wstring cand = inst->rootDir;
        NormalizeBackslashes(cand);
        if (!cand.empty() && cand.back() != L'\\') cand.push_back(L'\\');
        cand += path;
        NormalizeBackslashes(cand);
        return cand;
    }
    return path;
}

std::wstring MakeRelativePathIfInsideRoot(const StackDockletInstance* inst, const std::wstring& absPath) {
    if (!inst || inst->rootDir.empty() || absPath.empty()) return absPath;
    wchar_t rootNorm[MAX_PATH] = {};
    wchar_t pathNorm[MAX_PATH] = {};
    if (!GetFullPathNameW(inst->rootDir.c_str(), MAX_PATH, rootNorm, nullptr) ||
        !GetFullPathNameW(absPath.c_str(), MAX_PATH, pathNorm, nullptr)) {
        return absPath;
    }
    size_t rootLen = wcslen(rootNorm);
    while (rootLen > 0 && (rootNorm[rootLen - 1] == L'\\' || rootNorm[rootLen - 1] == L'/')) {
        rootNorm[--rootLen] = L'\0';
    }
    if (rootLen > 0 && _wcsnicmp(pathNorm, rootNorm, rootLen) == 0 &&
        (pathNorm[rootLen] == L'\\' || pathNorm[rootLen] == L'/')) {
        return std::wstring(pathNorm + rootLen + 1);
    }
    return absPath;
}

std::shared_ptr<Gdiplus::Bitmap> LoadBitmapFileCopy(const std::wstring& path) {
    if (path.empty()) return nullptr;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return nullptr;

    std::unique_ptr<Gdiplus::Bitmap> src(Gdiplus::Bitmap::FromFile(path.c_str(), FALSE));
    if (!src || src->GetLastStatus() != Gdiplus::Ok || src->GetWidth() == 0 || src->GetHeight() == 0) {
        return nullptr;
    }
    int w = static_cast<int>(src->GetWidth());
    int h = static_cast<int>(src->GetHeight());
    auto copy = std::make_shared<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(copy.get());
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    g.DrawImage(src.get(), 0, 0, w, h);
    return copy;
}

std::shared_ptr<Gdiplus::Bitmap> LoadImageAsset(const StackDockletInstance* inst, const wchar_t* imageName) {
    if (!inst || !imageName) return nullptr;
    std::wstring base = inst->dllDir.empty() ? GetModuleDirectory(g_hModule) : inst->dllDir;
    if (base.empty()) return nullptr;
    std::wstring path = base + L"\\Images\\" + imageName;
    return LoadBitmapFileCopy(path);
}

const std::vector<SpecialFolderInfo>& GetSpecialFolderList() {
    static const std::vector<SpecialFolderInfo> kList = {
        { L"Desktop",              CSIDL_DESKTOP,                   nullptr },
        { L"Desktop Directory",    CSIDL_DESKTOPDIRECTORY,          nullptr },
        { L"Common Desktop",       CSIDL_COMMON_DESKTOPDIRECTORY,   nullptr },
        { L"Documents",            CSIDL_PERSONAL,                  nullptr },
        { L"Downloads",            -1,                              &FOLDERID_Downloads },
        { L"My Computer",          CSIDL_DRIVES,                    nullptr },
        { L"Recycle Bin",          CSIDL_BITBUCKET,                 nullptr },
        { L"Start Menu",           CSIDL_STARTMENU,                 nullptr },
        { L"Common Start Menu",    CSIDL_COMMON_STARTMENU,          nullptr },
        { L"Favorites",            CSIDL_FAVORITES,                 nullptr },
        { L"Common Favorites",     CSIDL_COMMON_FAVORITES,          nullptr },
        { L"Recent Documents",     CSIDL_RECENT,                    nullptr },
        { L"Fonts",                CSIDL_FONTS,                     nullptr },
        { L"Network Neighborhood", CSIDL_NETHOOD,                   nullptr },
        { L"Network Top",          CSIDL_NETWORK,                   nullptr },
        { L"Printers",             CSIDL_PRINTERS,                  nullptr },
        { L"Control Panel",        CSIDL_CONTROLS,                  nullptr },
    };
    return kList;
}

LPITEMIDLIST ResolveSpecialFolderPidl(const std::wstring& name, std::wstring* outFsPath, std::wstring* outDisplayName) {
    if (outFsPath) outFsPath->clear();
    if (outDisplayName) outDisplayName->clear();
    const auto& list = GetSpecialFolderList();
    const SpecialFolderInfo* match = &list.front();
    for (const auto& info : list) {
        if (_wcsicmp(info.name, name.c_str()) == 0) {
            match = &info;
            break;
        }
    }
    if (_wcsicmp(name.c_str(), L"My Documents") == 0) {
        for (const auto& info : list) {
            if (_wcsicmp(info.name, L"Documents") == 0) {
                match = &info;
                break;
            }
        }
    }

    LPITEMIDLIST pidl = nullptr;
    if (match->knownId) {
        if (FAILED(SHGetKnownFolderIDList(*match->knownId, 0, nullptr, &pidl))) pidl = nullptr;
    } else if (match->csidl >= 0) {
        if (FAILED(SHGetFolderLocation(nullptr, match->csidl, nullptr, 0, &pidl))) pidl = nullptr;
    }
    if (outDisplayName) *outDisplayName = match->name;
    if (pidl && outFsPath) {
        wchar_t buf[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, buf)) *outFsPath = buf;
    }
    return pidl;
}

void LoadGlobalConfig(GlobalConfig& cfg) {
    cfg = GlobalConfig{};
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\StackDocklet", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }
    auto readStr = [&](const wchar_t* name, std::wstring& target) {
        wchar_t buf[256] = {};
        DWORD sz = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ) && *buf) {
            target = buf;
        }
    };
    auto readDword = [&](const wchar_t* name, DWORD defVal) -> DWORD {
        DWORD val = defVal;
        DWORD sz = sizeof(val);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<BYTE*>(&val), &sz) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            return val;
        }
        return defVal;
    };
    readStr(L"OpenFolderText", cfg.openFolderText);
    readStr(L"DMoreText", cfg.dMoreText);
    readStr(L"FontName", cfg.fontName);
    cfg.fontSize   = std::clamp(static_cast<int>(readDword(L"FontSize", 8)), 6, 36);
    cfg.fontBold   = readDword(L"FontBold", 0) != 0;
    cfg.followDock = readDword(L"FollowDock", 0) != 0;
    RegCloseKey(hKey);
}

void SaveGlobalConfig(const GlobalConfig& cfg) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\StackDocklet", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return;
    }
    auto writeStr = [&](const wchar_t* name, const std::wstring& val) {
        DWORD bytes = static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()), bytes);
    };
    auto writeDword = [&](const wchar_t* name, DWORD v) {
        RegSetValueExW(hKey, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    };
    writeStr(L"OpenFolderText", cfg.openFolderText);
    writeStr(L"DMoreText", cfg.dMoreText);
    writeStr(L"FontName", cfg.fontName);
    writeDword(L"FontSize", static_cast<DWORD>(std::clamp(cfg.fontSize, 6, 36)));
    writeDword(L"FontBold", cfg.fontBold ? 1u : 0u);
    writeDword(L"FollowDock", cfg.followDock ? 1u : 0u);
    RegCloseKey(hKey);
}

void RefreshStackItems(StackDockletInstance* inst) {
    if (!inst) return;
    ScopedCom com;
    std::vector<StackEntry> prevItems = std::move(inst->items);
    inst->items.clear();

    std::vector<CachedIconInfo> iconCache;
    iconCache.reserve(prevItems.size());
    for (const auto& prev : prevItems) {
        if (prev.icon && !prev.fullPath.empty()) {
            iconCache.push_back({ prev.fullPath, prev.ftLastWrite, prev.kindName, prev.icon });
        }
    }
    std::sort(iconCache.begin(), iconCache.end(), [](const CachedIconInfo& a, const CachedIconInfo& b) {
        return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
    });

    if (inst->folderType == FTYPE_SPECIAL) {
        std::wstring fsPath;
        LPITEMIDLIST pidl = ResolveSpecialFolderPidl(inst->folder, &fsPath, nullptr);
        if (pidl) {
            // Desktop merges both virtual shell items and desktop folder neatly via IShellFolder;
            // filesystem-backed folders like Documents/Downloads benefit from timestamps + fast scan.
            bool isVirtualOnly = fsPath.empty() || _wcsicmp(inst->folder.c_str(), L"Desktop") == 0;
            if (!isVirtualOnly) {
                PopulateFromFilesystemDir(fsPath, inst->items, iconCache);
            } else {
                PopulateFromShellFolder(pidl, inst->items, iconCache, prevItems);
            }
            CoTaskMemFree(pidl);
        }
    } else if (!inst->folder.empty()) {
        std::wstring dirPath = ResolvePathRelative(inst, inst->folder);
        DWORD attr = GetFileAttributesW(dirPath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            PopulateFromFilesystemDir(dirPath, inst->items, iconCache);
        }
    }

    SortStackEntries(inst->items, inst->sortMode);
}

void UpdateDockletIcon(StackDockletInstance* inst) {
    if (!inst || !inst->hwndDocklet || !inst->host.setImage) return;

    if (!inst->cachedBaseIconAttempted ||
        inst->cachedBaseIconKey != inst->iconPath ||
        inst->cachedBaseIconRoot != inst->rootDir) {
        std::shared_ptr<Gdiplus::Bitmap> loaded;
        if (!inst->iconPath.empty()) {
            loaded = LoadBitmapFileCopy(ResolvePathRelative(inst, inst->iconPath));
        }
        if (!loaded) loaded = LoadImageAsset(inst, L"Default icon.png");
        if (!loaded) loaded = LoadImageAsset(inst, L"Box.png");
        inst->cachedBaseIcon = std::move(loaded);
        inst->cachedBaseIconKey = inst->iconPath;
        inst->cachedBaseIconRoot = inst->rootDir;
        inst->cachedBaseIconAttempted = true;
    }
    const std::shared_ptr<Gdiplus::Bitmap>& baseIcon = inst->cachedBaseIcon;

    std::shared_ptr<Gdiplus::Bitmap> itemIcons[3];
    size_t itemIconCount = 0;
    for (const auto& it : inst->items) {
        if (it.icon && !it.isOpenFolderAction && !it.isMoreAction) {
            itemIcons[itemIconCount++] = it.icon;
            if (itemIconCount >= 3) break;
        }
    }

    const bool drawItems = !inst->popupOpen && (inst->iconStyle != ISTYLE_ALWAYS) && (itemIconCount > 0);
    const bool drawTray  = inst->popupOpen || (inst->iconStyle != ISTYLE_WHEN_OPEN) || (itemIconCount == 0);

    bool matchesCache = inst->hasCompositedIcon &&
                        (inst->cachedDrawItems == drawItems) &&
                        (inst->cachedDrawTray == drawTray) &&
                        (!drawTray || inst->cachedCompBasePtr.lock().get() == baseIcon.get());
    if (matchesCache && drawItems) {
        if (inst->cachedTopCount != itemIconCount) {
            matchesCache = false;
        } else {
            for (size_t k = 0; k < itemIconCount; ++k) {
                if (inst->cachedTopIcons[k].lock().get() != itemIcons[k].get()) {
                    matchesCache = false;
                    break;
                }
            }
        }
    }
    if (matchesCache) return;

    constexpr int kCanvas = 256;
    auto canvas = std::make_unique<Gdiplus::Bitmap>(kCanvas, kCanvas, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(canvas.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    if (drawItems) {
        if (itemIconCount >= 3) {
            g.DrawImage(itemIcons[2].get(), 64, 16, 128, 128);
        }
        if (itemIconCount >= 2) {
            g.DrawImage(itemIcons[1].get(), 48, 32, 160, 160);
        }
        if (itemIconCount >= 1) {
            g.DrawImage(itemIcons[0].get(), 32, 48, 192, 192);
        }
    }
    if (drawTray) {
        if (baseIcon) {
            g.DrawImage(baseIcon.get(), 0, 0, kCanvas, kCanvas);
        } else {
            DrawFallbackFolderGlyph(g, kCanvas);
        }
    }

    inst->hasCompositedIcon = true;
    inst->cachedDrawItems   = drawItems;
    inst->cachedDrawTray    = drawTray;
    inst->cachedCompBasePtr = drawTray ? std::weak_ptr<Gdiplus::Bitmap>(baseIcon)
                                       : std::weak_ptr<Gdiplus::Bitmap>();
    inst->cachedTopCount    = drawItems ? itemIconCount : 0;
    for (size_t k = 0; k < 3; ++k) {
        inst->cachedTopIcons[k] = (drawItems && k < itemIconCount)
                                      ? std::weak_ptr<Gdiplus::Bitmap>(itemIcons[k])
                                      : std::weak_ptr<Gdiplus::Bitmap>();
    }

    inst->host.setImage(inst->hwndDocklet, canvas.get(), FALSE);
}

void UpdateDockletLabelFromConfig(StackDockletInstance* inst, bool forceIfNoCustomTitle) {
    if (!inst || !inst->hwndDocklet || !inst->host.setLabel) return;
    if (!inst->title.empty()) {
        std::string u8 = EncodeUtf8String(inst->title);
        inst->host.setLabel(inst->hwndDocklet, u8.c_str());
        return;
    }

    std::wstring existing;
    if (inst->host.getLabel) {
        char buf[260] = {};
        if (inst->host.getLabel(inst->hwndDocklet, buf) > 0) {
            existing = DecodeHostString(buf);
        }
    }
    if (!forceIfNoCustomTitle && !existing.empty() && _wcsicmp(existing.c_str(), L"Stack Docklet") != 0) {
        return;
    }

    std::wstring derived;
    if (inst->folderType == FTYPE_SPECIAL) {
        ResolveSpecialFolderPidl(inst->folder, nullptr, &derived);
    } else if (!inst->folder.empty()) {
        std::wstring abs = ResolvePathRelative(inst, inst->folder);
        const wchar_t* leaf = PathFindFileNameW(abs.c_str());
        derived = (leaf && *leaf) ? leaf : abs;
    }
    if (derived.empty()) derived = L"Stack Docklet";
    std::string u8 = EncodeUtf8String(derived);
    inst->host.setLabel(inst->hwndDocklet, u8.c_str());
}

bool ExecuteStackEntry(const StackDockletInstance* inst, const StackEntry& entry) {
    if (!inst) return false;
    ScopedCom com;
    if (entry.isOpenFolderAction || entry.isMoreAction) {
        return OpenFolderTarget(inst);
    }
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.hwnd   = inst->hwndDocklet;
    sei.nShow  = SW_SHOWNORMAL;

    if (entry.pidlAbs && entry.fullPath.empty()) {
        sei.fMask  = SEE_MASK_IDLIST;
        sei.lpIDList = entry.pidlAbs;
        return ShellExecuteExW(&sei) != FALSE;
    }
    if (entry.fullPath.empty()) return false;

    wchar_t dirBuf[MAX_PATH] = {};
    wcsncpy_s(dirBuf, entry.fullPath.c_str(), _TRUNCATE);
    PathRemoveFileSpecW(dirBuf);

    sei.lpFile = entry.fullPath.c_str();
    if (!entry.isFolder && *dirBuf) sei.lpDirectory = dirBuf;
    return ShellExecuteExW(&sei) != FALSE;
}

bool OpenFolderTarget(const StackDockletInstance* inst) {
    if (!inst) return false;
    ScopedCom com;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.hwnd   = inst->hwndDocklet;
    sei.nShow  = SW_SHOWNORMAL;

    if (inst->folderType == FTYPE_SPECIAL) {
        std::wstring fsPath;
        LPITEMIDLIST pidl = ResolveSpecialFolderPidl(inst->folder, &fsPath, nullptr);
        if (!pidl) return false;
        if (!fsPath.empty() && _wcsicmp(inst->folder.c_str(), L"Desktop") != 0) {
            sei.lpVerb = L"open";
            sei.lpFile = fsPath.c_str();
        } else {
            sei.fMask    = SEE_MASK_IDLIST;
            sei.lpIDList = pidl;
        }
        BOOL ok = ShellExecuteExW(&sei);
        CoTaskMemFree(pidl);
        return ok != FALSE;
    }

    if (inst->folder.empty()) return false;
    std::wstring absPath = ResolvePathRelative(inst, inst->folder);
    sei.lpVerb = L"open";
    sei.lpFile = absPath.c_str();
    return ShellExecuteExW(&sei) != FALSE;
}

} // namespace stackdock

extern "C" {

__declspec(dllexport) void __stdcall OnGetInformation(char* szName, char* szAuthor, int* piVersion, char* szNotes) {
    if (szName)    strcpy_s(szName, 256, "Stack Docklet");
    if (szAuthor)  strcpy_s(szAuthor, 256, "Matonga (x64 Port)");
    if (piVersion) *piVersion = 200;
    if (szNotes)   strcpy_s(szNotes, 512, "Displays a folder or special folder stack directly on the dock.");
}

__declspec(dllexport) void* __stdcall OnCreate(HWND hwndDocklet, HINSTANCE hInstance, char* szIni, char* szIniGroup) {
    stackdock::AcquireGdiplus();
    stackdock::ScopedCom com;

    auto* inst = new stackdock::StackDockletInstance();
    inst->hwndDocklet = hwndDocklet;
    inst->hPlugin     = hInstance ? hInstance : stackdock::PluginModule();
    stackdock::ResolveHostApi(inst->host);
    inst->dllDir      = stackdock::GetModuleDirectory(inst->hPlugin);

    if (inst->host.getRootFolder && hwndDocklet) {
        char buf[MAX_PATH * 2] = {};
        inst->host.getRootFolder(hwndDocklet, buf);
        inst->rootDir = stackdock::DecodeHostString(buf);
        while (inst->rootDir.size() > 3 &&
               (inst->rootDir.back() == L'\\' || inst->rootDir.back() == L'/')) {
            inst->rootDir.pop_back();
        }
    }

    std::wstring iniW   = stackdock::DecodeHostString(szIni);
    std::wstring groupW = stackdock::DecodeHostString(szIniGroup);
    if (groupW.empty()) groupW = L"DockletOD";

    if (!iniW.empty()) {
        inst->folder         = stackdock::ReadIniSectionValue(iniW, groupW, L"Folder", L"");
        inst->iconPath       = stackdock::ReadIniSectionValue(iniW, groupW, L"Icon", L"");
        if (inst->iconPath.empty()) {
            inst->iconPath   = stackdock::ReadIniSectionValue(iniW, groupW, L"Image", L"");
        }
        inst->title          = stackdock::ReadIniSectionValue(iniW, groupW, L"Title", L"");
        inst->mode           = std::clamp(stackdock::ReadIniSectionInt(iniW, groupW, L"Mode", 0), 0, 2);
        inst->sortMode       = std::clamp(stackdock::ReadIniSectionInt(iniW, groupW, L"Sort", 0), 0, 4);
        inst->hideOpenFolder = stackdock::ReadIniSectionInt(iniW, groupW, L"HideOpenFolder", 0) != 0;
        inst->folderType     = std::clamp(stackdock::ReadIniSectionInt(iniW, groupW, L"FolderType", 0), 0, 1);
        int rawIconStyle     = stackdock::ReadIniSectionInt(iniW, groupW, L"IconStyle", 0);
        inst->iconStyle      = std::clamp(rawIconStyle, 0, 2);
    }

    stackdock::RefreshStackItems(inst);
    stackdock::UpdateDockletIcon(inst);
    stackdock::UpdateDockletLabelFromConfig(inst, false);
    return inst;
}

__declspec(dllexport) void __stdcall OnDestroy(void* lpData, HWND /*hwndDocklet*/) {
    auto* inst = static_cast<stackdock::StackDockletInstance*>(lpData);
    delete inst;
    stackdock::ReleaseGdiplus();
}

__declspec(dllexport) void __stdcall OnSave(void* lpData, char* szIni, char* szIniGroup, BOOL /*bIsForExport*/) {
    auto* inst = static_cast<stackdock::StackDockletInstance*>(lpData);
    if (!inst || !szIni || !*szIni) return;

    std::wstring iniW   = stackdock::DecodeHostString(szIni);
    std::wstring groupW = stackdock::DecodeHostString(szIniGroup);
    if (groupW.empty()) groupW = L"DockletOD";

    stackdock::EnsureIniFileUtf16BomIfMissing(iniW);

    std::wstring savedFolder = (inst->folderType == stackdock::FTYPE_FOLDER)
        ? stackdock::MakeRelativePathIfInsideRoot(inst, inst->folder)
        : inst->folder;
    std::wstring savedIcon = stackdock::MakeRelativePathIfInsideRoot(inst, inst->iconPath);

    WritePrivateProfileStringW(groupW.c_str(), L"Folder", savedFolder.c_str(), iniW.c_str());
    WritePrivateProfileStringW(groupW.c_str(), L"Icon", savedIcon.c_str(), iniW.c_str());
    if (!inst->title.empty()) {
        WritePrivateProfileStringW(groupW.c_str(), L"Title", inst->title.c_str(), iniW.c_str());
    } else {
        WritePrivateProfileStringW(groupW.c_str(), L"Title", nullptr, iniW.c_str());
    }
    WritePrivateProfileStringW(groupW.c_str(), L"Mode", std::to_wstring(inst->mode).c_str(), iniW.c_str());
    WritePrivateProfileStringW(groupW.c_str(), L"Sort", std::to_wstring(inst->sortMode).c_str(), iniW.c_str());
    WritePrivateProfileStringW(groupW.c_str(), L"HideOpenFolder", inst->hideOpenFolder ? L"1" : L"0", iniW.c_str());
    WritePrivateProfileStringW(groupW.c_str(), L"FolderType", std::to_wstring(inst->folderType).c_str(), iniW.c_str());
    WritePrivateProfileStringW(groupW.c_str(), L"IconStyle", std::to_wstring(inst->iconStyle).c_str(), iniW.c_str());
}

__declspec(dllexport) void __stdcall OnConfigure(void* lpData) {
    auto* inst = static_cast<stackdock::StackDockletInstance*>(lpData);
    if (!inst) return;
    stackdock::CloseStackPopup(inst, true);
    if (stackdock::ShowStackConfigureDialog(inst)) {
        stackdock::RefreshStackItems(inst);
        stackdock::UpdateDockletIcon(inst);
        stackdock::UpdateDockletLabelFromConfig(inst, true);
    }
}

__declspec(dllexport) BOOL __stdcall OnLeftButtonClick(void* lpData, POINT* ptCursor, SIZE* sizeDocklet) {
    auto* inst = static_cast<stackdock::StackDockletInstance*>(lpData);
    if (!inst) return FALSE;
    return stackdock::ToggleOrShowStackPopup(inst, ptCursor, sizeDocklet) ? TRUE : FALSE;
}

__declspec(dllexport) BOOL __stdcall OnAcceptDropFiles(void* lpData) {
    auto* inst = static_cast<stackdock::StackDockletInstance*>(lpData);
    if (!inst || inst->folderType != stackdock::FTYPE_FOLDER || inst->folder.empty()) {
        return FALSE;
    }
    std::wstring dir = stackdock::ResolvePathRelative(inst, inst->folder);
    DWORD attr = GetFileAttributesW(dir.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? TRUE : FALSE;
}

__declspec(dllexport) void __stdcall OnDropFiles(void* lpData, HDROP hDrop) {
    auto* inst = static_cast<stackdock::StackDockletInstance*>(lpData);
    if (!inst || !hDrop || inst->folderType != stackdock::FTYPE_FOLDER || inst->folder.empty()) {
        return;
    }
    std::wstring destDir = stackdock::ResolvePathRelative(inst, inst->folder);
    DWORD attr = GetFileAttributesW(destDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;

    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    if (count == 0) return;

    std::wstring fromBuf;
    for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
        if (len == 0) continue;
        std::wstring item(len, L'\0');
        if (DragQueryFileW(hDrop, i, item.data(), len + 1) > 0) {
            fromBuf += item;
            fromBuf.push_back(L'\0');
        }
    }
    if (fromBuf.empty()) return;
    fromBuf.push_back(L'\0');

    std::wstring toBuf = destDir;
    toBuf.push_back(L'\0');
    toBuf.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.hwnd   = inst->hwndDocklet;
    op.wFunc  = (GetKeyState(VK_SHIFT) < 0) ? FO_MOVE : FO_COPY;
    op.pFrom  = fromBuf.c_str();
    op.pTo    = toBuf.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) {
        stackdock::RefreshStackItems(inst);
        stackdock::UpdateDockletIcon(inst);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)lpReserved;
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        stackdock::g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

} // extern "C"
