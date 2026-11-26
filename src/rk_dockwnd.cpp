#include "rk_dockwnd.h"
#include "rk_common.h"
#include "rk_hook.h"
#include "rk_dialogs.h"
#include <commdlg.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace rk {

namespace {
constexpr UINT_PTR TIMER_RENDER           = 0x20;
constexpr UINT_PTR TIMER_WINDOWSYNC       = 0x21;
constexpr UINT_PTR TIMER_HOVERCHECK       = 0x23;
constexpr UINT_PTR TIMER_INDICATOR_RECHECK = 0x24;

bool IsExternalOccludingCandidate(HWND h, HWND hwndDock) {
    if (!h || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h) || h == hwndDock) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
    }
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) {
        return false;
    }
    return true;
}

bool GetEffectiveWindowRect(HWND h, RECT& outRc) {
    if (!GetWindowRect(h, &outRc)) return false;
    RECT ext{};
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext))) &&
        ext.right > ext.left && ext.bottom > ext.top) {
        // Guard against per-monitor DPI scale mismatch (>24px delta across monitors)
        if (std::abs(ext.left - outRc.left) <= 24 &&
            std::abs(ext.top - outRc.top) <= 24 &&
            std::abs(ext.right - outRc.right) <= 24 &&
            std::abs(ext.bottom - outRc.bottom) <= 24) {
            outRc = ext;
        }
    }
    return outRc.right > outRc.left && outRc.bottom > outRc.top;
}

bool IsZeroAlphaLayeredWindow(HWND h) {
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_LAYERED) == 0) return false;
    BYTE alpha = 255;
    DWORD flags = 0;
    return GetLayeredWindowAttributes(h, nullptr, &alpha, &flags) &&
           (flags & LWA_ALPHA) != 0 && alpha == 0;
}

bool IsWindowAboveDockInZOrder(HWND hTarget, HWND hwndDock) {
    if (!hTarget || !hwndDock) return false;
    for (HWND cur = GetWindow(hwndDock, GW_HWNDPREV); cur != nullptr; cur = GetWindow(cur, GW_HWNDPREV)) {
        if (cur == hTarget) return true;
    }
    return false;
}

bool IsNormalNonTopmostOverlapping(HWND h, HWND hwndDock, const RECT& dockRc) {
    if (!IsExternalOccludingCandidate(h, hwndDock)) return false;
    if ((GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) return false;
    RECT rc{};
    if (!GetEffectiveWindowRect(h, rc)) return false;
    RECT inter{};
    return IntersectRect(&inter, &rc, &dockRc) != FALSE;
}

bool IsPointOccludedByOtherWindow(HWND hwndDock, POINT screenPt) {
    // 1. 32-bit-aligned primary check via WindowFromPoint(screenPt) climbed to root,
    //    without filtering WS_EX_TRANSPARENT or WS_EX_LAYERED so WebView2 / Electron /
    //    Chromium composition windows reliably trigger edge occlusion.
    if (HWND hAtPt = WindowFromPoint(screenPt)) {
        HWND hRoot = GetAncestor(hAtPt, GA_ROOT);
        if (!hRoot) hRoot = hAtPt;
        if (IsExternalOccludingCandidate(hRoot, hwndDock) &&
            IsWindowAboveDockInZOrder(hRoot, hwndDock)) {
            return true;
        }
    }

    // 2. Fallback along Z-order above the dock in case a frameless window's transparent
    //    hit-test border at the screen edge let WindowFromPoint slip through.
    for (HWND cur = GetWindow(hwndDock, GW_HWNDPREV); cur != nullptr; cur = GetWindow(cur, GW_HWNDPREV)) {
        if (!IsExternalOccludingCandidate(cur, hwndDock)) continue;
        LONG_PTR ex = GetWindowLongPtrW(cur, GWL_EXSTYLE);
        if ((ex & WS_EX_TRANSPARENT) != 0) continue;
        if (IsZeroAlphaLayeredWindow(cur)) continue;
        if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) == (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) continue;
        RECT rc{};
        if (GetEffectiveWindowRect(cur, rc) && PtInRect(&rc, screenPt) != FALSE) {
            return true;
        }
    }
    return false;
}

// Matches 32-bit RKLauncher 0x00452F60 fullscreen foreground guard.
bool IsFullscreenWindow32(HWND hFg, HWND hwndDock) {
    if (!hFg || !IsWindow(hFg) || !IsWindowVisible(hFg) || hFg == hwndDock || hFg == GetDesktopWindow()) {
        return false;
    }
    wchar_t cls[64] = {};
    GetClassNameW(hFg, cls, 64);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0) {
        return false;
    }
    HMONITOR hMonFg = MonitorFromWindow(hFg, MONITOR_DEFAULTTONEAREST);
    HMONITOR hMonDock = hwndDock ? MonitorFromWindow(hwndDock, MONITOR_DEFAULTTONEAREST) : hMonFg;
    if (!hMonFg || hMonFg != hMonDock) {
        return false;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonFg, &mi)) {
        return false;
    }
    RECT rc{};
    LONG style = GetWindowLongW(hFg, GWL_STYLE);
    if ((style & (WS_CAPTION | WS_THICKFRAME)) == (WS_CAPTION | WS_THICKFRAME)) {
        if (!GetClientRect(hFg, &rc)) return false;
        MapWindowPoints(hFg, nullptr, reinterpret_cast<LPPOINT>(&rc), 2);
    } else {
        if (!GetWindowRect(hFg, &rc)) return false;
    }
    RECT u{};
    UnionRect(&u, &rc, &mi.rcMonitor);
    return EqualRect(&u, &rc) != FALSE;
}

// Matches 32-bit RKLauncher 0x00452C40 AttachThreadInput + SetForegroundWindow + BringWindowToTop.
bool ForceForegroundWindow32(HWND hTarget, bool allowActivate) {
    if (!hTarget || !IsWindow(hTarget)) return false;
    if (!allowActivate) {
        SetWindowPos(hTarget, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return true;
    }
    DWORD targetTid = GetWindowThreadProcessId(hTarget, nullptr);
    HWND hFg = GetForegroundWindow();
    DWORD fgTid = hFg ? GetWindowThreadProcessId(hFg, nullptr) : 0;
    if (targetTid != fgTid && targetTid != 0 && fgTid != 0) {
        AttachThreadInput(fgTid, targetTid, TRUE);
        SetForegroundWindow(hTarget);
        AttachThreadInput(fgTid, targetTid, FALSE);
    }
    if (GetForegroundWindow() != hTarget) {
        SetForegroundWindow(hTarget);
    }
    BringWindowToTop(hTarget);
    return true;
}
} // namespace

// ============================================================================
// LayeredSurface
// ============================================================================

LayeredSurface::~LayeredSurface() {
    if (memDC_) {
        if (old_) SelectObject(memDC_, old_);
        if (bmp_) DeleteObject(bmp_);
        DeleteDC(memDC_);
    }
}

bool LayeredSurface::EnsureSize(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (memDC_ && width_ == w && height_ == h) return true;

    if (!memDC_) memDC_ = CreateCompatibleDC(nullptr);
    if (old_ && bmp_) {
        SelectObject(memDC_, old_);
        DeleteObject(bmp_);
        bmp_ = nullptr;
        old_ = nullptr;
        bits_ = nullptr;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    bmp_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!bmp_) return false;
    old_ = static_cast<HBITMAP>(SelectObject(memDC_, bmp_));
    width_ = w;
    height_ = h;
    return true;
}

void LayeredSurface::Present(HWND hwnd, int screenX, int screenY, BYTE globalAlpha) {
    if (!hwnd || !memDC_ || !bmp_) return;
    POINT ptDst{ screenX, screenY };
    SIZE  sz{ width_, height_ };
    POINT ptSrc{ 0, 0 };
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = globalAlpha;
    bf.AlphaFormat         = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd, nullptr, &ptDst, &sz, memDC_, &ptSrc, 0, &bf, ULW_ALPHA);
}

// ============================================================================
// RKDockTextWnd
// ============================================================================

LRESULT CALLBACK RKDockTextWnd::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RKDockTextWnd::Create(HWND owner) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassText;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kClassText, L"", WS_POPUP,
        0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    lastTick_ = GetTickCount();
    return hwnd_ != nullptr;
}

void RKDockTextWnd::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void RKDockTextWnd::ShowLabel(const std::wstring& text, const RECT& anchorScreenRect,
                              const RECT& monitorWorkArea, const RKDockOptions& opts)
{
    if (!hwnd_ || opts.hideLabel || text.empty()) {
        HideLabel();
        return;
    }
    bool anchorChanged = memcmp(&anchorRect_, &anchorScreenRect, sizeof(RECT)) != 0 ||
                         memcmp(&workArea_, &monitorWorkArea, sizeof(RECT)) != 0;
    bool wasVisible = (IsWindowVisible(hwnd_) != FALSE);
    text_          = text;
    anchorRect_    = anchorScreenRect;
    workArea_      = monitorWorkArea;
    visibleTarget_ = true;
    if (!opts.fadeLabel) {
        alpha01_ = 1.0f;
    } else if (!wasVisible) {
        lastTick_ = GetTickCount();
    }

    const BYTE presentAlpha = opts.fadeLabel
        ? static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(alpha01_ * 255.0f)), 0, 255))
        : 255;

    bool needsRedraw = !MatchesRenderedStyle(opts) ||
                       surf_.Width() <= 0 || surf_.Height() <= 0;
    if (needsRedraw) {
        RenderNow(opts);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    } else if (anchorChanged || !wasVisible) {
        POINT pos = ComputeLabelScreenPos(opts, surf_.Width(), surf_.Height());
        if (!wasVisible) {
            surf_.Present(hwnd_, pos.x, pos.y, presentAlpha);
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        } else {
            SetWindowPos(hwnd_, nullptr, pos.x, pos.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

void RKDockTextWnd::HideLabel() {
    visibleTarget_ = false;
    if (!hwnd_) return;
    alpha01_ = 0.0f;
    text_.clear();
    if (IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
}

void RKDockTextWnd::TickFade(const RKDockOptions& opts) {
    if (!hwnd_ || !opts.fadeLabel || !visibleTarget_ || text_.empty()) return;
    DWORD now = GetTickCount();
    DWORD dt = now - lastTick_;
    lastTick_ = now;
    if (alpha01_ < 1.0f) {
        alpha01_ = std::min(1.0f, alpha01_ + dt / 140.0f);
        if (!MatchesRenderedStyle(opts) || surf_.Width() <= 0 || surf_.Height() <= 0) {
            RenderNow(opts);
        } else {
            POINT pos = ComputeLabelScreenPos(opts, surf_.Width(), surf_.Height());
            BYTE presentAlpha = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(alpha01_ * 255.0f)), 0, 255));
            surf_.Present(hwnd_, pos.x, pos.y, presentAlpha);
        }
    }
}

bool RKDockTextWnd::MatchesRenderedStyle(const RKDockOptions& opts) const {
    return !renderedText_.empty() &&
           renderedText_ == text_ &&
           renderedFont_ == opts.font &&
           renderedFontSize_ == opts.fontSize &&
           renderedBold_ == opts.bold &&
           renderedItalic_ == opts.italic &&
           renderedColorLabel_ == opts.colorLabel &&
           renderedColorEdge_ == opts.colorEdge &&
           renderedColorShadow_ == opts.colorShadow &&
           renderedAlphaLabel_ == opts.alphaLabel &&
           renderedAlphaEdge_ == opts.alphaEdge &&
           renderedAlphaShadow_ == opts.alphaShadow;
}

POINT RKDockTextWnd::ComputeLabelScreenPos(const RKDockOptions& opts, int w, int h) const {
    // Position tightly next to anchorRect based on dock edge (matching 32-bit RK Launcher)
    constexpr int kLabelGap = 2;
    int x = (anchorRect_.left + anchorRect_.right - w) / 2;
    int y = anchorRect_.top - h - kLabelGap;
    if (IsTopEdge(opts.position)) {
        y = anchorRect_.bottom + kLabelGap;
    } else if (IsLeftEdge(opts.position)) {
        x = anchorRect_.right + kLabelGap;
        y = (anchorRect_.top + anchorRect_.bottom - h) / 2;
    } else if (IsRightEdge(opts.position)) {
        x = anchorRect_.left - w - kLabelGap;
        y = (anchorRect_.top + anchorRect_.bottom - h) / 2;
    }

    if (workArea_.right > workArea_.left) {
        x = std::clamp(x, static_cast<int>(workArea_.left) + 4, static_cast<int>(workArea_.right) - w - 4);
        y = std::clamp(y, static_cast<int>(workArea_.top) + 4, static_cast<int>(workArea_.bottom) - h - 4);
    }
    return POINT{ x, y };
}

void RKDockTextWnd::RenderNow(const RKDockOptions& opts) {
    if (!hwnd_ || text_.empty()) return;

    int fontSize = std::clamp(opts.fontSize, 7, 72);
    INT style = Gdiplus::FontStyleRegular;
    if (opts.bold && opts.italic) style = Gdiplus::FontStyleBoldItalic;
    else if (opts.bold) style = Gdiplus::FontStyleBold;
    else if (opts.italic) style = Gdiplus::FontStyleItalic;

    auto ffPtr = std::make_unique<Gdiplus::FontFamily>(opts.font.c_str());
    if (!ffPtr->IsAvailable()) ffPtr = std::make_unique<Gdiplus::FontFamily>(L"Tahoma");
    if (!ffPtr->IsAvailable()) ffPtr.reset(Gdiplus::FontFamily::GenericSansSerif()->Clone());
    Gdiplus::FontFamily& ff = *ffPtr;
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    HDC tmpDC = GetDC(nullptr);
    Gdiplus::RectF box;
    {
        Gdiplus::Graphics gMeasure(tmpDC);
        Gdiplus::Font f(&ff, static_cast<float>(fontSize), style, Gdiplus::UnitPoint);
        gMeasure.MeasureString(text_.c_str(), -1, &f, Gdiplus::PointF(0, 0), &sf, &box);
    }
    ReleaseDC(nullptr, tmpDC);

    int padX = 8;
    int padY = 4;
    int w = std::max(24, static_cast<int>(std::ceil(box.Width)) + padX * 2);
    int h = std::max(18, static_cast<int>(std::ceil(box.Height)) + padY * 2);

    if (!surf_.EnsureSize(w, h)) return;
    {
        Gdiplus::Graphics g(surf_.MemoryDC());
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        Gdiplus::RectF textRc(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
        RKDockTheme::RenderStyledLabel(g, text_, textRc, opts, 1.0f);
    }

    renderedText_        = text_;
    renderedFont_        = opts.font;
    renderedFontSize_    = opts.fontSize;
    renderedBold_        = opts.bold;
    renderedItalic_      = opts.italic;
    renderedColorLabel_  = opts.colorLabel;
    renderedColorEdge_   = opts.colorEdge;
    renderedColorShadow_ = opts.colorShadow;
    renderedAlphaLabel_  = opts.alphaLabel;
    renderedAlphaEdge_   = opts.alphaEdge;
    renderedAlphaShadow_ = opts.alphaShadow;

    POINT pos = ComputeLabelScreenPos(opts, w, h);
    BYTE presentAlpha = opts.fadeLabel
        ? static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(alpha01_ * 255.0f)), 0, 255))
        : 255;
    surf_.Present(hwnd_, pos.x, pos.y, presentAlpha);
}

// ============================================================================
// RKDockDragWnd
// ============================================================================

LRESULT CALLBACK RKDockDragWnd::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RKDockDragWnd::Create(HWND owner) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassDrag;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kClassDrag, L"", WS_POPUP,
        0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    return hwnd_ != nullptr;
}

void RKDockDragWnd::Destroy() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void RKDockDragWnd::Begin(Gdiplus::Bitmap* bmp, int sizePx, POINT screenPt) {
    bmp_ = bmp;
    size_ = std::clamp(sizePx, 24, 160);
    deleteMode_ = false;
    if (!hwnd_) return;
    Render(screenPt);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void RKDockDragWnd::Move(POINT screenPt, bool willDelete) {
    deleteMode_ = willDelete;
    if (hwnd_) Render(screenPt);
}

void RKDockDragWnd::End() {
    bmp_ = nullptr;
    if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
}

void RKDockDragWnd::Render(POINT screenPt) {
    if (!hwnd_ || !surf_.EnsureSize(size_, size_)) return;
    {
        Gdiplus::Graphics g(surf_.MemoryDC());
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        if (bmp_) {
            Gdiplus::ImageAttributes attr;
            Gdiplus::ColorMatrix cm = {
                1, 0, 0, 0, 0,
                0, 1, 0, 0, 0,
                0, 0, 1, 0, 0,
                0, 0, 0, deleteMode_ ? 0.50f : 0.88f, 0,
                0, 0, 0, 0, 1
            };
            attr.SetColorMatrix(&cm);
            g.DrawImage(bmp_, Gdiplus::Rect(0, 0, size_, size_), 0, 0,
                        bmp_->GetWidth(), bmp_->GetHeight(), Gdiplus::UnitPixel, &attr);
        }
    }
    surf_.Present(hwnd_, screenPt.x - size_ / 2, screenPt.y - size_ / 2, 255);
}

// ============================================================================
// RKDockEffectWnd
// ============================================================================

LRESULT CALLBACK RKDockEffectWnd::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RKDockEffectWnd::Create(HWND owner) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassEffect;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kClassEffect, L"", WS_POPUP,
        0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    return hwnd_ != nullptr;
}

void RKDockEffectWnd::Destroy() {
    playing_ = false;
    StopMinMax();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void RKDockEffectWnd::StopMinMax() {
    effPlaying_ = false;
    if (effCtx_ && fnEffDestroy_) {
        fnEffDestroy_(effCtx_);
    }
    effCtx_ = nullptr;
    effSrcBmp_.reset();
    fnEffNext_ = nullptr;
    fnEffDestroy_ = nullptr;
    if (hwnd_ && !playing_ && IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

static std::unique_ptr<Gdiplus::Bitmap> CaptureWindowSnapshot(HWND target) {
    RECT rc{};
    if (!target || !IsWindow(target) || IsIconic(target) || !GetWindowRect(target, &rc)) {
        return nullptr;
    }
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return nullptr;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return nullptr;
    HDC memDC = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, w, h);
    std::unique_ptr<Gdiplus::Bitmap> outBmp;

    if (memDC && hbm) {
        HGDIOBJ old = SelectObject(memDC, hbm);
        constexpr UINT kPwRenderFullContent = 2;
        BOOL ok = PrintWindow(target, memDC, kPwRenderFullContent);
        if (!ok) ok = PrintWindow(target, memDC, 0);
        if (!ok) {
            BitBlt(memDC, 0, 0, w, h, hdcScreen, rc.left, rc.top, SRCCOPY | CAPTUREBLT);
        }
        SelectObject(memDC, old);

        std::unique_ptr<Gdiplus::Bitmap> tmp(Gdiplus::Bitmap::FromHBITMAP(hbm, nullptr));
        if (tmp && tmp->GetLastStatus() == Gdiplus::Ok) {
            // Downscale large windows slightly for snappy, smooth effect animation
            int maxDim = 512;
            int dw = w, dh = h;
            if (dw > maxDim || dh > maxDim) {
                double scale = static_cast<double>(maxDim) / std::max(dw, dh);
                dw = std::max(1, static_cast<int>(std::round(dw * scale)));
                dh = std::max(1, static_cast<int>(std::round(dh * scale)));
            }
            auto scaled = std::make_unique<Gdiplus::Bitmap>(dw, dh, PixelFormat32bppPARGB);
            if (scaled && scaled->GetLastStatus() == Gdiplus::Ok) {
                Gdiplus::Graphics g(scaled.get());
                g.SetInterpolationMode(Gdiplus::InterpolationModeLowQuality);
                g.DrawImage(tmp.get(), Gdiplus::Rect(0, 0, dw, dh), 0, 0, w, h, Gdiplus::UnitPixel);
                outBmp = std::move(scaled);
            }
        }
    }

    if (hbm) DeleteObject(hbm);
    if (memDC) DeleteDC(memDC);
    ReleaseDC(nullptr, hdcScreen);
    return outBmp;
}

void RKDockEffectWnd::StartMinMax(
    HWND target,
    const RECT& dockIconScreenRect,
    FnEffectCreate fnCreate,
    FnEffectNextStep fnNext,
    FnEffectDestroy fnDestroy)
{
    if (!hwnd_ || !target || !fnCreate || !fnNext || !fnDestroy) return;
    StopMinMax();
    playing_ = false;

    std::unique_ptr<Gdiplus::Bitmap> snap = CaptureWindowSnapshot(target);
    if (!snap) return;

    void* ctx = fnCreate(target, snap.get(), 0, 0.0f);
    if (!ctx) return;

    effSrcBmp_ = std::move(snap);
    effCtx_ = ctx;
    fnEffNext_ = fnNext;
    fnEffDestroy_ = fnDestroy;
    int dw = std::max(16, static_cast<int>(dockIconScreenRect.right - dockIconScreenRect.left));
    int dh = std::max(16, static_cast<int>(dockIconScreenRect.bottom - dockIconScreenRect.top));
    effDstRect_ = Gdiplus::Rect(dockIconScreenRect.left, dockIconScreenRect.top, dw, dh);
    lastFrameTick_ = GetTickCount() - 40;
    effPlaying_ = true;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Tick();
}

void RKDockEffectWnd::StartPoof(POINT screenCenter, int sizePx, RKDockTheme* theme) {
    if (!hwnd_ || !theme) return;
    StopMinMax();
    theme_ = theme;
    center_ = screenCenter;
    size_ = std::clamp(sizePx, 64, 192);
    frame_ = 0;
    totalFrames_ = std::max(1, theme_->GetPoofFrameCount());
    lastFrameTick_ = GetTickCount() - 40;
    playing_ = true;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Tick();
}

void RKDockEffectWnd::Tick() {
    if (!hwnd_) return;
    DWORD now = GetTickCount();

    if (effPlaying_ && effCtx_ && fnEffNext_) {
        if (now - lastFrameTick_ < 20) return;
        lastFrameTick_ = now;

        Gdiplus::Bitmap* stepBmp = nullptr;
        BYTE stepAlpha = 255;
        RECT stepRc{};
        float pct = 0.0f;
        BOOL more = fnEffNext_(effCtx_, &stepBmp, &stepAlpha, &effDstRect_, &stepRc, &pct);

        if (stepBmp && stepBmp->GetLastStatus() == Gdiplus::Ok) {
            int w = std::max(1, static_cast<int>(stepRc.right - stepRc.left));
            int h = std::max(1, static_cast<int>(stepRc.bottom - stepRc.top));
            if (surf_.EnsureSize(w, h)) {
                Gdiplus::Graphics g(surf_.MemoryDC());
                g.Clear(Gdiplus::Color(0, 0, 0, 0));
                g.DrawImage(stepBmp, Gdiplus::Rect(0, 0, w, h),
                            0, 0, stepBmp->GetWidth(), stepBmp->GetHeight(),
                            Gdiplus::UnitPixel);
                surf_.Present(hwnd_, stepRc.left, stepRc.top, stepAlpha);
            }
        }
        delete stepBmp;

        if (!more) {
            StopMinMax();
        }
        return;
    }

    if (!playing_ || !theme_) return;
    if (now - lastFrameTick_ < 32) return;
    lastFrameTick_ = now;

    if (frame_ >= totalFrames_) {
        playing_ = false;
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }

    if (surf_.EnsureSize(size_, size_)) {
        Gdiplus::Graphics g(surf_.MemoryDC());
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        theme_->DrawPoofFrame(g, Gdiplus::Rect(0, 0, size_, size_), frame_);
        surf_.Present(hwnd_, center_.x - size_ / 2, center_.y - size_ / 2, 255);
    }
    ++frame_;
}

// ============================================================================
// RKDockWnd
// ============================================================================

RKDockWnd::RKDockWnd() = default;

RKDockWnd::~RKDockWnd() {
    Destroy();
}

bool RKDockWnd::Create() {
    msgHookMouse_      = RegisterWindowMessageW(kMsgHookMouseName);
    msgHookCbt_        = RegisterWindowMessageW(kMsgHookCbtName);
    msgTaskbarCreated_ = RegisterWindowMessageW(kMsgTaskbarCreated);
    msgShellHook_      = RegisterWindowMessageW(kMsgShellHook);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = DockWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(128));
    wc.lpszClassName = kClassDock;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (options_.neverTakeFocus) exStyle |= WS_EX_NOACTIVATE;
    if (options_.alwaysOnTop) exStyle |= WS_EX_TOPMOST;

    hwnd_ = CreateWindowExW(exStyle, kClassDock, L"RK Launcher", WS_POPUP,
                            0, 0, 200, 80, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    RKDockItemDockletOD::RegisterHostDockHwnd(hwnd_);
    textWnd_.Create(hwnd_);
    dragWnd_.Create(hwnd_);
    effectWnd_.Create(hwnd_);

    LoadConfigAndItems();
    RegisterShellHookWindow(hwnd_);
    RegisterDragDrop(hwnd_, this);
    DragAcceptFiles(hwnd_, TRUE);
    InstallHooks();

    QueryPerformanceFrequency(&qpcFreq_);
    ApplyOptionsAndReload(true);
    DWORD now = GetTickCount();
    mouseLeaveTick_ = now;
    autoWakeTick_   = now;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    if (NeedActiveRenderTimer()) EnsureRenderTimer();
    SetTimer(hwnd_, TIMER_WINDOWSYNC, 1000, nullptr);
    SetHoverTimerInterval(25);
    return true;
}

void RKDockWnd::Destroy() {
    if (destroyed_) return;
    destroyed_ = true;

    StopRenderTimer();
    if (hwnd_) {
        KillTimer(hwnd_, TIMER_WINDOWSYNC);
        KillTimer(hwnd_, TIMER_HOVERCHECK);
        hoverTimerIntervalMs_ = 0;
        KillTimer(hwnd_, TIMER_INDICATOR_RECHECK);
        RevokeDragDrop(hwnd_);
        DeregisterShellHookWindow(hwnd_);
    }

    if (options_.saveOnExit) {
        SaveAllConfig();
    }

    RestoreDockMinimizedWindows();
    if (taskbarHiddenByUs_) {
        HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (hTray) ShowWindow(hTray, SW_SHOWNA);
        taskbarHiddenByUs_ = false;
    }

    UninstallHooks();
    effectWnd_.Destroy();
    if (hEffectDll_) {
        FreeLibrary(hEffectDll_);
        hEffectDll_ = nullptr;
    }

    items_.clear();
    dragWnd_.Destroy();
    textWnd_.Destroy();

    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
}

void RKDockWnd::LoadConfigAndItems() {
    std::wstring confPath = ResolveAppPath(L"RKLauncher.conf");
    options_.LoadFromFile(confPath);

    std::wstring exclPath = ResolveAppPath(L"excluded.conf");
    options_.LoadExclusions(exclPath);

    RKLangManager::Instance().LoadLanguage(options_.langResource);

    std::wstring itemsPath = ResolveAppPath(L"itemlist.conf");
    if (!LoadItemListFile(itemsPath, hwnd_, items_) || items_.empty()) {
        if (!FileExists(itemsPath)) {
            PopulateDefaultStandaloneItems(items_);
        }
    }
}

void RKDockWnd::SaveAllConfig() {
    options_.SaveToFile(ResolveAppPath(L"RKLauncher.conf"));
    options_.SaveExclusions(ResolveAppPath(L"excluded.conf"));
    if (options_.saveItems) {
        SaveItemListFile(ResolveAppPath(L"itemlist.conf"), items_);
    }
}

void RKDockWnd::InstallHooks() {
    std::wstring dllPath = ResolveAppPath(L"RKLauncher.dll");
    if (!FileExists(dllPath) || !IsPeMachineX64(dllPath)) {
        DebugLog(L"Cannot load x64 RKLauncher.dll at %s", dllPath.c_str());
        return;
    }
    hHookDll_ = LoadLibraryW(dllPath.c_str());
    RKSetHookCBT(hwnd_);
    RKSetMinimize(options_.minimizeToDock ? TRUE : FALSE);
}

void RKDockWnd::UninstallHooks() {
    SetMouseHookActive(false);
    if (hwnd_) {
        RKRemoveHookCBT(hwnd_);
    }
    if (hHookDll_) {
        FreeLibrary(hHookDll_);
        hHookDll_ = nullptr;
    }
}

void RKDockWnd::SetMouseHookActive(bool active) {
    if (!hwnd_ || active == mouseHookActive_) return;
    if (active) {
        RKSetHookMouse(hwnd_);
        mouseHookActive_ = true;
    } else {
        RKRemoveHookMouse(hwnd_);
        mouseHookActive_ = false;
    }
}

void RKDockWnd::MarkDirty() {
    dirty_ = true;
    EnsureRenderTimer();
}

bool RKDockWnd::NeedActiveRenderTimer() const {
    if (dirty_ || effectWnd_.IsPlaying()) return true;
    float targetZoom = (mouseInside_ && options_.magnification) ? 1.0f : 0.0f;
    if (zoomProgress_ != targetZoom) return true;
    float targetHide = autoHidden_ ? 1.0f : 0.0f;
    if (hideOffset01_ != targetHide) return true;
    if (options_.automaticHide && !autoHidden_ && !mouseInside_ && !menuOpen_ && !dragging_) {
        return true;
    }
    for (const auto& it : items_) {
        if (it->IsBouncing()) return true;
    }
    return false;
}

void RKDockWnd::EnsureRenderTimer() {
    if (!hwnd_ || destroyed_) return;
    if (!renderPeriodSet_) {
        timeBeginPeriod(1);
        renderPeriodSet_ = true;
    }
    if (!renderTimerActive_) {
        QueryPerformanceCounter(&lastRenderQpc_);
        SetTimer(hwnd_, TIMER_RENDER, 16, nullptr);
        renderTimerActive_ = true;
    }
}

void RKDockWnd::StopRenderTimer() {
    if (renderTimerActive_ && hwnd_) {
        KillTimer(hwnd_, TIMER_RENDER);
    }
    renderTimerActive_ = false;
    if (renderPeriodSet_) {
        timeEndPeriod(1);
        renderPeriodSet_ = false;
    }
}

void RKDockWnd::LoadOptionalEffectPlugin() {
    effectWnd_.StopMinMax();
    if (hEffectDll_) {
        FreeLibrary(hEffectDll_);
        hEffectDll_ = nullptr;
        fnEffCreate_ = nullptr;
        fnEffNext_ = nullptr;
        fnEffDestroy_ = nullptr;
    }
    if (!options_.useMinMaxEffect || options_.minMaxEffect.empty()) return;
    std::wstring relNorm = NormalizeSlashes(options_.minMaxEffect, L'\\');
    std::wstring p = ResolveAppPath(L"plugins\\" + relNorm);
    if (!FileExists(p)) {
        p = ResolveAppPath(relNorm);
    }
    if (!FileExists(p) || !IsPeMachineX64(p)) {
        DebugLog(L"Skipping missing or non-x64 minMaxEffect DLL: %s", p.c_str());
        return;
    }
    hEffectDll_ = LoadLibraryW(p.c_str());
    if (hEffectDll_) {
        fnEffCreate_  = reinterpret_cast<FnEffectCreate>(GetProcAddress(hEffectDll_, "EffectCreate"));
        fnEffNext_    = reinterpret_cast<FnEffectNextStep>(GetProcAddress(hEffectDll_, "EffectNextStep"));
        fnEffDestroy_ = reinterpret_cast<FnEffectDestroy>(GetProcAddress(hEffectDll_, "EffectDestroy"));
    }
}

void RKDockWnd::ApplyOptionsAndReload(bool reloadTheme) {
    options_.ClampValid();
    if (reloadTheme) {
        theme_.Load(options_.theme);
    }
    if (hwnd_) {
        if (!options_.automaticHide && (autoHidden_ || hideOffset01_ > 0.001f)) {
            WakeFromAutoHide();
        }
        raisedFront32_ = false;
        hotspotPoppedTop_ = false;
        hotspotWaiting_ = false;
        prevFgBeforePopup_ = nullptr;
        LONG_PTR curEx = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        LONG_PTR desiredEx = options_.neverTakeFocus ? (curEx | WS_EX_NOACTIVATE) : (curEx & ~WS_EX_NOACTIVATE);
        if (desiredEx != curEx) {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, desiredEx);
        }
        SetWindowPos(hwnd_, options_.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SyncAutoHideZOrder();
    }
    RKSetMinimize(options_.minimizeToDock ? TRUE : FALSE);
    LoadOptionalEffectPlugin();
    SyncTaskbarVisibility();
    textWnd_.InvalidateCache();
    if (!options_.newApplications) {
        items_.erase(
            std::remove_if(items_.begin(), items_.end(), [](const std::unique_ptr<RKDockItem>& it) {
                if (!it || it->Type() != DockItemType::Window) return false;
                const auto* wi = static_cast<const RKDockItemWindow*>(it.get());
                return !wi->WasMinimizedByDock();
            }),
            items_.end());
    } else if (!options_.excludedApplications.empty()) {
        items_.erase(
            std::remove_if(items_.begin(), items_.end(), [this](const std::unique_ptr<RKDockItem>& it) {
                if (!it || it->Type() != DockItemType::Window) return false;
                const auto* wi = static_cast<const RKDockItemWindow*>(it.get());
                HWND target = wi->GetTargetHwnd();
                if (wi->WasMinimizedByDock() || !target) return false;
                DWORD pid = 0;
                GetWindowThreadProcessId(target, &pid);
                if (!pid) return false;
                HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!hp) return false;
                wchar_t p[MAX_PATH] = {};
                DWORD sz = MAX_PATH;
                std::wstring exePath;
                if (QueryFullProcessImageNameW(hp, 0, p, &sz)) exePath = p;
                CloseHandle(hp);
                return !exePath.empty() && options_.IsAppExcluded(exePath);
            }),
            items_.end());
    }
    RefreshIndicatorsAndWindows();
    RequestLayoutAndRedraw();
    if (NeedActiveRenderTimer()) EnsureRenderTimer();
}

void RKDockWnd::SyncTaskbarVisibility() {
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTray) return;
    if (options_.hideTaskbar) {
        if (IsWindowVisible(hTray)) {
            ShowWindow(hTray, SW_HIDE);
            taskbarHiddenByUs_ = true;
        }
    } else if (taskbarHiddenByUs_) {
        ShowWindow(hTray, SW_SHOWNA);
        taskbarHiddenByUs_ = false;
    }
}

void RKDockWnd::RequestLayoutAndRedraw() {
    hoverTracker_.Reset();
    dirty_ = true;
    RenderFrame();
    if (NeedActiveRenderTimer()) EnsureRenderTimer();
}

RECT RKDockWnd::GetTargetWorkArea() const {
    POINT refPt{ 0, 0 };
    if (options_.multiMonitorSupport == 1) {
        GetCursorPos(&refPt);
    } else if (lastLayout_.windowScreenRect.right > lastLayout_.windowScreenRect.left &&
               lastLayout_.windowScreenRect.bottom > lastLayout_.windowScreenRect.top) {
        // Anchor to nominal dock position so AutoHide off-screen slide never drifts onto another monitor
        refPt = {
            (lastLayout_.windowScreenRect.left + lastLayout_.windowScreenRect.right) / 2,
            (lastLayout_.windowScreenRect.top + lastLayout_.windowScreenRect.bottom) / 2
        };
    } else if (hwnd_ && IsWindowVisible(hwnd_)) {
        RECT rw{};
        GetWindowRect(hwnd_, &rw);
        refPt = { (rw.left + rw.right) / 2, (rw.top + rw.bottom) / 2 };
    }
    HMONITOR hMon = MonitorFromPoint(refPt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        return options_.hideTaskbar ? mi.rcMonitor : mi.rcWork;
    }
    RECT wa{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    return wa;
}

int RKDockWnd::HitTestItem(POINT clientPt) const {
    for (size_t i = 0; i < lastLayout_.items.size() && i < items_.size(); ++i) {
        const RECT& r = lastLayout_.items[i].iconRect;
        if (clientPt.x >= r.left - 2 && clientPt.x <= r.right + 2 &&
            clientPt.y >= r.top - 4  && clientPt.y <= r.bottom + 4) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

float RKDockWnd::MapCursorToNominalAxis(POINT clientPt) const {
    const bool horiz = IsHorizontalPos(options_.position);
    const float rawAxis = horiz ? static_cast<float>(clientPt.x) : static_cast<float>(clientPt.y);
    if (zoomProgress_ <= 0.01f || !options_.magnification || lastLayout_.items.size() != items_.size() || items_.empty()) {
        return rawAxis;
    }

    const DockEffectivePadding pad = AlgMagnification::ComputeEffectivePadding(
        theme_.GetEffectiveMargins(options_.position), options_);
    const float leadPad  = horiz ? static_cast<float>(pad.totalLeft) : static_cast<float>(pad.totalTop);
    const float smallSz  = static_cast<float>(std::max(16, options_.smallSize));
    const float sepBase  = AlgMagnification::SeparatorSlotWidth(options_);
    const float gap      = static_cast<float>(std::max(0, options_.itemDistance));

    // Reconstruct nominal unmagnified [start, end] for each visible item and invert current rendered layout
    float nomCursor = leadPad;
    bool first = true;
    int prevVisIdx = -1;
    float prevNomEnd = leadPad;
    float prevMagEnd = leadPad;

    for (size_t i = 0; i < items_.size(); ++i) {
        if (dragging_ && static_cast<int>(i) == dragIndex_) continue;
        if (!first) nomCursor += gap;
        float w = (items_[i]->Type() == DockItemType::Separator) ? sepBase : smallSz;
        float nomStart = nomCursor;
        float nomEnd   = nomCursor + w;
        nomCursor = nomEnd;

        const RECT& ir = lastLayout_.items[i].iconRect;
        float magStart = horiz ? static_cast<float>(ir.left)  : static_cast<float>(ir.top);
        float magEnd   = horiz ? static_cast<float>(ir.right) : static_cast<float>(ir.bottom);

        if (first && rawAxis <= magStart) {
            return nomStart - (magStart - rawAxis);
        }
        if (prevVisIdx >= 0 && rawAxis < magStart) {
            float spanMag = std::max(1.0f, magStart - prevMagEnd);
            float t = (rawAxis - prevMagEnd) / spanMag;
            return prevNomEnd + t * (nomStart - prevNomEnd);
        }
        if (rawAxis >= magStart && rawAxis <= magEnd) {
            float spanMag = std::max(1.0f, magEnd - magStart);
            float t = (rawAxis - magStart) / spanMag;
            return nomStart + t * (nomEnd - nomStart);
        }

        first = false;
        prevVisIdx = static_cast<int>(i);
        prevNomEnd = nomEnd;
        prevMagEnd = magEnd;
    }

    return prevVisIdx >= 0 ? (prevNomEnd + (rawAxis - prevMagEnd)) : rawAxis;
}

void RKDockWnd::SyncAutoHideZOrder() {
    if (!hwnd_) return;
    if (options_.alwaysOnTop) {
        raisedFront32_ = false;
        hotspotPoppedTop_ = false;
        prevFgBeforePopup_ = nullptr;
        if ((GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return;
    }

    // 32-bit mode: never promote to HWND_TOPMOST when Always on Top is off;
    // instead guard against fullscreen foreground apps and use AttachThreadInput +
    // SetForegroundWindow + BringWindowToTop, then restore the previous foreground window on leave.
    const bool wantFront = (options_.automaticHide && !autoHidden_) || hotspotPoppedTop_;
    if (wantFront && !raisedFront32_) {
        HWND fg = GetForegroundWindow();
        if (fg != hwnd_ && IsFullscreenWindow32(fg, hwnd_)) {
            hotspotPoppedTop_ = false;
            return;
        }
        if (!prevFgBeforePopup_ && IsExternalOccludingCandidate(fg, hwnd_)) {
            prevFgBeforePopup_ = fg;
        }
        raisedFront32_ = true;
        ForceForegroundWindow32(hwnd_, !options_.neverTakeFocus);
    } else if (!wantFront && raisedFront32_) {
        raisedFront32_ = false;
        HWND prev = prevFgBeforePopup_;
        prevFgBeforePopup_ = nullptr;

        HWND fg = GetForegroundWindow();
        if (prev && IsWindow(prev) && !IsIconic(prev) && prev != hwnd_ && fg == hwnd_) {
            ForceForegroundWindow32(prev, true);
        } else {
            RECT winRc{};
            GetWindowRect(hwnd_, &winRc);
            HWND hAfter = nullptr;
            if (IsNormalNonTopmostOverlapping(prev, hwnd_, winRc)) {
                hAfter = prev;
            } else if (fg && fg != hwnd_ && IsNormalNonTopmostOverlapping(fg, hwnd_, winRc)) {
                hAfter = fg;
            }
            if (hAfter) {
                SetWindowPos(hwnd_, hAfter, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
    }
}

void RKDockWnd::SetHoverTimerInterval(UINT ms) {
    if (!hwnd_ || hoverTimerIntervalMs_ == ms) return;
    hoverTimerIntervalMs_ = ms;
    SetTimer(hwnd_, TIMER_HOVERCHECK, ms, nullptr);
}

void RKDockWnd::WakeFromAutoHide() {
    DWORD now = GetTickCount();
    autoHidden_ = false;
    hotspotWaiting_ = false;
    autoWakeTick_ = now;
    mouseLeaveTick_ = now;
    SyncAutoHideZOrder();
    SetHoverTimerInterval(25);
    MarkDirty();
}

void RKDockWnd::UpdateHoverFromCursor() {
    if (!hwnd_ || menuOpen_) return;
    POINT pt{};
    GetCursorPos(&pt);

    RECT winRc{};
    GetWindowRect(hwnd_, &winRc);

    RECT wa = GetTargetWorkArea();
    const RECT& nomRc = (lastLayout_.windowScreenRect.right > lastLayout_.windowScreenRect.left &&
                         lastLayout_.windowScreenRect.bottom > lastLayout_.windowScreenRect.top)
                            ? lastLayout_.windowScreenRect
                            : winRc;

    auto isOverDockActiveArea = [&](POINT screenPt) -> bool {
        if (PtInRect(&winRc, screenPt) == FALSE) return false;
        POINT cl = screenPt;
        ScreenToClient(hwnd_, &cl);
        const RECT& bg = lastLayout_.bgLocalRect;
        return (PtInRect(&bg, cl) != FALSE) || (HitTestItem(cl) >= 0);
    };

    const bool overDockArea = isOverDockActiveArea(pt);
    const bool occludedAtPt = (overDockArea && !options_.alwaysOnTop)
                                  ? IsPointOccludedByOtherWindow(hwnd_, pt)
                                  : false;

    if (dockletLockOwner_ && dockletLockOwner_ != hwnd_ && !IsWindow(dockletLockOwner_)) {
        dockletLockOwner_ = nullptr;
    }
    const bool dockletLocked = IsDockletMouseLocked();

    bool insideActiveZone = false;
    if (dockletLocked) {
        insideActiveZone = true;
    } else if (!autoHidden_ && overDockArea && !occludedAtPt) {
        insideActiveZone = true;
    }

    if (!options_.automaticHide) {
        if (!options_.alwaysOnTop && options_.useMarginHotspot) {
            const bool inMarginHotspot = IsPointInDockMarginHotspot(pt, options_, wa, nomRc);
            if (!hotspotPoppedTop_) {
                if (inMarginHotspot && (occludedAtPt || !overDockArea)) {
                    DWORD now = GetTickCount();
                    DWORD delayMs = static_cast<DWORD>(std::max(0, options_.hotspotDelay));
                    if (!hotspotWaiting_) {
                        hotspotWaiting_ = true;
                        hotspotEnterTick_ = now;
                    }
                    if (now - hotspotEnterTick_ >= delayMs) {
                        hotspotPoppedTop_ = true;
                        hotspotWaiting_ = false;
                        SyncAutoHideZOrder();
                        if (raisedFront32_ && !autoHidden_ && overDockArea &&
                            !IsPointOccludedByOtherWindow(hwnd_, pt)) {
                            insideActiveZone = true;
                        }
                    }
                } else {
                    hotspotWaiting_ = false;
                }
            } else if (!insideActiveZone && !inMarginHotspot && !dragging_) {
                hotspotPoppedTop_ = false;
                hotspotWaiting_ = false;
                SyncAutoHideZOrder();
            }
        } else if (hotspotPoppedTop_) {
            hotspotPoppedTop_ = false;
            hotspotWaiting_ = false;
            SyncAutoHideZOrder();
        }
    } else if (hotspotPoppedTop_) {
        hotspotPoppedTop_ = false;
    }

    if (insideActiveZone) {
        bool wasInside = mouseInside_;
        mouseInside_ = true;
        hotspotWaiting_ = false;
        if (autoHidden_) {
            WakeFromAutoHide();
        }
        POINT cl = pt;
        ScreenToClient(hwnd_, &cl);
        if (!dockletLocked) {
            hoverTracker_.UpdateAxisIfChanged(
                pt, cl, zoomProgress_, 0.5f, true, !wasInside,
                mouseAxisLocal_, dirty_,
                [this](POINT c) { return MapCursorToNominalAxis(c); });
        }
        int newHover = HitTestItem(cl);
        if (newHover != hoveredIndex_) {
            hoveredIndex_ = newHover;
            dirty_ = true;
        }
        if (!dockletLocked && hoveredIndex_ >= 0 && hoveredIndex_ < static_cast<int>(items_.size()) &&
            items_[hoveredIndex_]->Type() != DockItemType::Separator && !dragging_) {
            textWnd_.ShowLabel(items_[hoveredIndex_]->Label(), items_[hoveredIndex_]->screenRect, wa, options_);
        } else {
            textWnd_.HideLabel();
        }
    } else {
        hoverTracker_.Reset();
        if (mouseInside_) {
            mouseInside_ = false;
            mouseLeaveTick_ = GetTickCount();
            dirty_ = true;
        }
        if (hoveredIndex_ != -1) {
            hoveredIndex_ = -1;
            dirty_ = true;
        }
        if (!dragging_) textWnd_.HideLabel();

        if (options_.automaticHide) {
            bool inWakeZone = IsPointInDockWakeZone(pt, options_, wa, nomRc, winRc);
            if (!autoHidden_) {
                // Keep dock open while cursor sits on the screen-edge margin or while slide-in is animating
                if (inWakeZone || hideOffset01_ > 0.01f) {
                    mouseLeaveTick_ = GetTickCount();
                }
            } else {
                DWORD now = GetTickCount();
                DWORD delayMs = options_.useMarginHotspot
                                    ? static_cast<DWORD>(std::max(0, options_.hotspotDelay))
                                    : 0;
                if (inWakeZone) {
                    if (!hotspotWaiting_) {
                        hotspotWaiting_ = true;
                        hotspotEnterTick_ = now;
                    }
                    if (now - hotspotEnterTick_ >= delayMs) {
                        WakeFromAutoHide();
                    }
                } else {
                    hotspotWaiting_ = false;
                }
            }
        }
    }

    bool needFastHover = insideActiveZone || mouseInside_ || dragging_ || dockletLocked ||
                         hotspotWaiting_ || raisedFront32_ || hotspotPoppedTop_ ||
                         (options_.automaticHide && !autoHidden_) || (hideOffset01_ > 0.001f);
    if (!needFastHover) {
        RECT nearNom = nomRc;
        InflateRect(&nearNom, 72, 72);
        const bool nearDock = (PtInRect(&nearNom, pt) != FALSE);
        bool nearEdge = false;
        if (!nearDock && (options_.automaticHide || (!options_.alwaysOnTop && options_.useMarginHotspot))) {
            if (IsHorizontalPos(options_.position)) {
                if (pt.x >= wa.left - 80 && pt.x <= wa.right + 80) {
                    const long waCy = (wa.top + wa.bottom) / 2;
                    const long nomCy = (nomRc.top + nomRc.bottom) / 2;
                    if (nomCy < waCy) {
                        nearEdge = (pt.y >= std::min(wa.top, nomRc.top) - 80 &&
                                    pt.y <= std::max(wa.top, nomRc.top) + 32);
                    } else {
                        nearEdge = (pt.y >= std::min(wa.bottom, nomRc.bottom) - 32 &&
                                    pt.y <= std::max(wa.bottom, nomRc.bottom) + 80);
                    }
                }
            } else {
                if (pt.y >= wa.top - 80 && pt.y <= wa.bottom + 80) {
                    const long waCx = (wa.left + wa.right) / 2;
                    const long nomCx = (nomRc.left + nomRc.right) / 2;
                    if (nomCx < waCx) {
                        nearEdge = (pt.x >= std::min(wa.left, nomRc.left) - 80 &&
                                    pt.x <= std::max(wa.left, nomRc.left) + 32);
                    } else {
                        nearEdge = (pt.x >= std::min(wa.right, nomRc.right) - 32 &&
                                    pt.x <= std::max(wa.right, nomRc.right) + 80);
                    }
                }
            }
        }
        needFastHover = nearDock || nearEdge;
    }
    SetHoverTimerInterval(needFastHover ? 25u : 85u);

    if (NeedActiveRenderTimer()) {
        EnsureRenderTimer();
    }
}

void RKDockWnd::RenderFrame() {
    if (!hwnd_) return;

    DWORD now = GetTickCount();
    ThemeMargins margins = theme_.GetEffectiveMargins(options_.position);
    magInScratch_.assign(items_.size(), MagItemInput{});
    bool anyBouncing = false;

    for (size_t i = 0; i < items_.size(); ++i) {
        magInScratch_[i].isSeparator = (items_[i]->Type() == DockItemType::Separator);
        magInScratch_[i].hidden      = (dragging_ && static_cast<int>(i) == dragIndex_);
        float b = items_[i]->GetBounceOffset01(now);
        magInScratch_[i].bounceNorm = b;
        if (items_[i]->IsBouncing()) anyBouncing = true;
    }

    if (items_.size() != lastLayout_.items.size()) {
        hoverTracker_.Reset();
    }
    RECT wa = GetTargetWorkArea();
    lastLayout_ = AlgMagnification::ComputeLayout(
        magInScratch_, options_, margins, wa, mouseInside_, mouseAxisLocal_, zoomProgress_);

    int w = lastLayout_.dockWidth;
    int h = lastLayout_.dockHeight;
    if (!surface_.EnsureSize(w, h)) return;

    {
        Gdiplus::Graphics g(surface_.MemoryDC());
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if (_wcsicmp(options_.quality.c_str(), L"High") == 0) {
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        } else if (_wcsicmp(options_.quality.c_str(), L"Low") == 0) {
            g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        } else {
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        }
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        const RECT& bgr = lastLayout_.bgLocalRect;
        const RECT& pr  = lastLayout_.pillLocalRect;
        Gdiplus::Rect gPill(pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top);
        theme_.DrawBackground(g, Gdiplus::Rect(bgr.left, bgr.top, bgr.right - bgr.left, bgr.bottom - bgr.top),
                              gPill, options_.position, options_.backgroundAlpha);

        for (size_t i = 0; i < items_.size(); ++i) {
            auto& item = items_[i];
            const RECT& lr = lastLayout_.items[i].iconRect;
            item->localRect = lr;
            item->screenRect = {
                lastLayout_.windowScreenRect.left + lr.left,
                lastLayout_.windowScreenRect.top + lr.top,
                lastLayout_.windowScreenRect.left + lr.right,
                lastLayout_.windowScreenRect.top + lr.bottom
            };

            if (magInScratch_[i].hidden) continue;
            Gdiplus::Rect gr(lr.left, lr.top, lr.right - lr.left, lr.bottom - lr.top);
            if (gr.Width <= 0 || gr.Height <= 0) continue;

            if (item->Type() == DockItemType::Separator) {
                theme_.DrawSeparator(g, gr, options_.position, options_.backgroundAlpha);
                continue;
            }

            if (Gdiplus::Bitmap* bmp = item->PrimaryBitmap()) {
                g.DrawImage(bmp, gr, 0, 0, bmp->GetWidth(), bmp->GetHeight(), Gdiplus::UnitPixel);
            }
            if (Gdiplus::Bitmap* ov = item->OverlayBitmap()) {
                g.DrawImage(ov, gr, 0, 0, ov->GetWidth(), ov->GetHeight(), Gdiplus::UnitPixel);
            }
            if (!options_.hideIndicators && item->ShowIndicator() && item->IsRunning()) {
                theme_.DrawIndicator(g, gr, gPill, options_.position);
            }
        }
    }

    POINT pos = ComputeAutoHideOrigin(lastLayout_, options_.position, hideOffset01_);
    BYTE alpha = static_cast<BYTE>(std::clamp(static_cast<int>(255.0f * (1.0f - hideOffset01_ * 0.85f)), 2, 255));
    surface_.Present(hwnd_, pos.x, pos.y, alpha);
    dirty_ = anyBouncing;
    if (anyBouncing) EnsureRenderTimer();
}

void RKDockWnd::OnTimerTick(UINT_PTR timerId) {
    if (timerId == TIMER_HOVERCHECK) {
        UpdateHoverFromCursor();
        textWnd_.TickFade(options_);
        return;
    }

    if (timerId == TIMER_WINDOWSYNC) {
        RefreshIndicatorsAndWindows();
        return;
    }

    if (timerId == TIMER_INDICATOR_RECHECK) {
        KillTimer(hwnd_, TIMER_INDICATOR_RECHECK);
        RefreshIndicatorsAndWindows();
        return;
    }

    if (timerId == TIMER_RENDER) {
        LARGE_INTEGER nowQpc{};
        QueryPerformanceCounter(&nowQpc);
        float dtSec = 0.016f;
        if (qpcFreq_.QuadPart > 0 && lastRenderQpc_.QuadPart > 0) {
            dtSec = static_cast<float>(nowQpc.QuadPart - lastRenderQpc_.QuadPart) /
                    static_cast<float>(qpcFreq_.QuadPart);
        }
        lastRenderQpc_ = nowQpc;
        float frames = std::clamp(dtSec * 60.0f, 0.1f, 3.0f);

        // AutoHide delay trigger
        if (options_.automaticHide && !mouseInside_ && !autoHidden_ && !menuOpen_ && !dragging_) {
            DWORD now = GetTickCount();
            DWORD delayMs = static_cast<DWORD>(std::max(0, options_.autoHideDelay));
            DWORD wakeGraceMs = std::max<DWORD>(delayMs, 600);
            if (hideOffset01_ <= 0.01f &&
                (now - autoWakeTick_ >= wakeGraceMs) &&
                (now - mouseLeaveTick_ >= delayMs)) {
                autoHidden_ = true;
                hotspotWaiting_ = false;
                dirty_ = true;
            }
        } else if (!options_.automaticHide && autoHidden_) {
            WakeFromAutoHide();
        }

        float targetZoom = (mouseInside_ && options_.magnification) ? 1.0f : 0.0f;
        if (std::abs(zoomProgress_ - targetZoom) > 0.005f) {
            float speed = (targetZoom > zoomProgress_)
                              ? (frames / std::max(1, options_.magnifyTransitionIn))
                              : (frames / std::max(1, options_.magnifyTransitionOut));
            if (targetZoom > zoomProgress_) zoomProgress_ = std::min(1.0f, zoomProgress_ + speed);
            else zoomProgress_ = std::max(0.0f, zoomProgress_ - speed);
            dirty_ = true;
        } else if (zoomProgress_ != targetZoom) {
            zoomProgress_ = targetZoom;
            dirty_ = true;
        }

        float targetHide = autoHidden_ ? 1.0f : 0.0f;
        if (std::abs(hideOffset01_ - targetHide) > 0.01f) {
            hideOffset01_ += (targetHide > hideOffset01_) ? (0.14f * frames) : (-0.20f * frames);
            hideOffset01_ = std::clamp(hideOffset01_, 0.0f, 1.0f);
            dirty_ = true;
        } else if (hideOffset01_ != targetHide) {
            hideOffset01_ = targetHide;
            dirty_ = true;
        }

        if (dirty_) RenderFrame();
        if (effectWnd_.IsPlaying()) effectWnd_.Tick();
        SyncAutoHideZOrder();
        if (!NeedActiveRenderTimer()) {
            StopRenderTimer();
        }
    }
}

void RKDockWnd::RefreshIndicatorsAndWindows() {
    SyncRunningWindows();
    bool needRunningSet = false;
    if (!options_.hideIndicators) {
        for (const auto& it : items_) {
            if (it->ShowIndicator() &&
                (it->Type() == DockItemType::Shortcut || it->Type() == DockItemType::Folder)) {
                needRunningSet = true;
                break;
            }
        }
    }
    auto runningExes = needRunningSet ? CollectRunningExeNames()
                                      : decltype(CollectRunningExeNames()){};
    bool changed = false;
    for (size_t i = items_.size(); i > 0; --i) {
        bool wasRunning = items_[i - 1]->IsRunning();
        items_[i - 1]->RefreshRuntimeStateWithSet(runningExes);
        if (items_[i - 1]->IsRunning() != wasRunning) {
            changed = true;
        }
        if (items_[i - 1]->markedForRemoval) {
            items_.erase(items_.begin() + static_cast<ptrdiff_t>(i - 1));
            changed = true;
        }
    }
    if (changed || dirty_) {
        dirty_ = true;
        RenderFrame();
    }
    if (NeedActiveRenderTimer()) {
        EnsureRenderTimer();
    }
}

void RKDockWnd::SyncRunningWindows() {
    if (!options_.newApplications) return;
    struct EnumCtx {
        RKDockWnd* self;
    } ctx{ this };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lp);
        if (!IsWindowVisible(hwnd) || hwnd == c->self->hwnd_) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        LONG_PTR ex    = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((ex & WS_EX_TOOLWINDOW) || !(style & WS_CAPTION)) return TRUE;

        wchar_t title[128] = {};
        if (GetWindowTextW(hwnd, title, 128) <= 0) return TRUE;

        for (const auto& it : c->self->items_) {
            if (it->GetTargetHwnd() == hwnd) return TRUE;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        std::wstring exeName;
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hp) {
            wchar_t p[MAX_PATH] = {};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(hp, 0, p, &sz)) exeName = p;
            CloseHandle(hp);
        }
        if (c->self->options_.IsAppExcluded(exeName)) return TRUE;

        auto wi = std::make_unique<RKDockItemWindow>(hwnd);
        c->self->items_.push_back(std::move(wi));
        c->self->dirty_ = true;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

void RKDockWnd::RestoreDockMinimizedWindows() {
    for (auto& it : items_) {
        if (auto* wi = dynamic_cast<RKDockItemWindow*>(it.get())) {
            if (wi->WasMinimizedByDock() && wi->GetTargetHwnd() && IsWindow(wi->GetTargetHwnd())) {
                ShowWindow(wi->GetTargetHwnd(), SW_SHOWNA);
                ShowWindow(wi->GetTargetHwnd(), SW_RESTORE);
            }
        }
    }
}

void RKDockWnd::OnHookMouse(WPARAM wParam, LPARAM lParam) {
    if (dragging_ && wParam == WM_MOUSEMOVE) {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT winRc{};
        GetWindowRect(hwnd_, &winRc);
        InflateRect(&winRc, 48, 48);
        bool willDelete = !PtInRect(&winRc, pt);
        dragWnd_.Move(pt, willDelete);
    } else if (dragging_ && wParam == WM_LBUTTONUP) {
        SendMessageW(hwnd_, WM_LBUTTONUP, 0, 0);
    }
}

void RKDockWnd::OnHookCBT(WPARAM wParam, LPARAM lParam) {
    HWND target = reinterpret_cast<HWND>(lParam);
    if (!target) return;

    if (wParam == kCbtEventDestroy) {
        bool removed = false;
        for (size_t i = items_.size(); i > 0; --i) {
            if (items_[i - 1]->GetTargetHwnd() == target) {
                items_.erase(items_.begin() + static_cast<ptrdiff_t>(i - 1));
                removed = true;
            }
        }
        if (removed) {
            dirty_ = true;
            RenderFrame();
        }
        if (hwnd_) SetTimer(hwnd_, TIMER_INDICATOR_RECHECK, 120, nullptr);
        return;
    }

    if (wParam == SW_SHOWMINIMIZED || wParam == SW_MINIMIZE ||
        wParam == SW_FORCEMINIMIZE || wParam == SW_SHOWMINNOACTIVE) {
        if (!options_.minimizeToDock || !IsWindow(target)) return;

        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        if (pid == GetCurrentProcessId()) return;

        std::wstring exeName;
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hp) {
            wchar_t p[MAX_PATH] = {};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(hp, 0, p, &sz)) exeName = p;
            CloseHandle(hp);
        }
        if (options_.IsMinimizeExcluded(exeName)) return;

        auto playMinimizeEffect = [&](const RECT& dstRect) {
            if (options_.useMinMaxEffect && fnEffCreate_ && fnEffNext_ && fnEffDestroy_) {
                effectWnd_.StartMinMax(target, dstRect, fnEffCreate_, fnEffNext_, fnEffDestroy_);
                EnsureRenderTimer();
            }
        };

        for (const auto& it : items_) {
            if (it->GetTargetHwnd() == target) {
                if (auto* wi = dynamic_cast<RKDockItemWindow*>(it.get())) {
                    wi->SetMinimizedByDock(true);
                    playMinimizeEffect(wi->screenRect);
                    ShowWindowAsync(target, SW_HIDE);
                    MarkDirty();
                }
                return;
            }
        }

        auto wi = std::make_unique<RKDockItemWindow>(target);
        wi->SetMinimizedByDock(true);
        if (options_.itemAnimation && options_.itemAnimationNew > 0) {
            wi->StartBounce(1);
        }
        RECT dstRect = lastLayout_.windowScreenRect;
        if (dstRect.right <= dstRect.left) {
            GetWindowRect(hwnd_, &dstRect);
        } else {
            int cx = (dstRect.left + dstRect.right) / 2;
            int cy = (dstRect.top + dstRect.bottom) / 2;
            int half = options_.smallSize / 2;
            dstRect = { cx - half, cy - half, cx + half, cy + half };
        }
        playMinimizeEffect(dstRect);
        ShowWindowAsync(target, SW_HIDE);
        if (options_.minimizePosition >= 0 && options_.minimizePosition <= static_cast<int>(items_.size())) {
            items_.insert(items_.begin() + options_.minimizePosition, std::move(wi));
        } else {
            items_.push_back(std::move(wi));
        }
        MarkDirty();
        return;
    }

    if (wParam == SW_RESTORE) {
        for (size_t i = items_.size(); i > 0; --i) {
            if (auto* wi = dynamic_cast<RKDockItemWindow*>(items_[i - 1].get())) {
                if (wi->GetTargetHwnd() == target && wi->WasMinimizedByDock()) {
                    if (!options_.newApplications) {
                        items_.erase(items_.begin() + static_cast<ptrdiff_t>(i - 1));
                    } else {
                        wi->SetMinimizedByDock(false);
                    }
                    MarkDirty();
                }
            }
        }
    }
}

void RKDockWnd::OnShellHook(WPARAM wParam, LPARAM lParam) {
    HWND target = reinterpret_cast<HWND>(lParam);
    if (wParam == HSHELL_FLASH && target) {
        for (auto& it : items_) {
            if (it->GetTargetHwnd() == target) {
                it->StartBounce(3);
                MarkDirty();
                break;
            }
        }
    } else if (wParam == HSHELL_WINDOWACTIVATED && hwnd_) {
        SetTimer(hwnd_, TIMER_INDICATOR_RECHECK, 120, nullptr);
    }
}

void RKDockWnd::AddSeparatorNear(int index) {
    auto sep = std::make_unique<RKDockItemSeparator>();
    if (index >= 0 && index <= static_cast<int>(items_.size())) {
        items_.insert(items_.begin() + index, std::move(sep));
    } else {
        items_.push_back(std::move(sep));
    }
    if (options_.saveOnChange && options_.saveItems) {
        SaveItemListFile(ResolveAppPath(L"itemlist.conf"), items_);
    }
    dirty_ = true;
    RenderFrame();
}

void RKDockWnd::AddDockletOD(const std::wstring& relDllPath, const std::wstring& label) {
    auto od = std::make_unique<RKDockItemDockletOD>();
    if (!label.empty()) od->SetLabel(label);
    IniFile cleanTmp;
    cleanTmp.SetEncoding(IniFile::Encoding::Utf16LE);
    cleanTmp.AddSection(L"DockletOD");
    cleanTmp.Save(ResolveAppPath(L"dockletstemp.ini"), L"; Temporary file generated by RK Launcher");
    if (od->InitializePlugin(hwnd_, relDllPath, L"dockletstemp.ini", L"DockletOD")) {
        items_.push_back(std::move(od));
        if (options_.saveOnChange && options_.saveItems) {
            SaveItemListFile(ResolveAppPath(L"itemlist.conf"), items_);
        }
        dirty_ = true;
        RenderFrame();
    }
}

void RKDockWnd::RemoveItemAt(size_t idx, bool playPoof, POINT poofScreenCenter) {
    if (idx >= items_.size()) return;
    if (playPoof && !options_.hidePoofs) {
        effectWnd_.StartPoof(poofScreenCenter, options_.magnifySize + 24, &theme_);
        EnsureRenderTimer();
    }
    items_.erase(items_.begin() + static_cast<ptrdiff_t>(idx));
    hoveredIndex_ = -1;
    textWnd_.HideLabel();
    if (options_.saveOnChange && options_.saveItems) {
        SaveItemListFile(ResolveAppPath(L"itemlist.conf"), items_);
    }
    dirty_ = true;
    RenderFrame();
}

void RKDockWnd::AddItemAtDropPoint(const std::wstring& filePath, POINT screenPt) {
    if (filePath.empty()) return;
    std::unique_ptr<RKDockItem> newItem;
    if (DirExists(filePath)) {
        auto f = std::make_unique<RKDockItemFolder>();
        f->InitFromDroppedFile(filePath);
        newItem = std::move(f);
    } else {
        auto s = std::make_unique<RKDockItemShortcut>();
        s->InitFromDroppedFile(filePath);
        newItem = std::move(s);
    }
    if (options_.itemAnimationNew > 0 && options_.itemAnimation) {
        newItem->StartBounce(2);
    }

    // Determine insertion slot based on screenPt
    size_t insertIdx = items_.size();
    bool horiz = IsHorizontalPos(options_.position);
    for (size_t i = 0; i < items_.size(); ++i) {
        const RECT& sr = items_[i]->screenRect;
        int mid = horiz ? ((sr.left + sr.right) / 2) : ((sr.top + sr.bottom) / 2);
        int cur = horiz ? screenPt.x : screenPt.y;
        if (cur < mid) { insertIdx = i; break; }
    }
    items_.insert(items_.begin() + static_cast<ptrdiff_t>(insertIdx), std::move(newItem));

    if (options_.saveOnChange && options_.saveItems) {
        SaveItemListFile(ResolveAppPath(L"itemlist.conf"), items_);
    }
    dirty_ = true;
    RenderFrame();
}

void RKDockWnd::ShowDockletOrItemContextMenu(POINT screenPt, bool allowPluginOverride) {
    POINT cl = screenPt;
    ScreenToClient(hwnd_, &cl);
    int hit = HitTestItem(cl);
    RKDockItem* item = (hit >= 0 && hit < static_cast<int>(items_.size())) ? items_[hit].get() : nullptr;

    menuOpen_ = true;
    contextMenuIndex_ = hit;
    if (hit >= 0) hoveredIndex_ = hit;
    textWnd_.HideLabel();
    RKHookDisable(5);

    bool handledByPlugin = false;
    if (allowPluginOverride && item) {
        if (auto* od = dynamic_cast<RKDockItemDockletOD*>(item)) {
            handledByPlugin = od->TryPluginRightClick();
        }
    }
    if (!handledByPlugin) {
        bool isSep = item && (item->Type() == DockItemType::Separator);
        ShowDockContextMenu(hwnd_, screenPt, options_, item, isSep);
    }

    RKHookEnable(5);
    menuOpen_ = false;
    mouseLeaveTick_ = GetTickCount();
    if (options_.alwaysOnTop) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else if (raisedFront32_) {
        ForceForegroundWindow32(hwnd_, !options_.neverTakeFocus);
    }
}

void RKDockWnd::OnCommand(UINT cmdId) {
    int targetIdx = (contextMenuIndex_ >= 0 && contextMenuIndex_ < static_cast<int>(items_.size()))
        ? contextMenuIndex_
        : hoveredIndex_;
    if (cmdId >= IDM_POS_BOTTOM_LEFT && cmdId <= IDM_POS_TOP_RIGHT) {
        // Original binary mapping:
        // 0x800d..0x800f = Bottom (3,4,5)
        // 0x8010..0x8012 = Left   (6,7,8)
        // 0x8013..0x8015 = Right  (9,10,11)
        // 0x8016..0x8018 = Top    (0,1,2)
        static constexpr int kPosTable[12] = {
            POS_BOTTOM_LEFT, POS_BOTTOM_CENTER, POS_BOTTOM_RIGHT,
            POS_LEFT_TOP,    POS_LEFT_CENTER,   POS_LEFT_BOTTOM,
            POS_RIGHT_TOP,   POS_RIGHT_CENTER,  POS_RIGHT_BOTTOM,
            POS_TOP_LEFT,    POS_TOP_CENTER,    POS_TOP_RIGHT
        };
        options_.position = kPosTable[cmdId - IDM_POS_BOTTOM_LEFT];
        if (options_.saveOnChange) SaveAllConfig();
        RequestLayoutAndRedraw();
        return;
    }

    switch (cmdId) {
    case 0x9c43: { // IDM_DOCK_ADD_ITEM ("Add Item...")
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"Applications & Shortcuts (*.exe;*.lnk;*.bat)\0*.exe;*.lnk;*.bat\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            POINT spt{};
            GetCursorPos(&spt);
            AddItemAtDropPoint(file, spt);
        }
        break;
    }
    case IDM_DOCK_QUIT:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    case IDM_DOCK_ADD_SEPARATOR:
        AddSeparatorNear(hoveredIndex_ >= 0 ? hoveredIndex_ + 1 : static_cast<int>(items_.size()));
        break;
    case IDM_DOCK_DEL_SEPARATOR:
        if (targetIdx >= 0 && targetIdx < static_cast<int>(items_.size()) &&
            items_[targetIdx]->Type() == DockItemType::Separator) {
            RemoveItemAt(static_cast<size_t>(targetIdx), false, {});
        }
        contextMenuIndex_ = -1;
        break;
    case IDM_DOCK_ADD_DOCKLET: {
        std::wstring dll, lbl;
        if (ShowAddDockletDialog(hwnd_, dll, lbl)) {
            AddDockletOD(dll, lbl);
        }
        break;
    }
    case IDM_DOCK_SETTINGS:
        if (ShowSettingsPropertySheet(
                hwnd_, options_,
                [](const RKDockOptions& appliedOpts, void* ud) {
                    auto* self = static_cast<RKDockWnd*>(ud);
                    if (!self) return;
                    self->options_ = appliedOpts;
                    self->ApplyOptionsAndReload(true);
                    if (self->options_.saveOnChange) self->SaveAllConfig();
                },
                this)) {
            ApplyOptionsAndReload(true);
            if (options_.saveOnChange) SaveAllConfig();
        }
        break;
    case IDM_DOCK_LANGUAGE:
        if (ShowLanguageDialog(hwnd_, options_)) {
            RKLangManager::Instance().LoadLanguage(options_.langResource);
            if (options_.saveOnChange) SaveAllConfig();
        }
        break;
    case IDM_DOCK_ABOUT:
        ShowAboutDialog(hwnd_);
        break;
    case IDM_DOCK_MAGNIFY:
        options_.magnification = !options_.magnification;
        if (options_.saveOnChange) SaveAllConfig();
        RequestLayoutAndRedraw();
        break;
    case IDM_DOCK_ALWAYSONTOP:
        options_.alwaysOnTop = !options_.alwaysOnTop;
        if (options_.saveOnChange) SaveAllConfig();
        ApplyOptionsAndReload(false);
        break;
    case IDM_DOCK_LOCKITEMS:
        options_.locked = !options_.locked;
        if (options_.saveOnChange) SaveAllConfig();
        break;
    case IDM_DOCK_AUTOHIDE:
        options_.automaticHide = !options_.automaticHide;
        mouseLeaveTick_ = GetTickCount();
        if (!options_.automaticHide) {
            WakeFromAutoHide();
        }
        if (options_.saveOnChange) SaveAllConfig();
        SyncAutoHideZOrder();
        break;
    case IDM_DOCK_MINIMIZE_TO_DOCK:
        options_.minimizeToDock = !options_.minimizeToDock;
        RKSetMinimize(options_.minimizeToDock ? TRUE : FALSE);
        if (options_.saveOnChange) SaveAllConfig();
        break;
    case IDM_ITEM_PROPERTIES:
        if (targetIdx >= 0 && targetIdx < static_cast<int>(items_.size())) {
            RKDockItem* it = items_[targetIdx].get();
            if (auto* od = dynamic_cast<RKDockItemDockletOD*>(it)) {
                menuOpen_ = true;
                RKHookDisable(5);
                od->ConfigurePlugin();
                RKHookEnable(5);
                menuOpen_ = false;
                if (options_.saveOnChange && options_.saveItems) SaveAllConfig();
                RequestLayoutAndRedraw();
            } else if (ShowItemPropertiesDialog(hwnd_, it)) {
                if (options_.saveOnChange && options_.saveItems) SaveAllConfig();
                RequestLayoutAndRedraw();
            }
        }
        contextMenuIndex_ = -1;
        break;
    case IDM_ITEM_REMOVE:
    case IDM_ITEM_DELETE_DOCKLET:
        if (targetIdx >= 0 && targetIdx < static_cast<int>(items_.size())) {
            RECT r = items_[targetIdx]->screenRect;
            POINT center{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
            RemoveItemAt(static_cast<size_t>(targetIdx), true, center);
        }
        contextMenuIndex_ = -1;
        break;
    case IDM_ITEM_CLOSE_WINDOW:
        if (targetIdx >= 0 && targetIdx < static_cast<int>(items_.size())) {
            if (auto* wi = dynamic_cast<RKDockItemWindow*>(items_[targetIdx].get())) {
                wi->CloseTargetWindow();
            }
        }
        contextMenuIndex_ = -1;
        break;
    default:
        break;
    }
}

// ============================================================================
// IDropTarget implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE RKDockWnd::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *ppv = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE RKDockWnd::AddRef() { return ++refCount_; }
ULONG STDMETHODCALLTYPE RKDockWnd::Release() { return --refCount_; }

HRESULT STDMETHODCALLTYPE RKDockWnd::DragEnter(IDataObject*, DWORD, POINTL pt, DWORD* pdwEffect) {
    mouseInside_ = true;
    POINT spt{ pt.x, pt.y };
    ScreenToClient(hwnd_, &spt);
    mouseAxisLocal_ = IsHorizontalPos(options_.position) ? static_cast<float>(spt.x) : static_cast<float>(spt.y);
    MarkDirty();
    if (pdwEffect) *pdwEffect = DROPEFFECT_COPY | DROPEFFECT_LINK;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RKDockWnd::DragOver(DWORD, POINTL pt, DWORD* pdwEffect) {
    POINT spt{ pt.x, pt.y };
    ScreenToClient(hwnd_, &spt);
    mouseAxisLocal_ = IsHorizontalPos(options_.position) ? static_cast<float>(spt.x) : static_cast<float>(spt.y);
    dirty_ = true;
    RenderFrame();
    if (pdwEffect) *pdwEffect = DROPEFFECT_COPY | DROPEFFECT_LINK;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RKDockWnd::DragLeave() {
    hoverTracker_.Reset();
    mouseInside_ = false;
    MarkDirty();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RKDockWnd::Drop(IDataObject* pDataObj, DWORD, POINTL pt, DWORD* pdwEffect) {
    if (!pDataObj) return E_INVALIDARG;
    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg{};
    if (SUCCEEDED(pDataObj->GetData(&fmt, &stg))) {
        HDROP hDrop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
        if (hDrop) {
            POINT spt{ pt.x, pt.y };
            POINT cl = spt;
            ScreenToClient(hwnd_, &cl);
            int hit = HitTestItem(cl);
            if (hit >= 0 && hit < static_cast<int>(items_.size()) && items_[hit]->AcceptsDrop()) {
                items_[hit]->OnDropFiles(hwnd_, hDrop);
            } else {
                UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    wchar_t path[MAX_PATH] = {};
                    if (DragQueryFileW(hDrop, i, path, MAX_PATH)) {
                        AddItemAtDropPoint(path, spt);
                    }
                }
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);
    }
    if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}

// ============================================================================
// Window Procedure
// ============================================================================

LRESULT CALLBACK RKDockWnd::DockWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RKDockWnd* self = reinterpret_cast<RKDockWnd*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<RKDockWnd*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    if (msg == self->msgHookMouse_) {
        self->OnHookMouse(wParam, lParam);
        return 0;
    }
    if (msg == self->msgHookCbt_) {
        self->OnHookCBT(wParam, lParam);
        return 0;
    }
    if (msg == self->msgShellHook_) {
        self->OnShellHook(wParam, lParam);
        return 0;
    }
    if (msg == self->msgTaskbarCreated_) {
        self->SyncTaskbarVisibility();
        return 0;
    }

    switch (msg) {
    case WM_TIMER:
        self->OnTimerTick(wParam);
        return 0;

    case WM_MOUSEMOVE: {
        POINT cl{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (self->leftDown_ && !self->dragging_ && !self->options_.locked &&
            self->dragIndex_ >= 0 && self->dragIndex_ < static_cast<int>(self->items_.size())) {
            POINT spt{};
            GetCursorPos(&spt);
            if (std::abs(spt.x - self->dragDownScreen_.x) > GetSystemMetrics(SM_CXDRAG) ||
                std::abs(spt.y - self->dragDownScreen_.y) > GetSystemMetrics(SM_CYDRAG)) {
                self->dragging_ = true;
                self->SetMouseHookActive(true);
                self->textWnd_.HideLabel();
                auto* bmp = self->items_[self->dragIndex_]->PrimaryBitmap();
                self->dragWnd_.Begin(bmp, self->options_.smallSize, spt);
                SetCapture(hwnd);
            }
        }
        if (self->dragging_) {
            POINT spt{};
            GetCursorPos(&spt);
            RECT winRc{};
            GetWindowRect(hwnd, &winRc);
            InflateRect(&winRc, 48, 48);
            bool willDel = !PtInRect(&winRc, spt);
            self->dragWnd_.Move(spt, willDel);
            if (!willDel) {
                bool horiz = IsHorizontalPos(self->options_.position);
                int cur = horiz ? cl.x : cl.y;
                int targetIdx = self->dragIndex_;
                for (int i = 0; i < static_cast<int>(self->items_.size()); ++i) {
                    const RECT& ir = self->items_[i]->localRect;
                    int mid = horiz ? ((ir.left + ir.right) / 2) : ((ir.top + ir.bottom) / 2);
                    if (i < self->dragIndex_ && cur < mid) { targetIdx = i; break; }
                    if (i > self->dragIndex_ && cur > mid) { targetIdx = i; }
                }
                if (targetIdx != self->dragIndex_) {
                    auto moved = std::move(self->items_[self->dragIndex_]);
                    self->items_.erase(self->items_.begin() + self->dragIndex_);
                    self->items_.insert(self->items_.begin() + targetIdx, std::move(moved));
                    self->dragIndex_ = targetIdx;
                    self->hoverTracker_.Reset();
                    self->MarkDirty();
                }
            }
        }
        if (!self->autoHidden_) {
            POINT spt{};
            GetCursorPos(&spt);
            POINT sptCl = spt;
            ScreenToClient(hwnd, &sptCl);
            bool wasInside = self->mouseInside_;
            self->mouseInside_ = true;
            if (!wasInside) self->dirty_ = true;
            self->SetHoverTimerInterval(25);
            if (!self->IsDockletMouseLocked()) {
                self->hoverTracker_.UpdateAxisIfChanged(
                    spt, sptCl, self->zoomProgress_, 0.25f, false, !wasInside,
                    self->mouseAxisLocal_, self->dirty_,
                    [self](POINT c) { return self->MapCursorToNominalAxis(c); });
            }
            if (self->NeedActiveRenderTimer()) self->EnsureRenderTimer();
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT cl{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hit = self->HitTestItem(cl);
        if (hit >= 0) {
            self->leftDown_ = true;
            self->dragIndex_ = hit;
            GetCursorPos(&self->dragDownScreen_);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        bool wasDragging = self->dragging_;
        int  dIdx        = self->dragIndex_;
        self->leftDown_ = false;
        self->dragging_ = false;
        self->dragIndex_ = -1;
        self->SetMouseHookActive(false);
        if (GetCapture() == hwnd) ReleaseCapture();

        if (wasDragging && dIdx >= 0 && dIdx < static_cast<int>(self->items_.size())) {
            self->dragWnd_.End();
            POINT spt{};
            GetCursorPos(&spt);
            RECT winRc{};
            GetWindowRect(hwnd, &winRc);
            InflateRect(&winRc, 48, 48);
            if (!PtInRect(&winRc, spt) && self->items_[dIdx]->Type() != DockItemType::Window) {
                self->RemoveItemAt(static_cast<size_t>(dIdx), true, spt);
            } else {
                if (self->options_.saveOnChange && self->options_.saveItems) {
                    SaveItemListFile(ResolveAppPath(L"itemlist.conf"), self->items_);
                }
                self->dirty_ = true;
                self->RenderFrame();
            }
            if (self->NeedActiveRenderTimer()) self->EnsureRenderTimer();
            return 0;
        }

        POINT cl{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hit = self->HitTestItem(cl);
        if (hit >= 0 && hit < static_cast<int>(self->items_.size())) {
            auto& it = self->items_[hit];
            if (it->Type() != DockItemType::Separator) {
                if (self->options_.itemAnimation && it->Type() != DockItemType::DockletOD) {
                    it->StartBounce(self->options_.itemAnimationCount);
                    self->dirty_ = true;
                    self->RenderFrame();
                }
                self->textWnd_.HideLabel();
                it->Execute(hwnd);
                self->MarkDirty();
            }
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != hwnd) {
            if (self->dragging_) {
                self->dragWnd_.End();
            }
            self->leftDown_ = false;
            self->dragging_ = false;
            self->dragIndex_ = -1;
            self->SetMouseHookActive(false);
            if (self->NeedActiveRenderTimer()) self->EnsureRenderTimer();
        }
        return 0;

    case WM_LBUTTONDBLCLK: {
        POINT cl{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hit = self->HitTestItem(cl);
        if (hit >= 0 && hit < static_cast<int>(self->items_.size())) {
            self->items_[hit]->OnDoubleClick(hwnd);
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT spt{};
        GetCursorPos(&spt);
        self->ShowDockletOrItemContextMenu(spt);
        return 0;
    }

    case WM_CONTEXTMENU: {
        POINT spt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (spt.x == -1 && spt.y == -1) GetCursorPos(&spt);
        self->ShowDockletOrItemContextMenu(spt, false);
        return 0;
    }

    case WM_COMMAND:
        self->OnCommand(LOWORD(wParam));
        return 0;

    case WM_DROPFILES: {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        POINT spt{};
        GetCursorPos(&spt);
        UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH] = {};
            if (DragQueryFileW(hDrop, i, path, MAX_PATH)) {
                self->AddItemAtDropPoint(path, spt);
            }
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_RK_LAYOUT_CHANGED:
        if (wParam == 0xFFFF) return self->options_.position;
        self->RequestLayoutAndRedraw();
        return 0;

    case WM_RK_ITEM_CHANGED:
        if (wParam == 1 && lParam) {
            auto* folder = reinterpret_cast<RKDockItemFolder*>(lParam);
            POINT spt{ (folder->screenRect.left + folder->screenRect.right) / 2,
                       (folder->screenRect.top + folder->screenRect.bottom) / 2 };
            self->menuOpen_ = true;
            RKHookDisable(5);
            folder->ShowFolderPopupMenu(hwnd, spt, self->options_);
            RKHookEnable(5);
            self->menuOpen_ = false;
        }
        self->dirty_ = true;
        self->RenderFrame();
        return 0;

    case WM_RK_DOCKLET_LOCK: {
        HWND owner = reinterpret_cast<HWND>(lParam);
        if (wParam != 0) {
            self->dockletLockOwner_ = owner ? owner : hwnd;
            self->textWnd_.HideLabel();
            self->SetHoverTimerInterval(25);
        } else if (!owner || !self->dockletLockOwner_ ||
                   self->dockletLockOwner_ == owner || !IsWindow(self->dockletLockOwner_)) {
            self->dockletLockOwner_ = nullptr;
            self->hoverTracker_.Reset();
        }
        return 0;
    }

    case WM_RK_SAVE_ITEMS:
        if (self->options_.saveOnChange && self->options_.saveItems) self->SaveAllConfig();
        self->RequestLayoutAndRedraw();
        return 0;

    case WM_RK_ATTENTION:
        if (wParam == 1) {
            self->WakeFromAutoHide();
        } else {
            self->MarkDirty();
        }
        return 0;

    case WM_RK_SHOW_PROPERTIES:
        if (lParam) {
            auto* item = reinterpret_cast<RKDockItem*>(lParam);
            if (ShowItemPropertiesDialog(hwnd, item)) {
                if (self->options_.saveOnChange && self->options_.saveItems) self->SaveAllConfig();
                self->RequestLayoutAndRedraw();
            }
        }
        return 0;

    case WM_MOUSEACTIVATE:
        if (self->options_.neverTakeFocus) return MA_NOACTIVATE;
        break;

    case WM_CLOSE:
        self->Destroy();
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        if (!self->destroyed_) PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace rk
