#include "stack_docklet_internal.h"
#include "stack_docklet_res.h"
#include <mmsystem.h>
#include <atomic>

namespace stackdock {

namespace {

constexpr wchar_t kPopupClass[]       = L"RK_StackDockletPopup_x64";
constexpr wchar_t kGridTooltipClass[] = L"RK_StackDockletTooltip_x64";
constexpr UINT_PTR kAnimTimerId       = 1;
constexpr UINT_PTR kFocusWatchTimerId = 2;
constexpr UINT WM_APP_ANIM_STEP       = WM_APP + 0x51;
constexpr UINT kAnimTimerIntervalMs   = 10;
constexpr DWORD kAnimDurationFanMs       = 250;
constexpr DWORD kAnimDurationGridOpenMs  = 250;
constexpr DWORD kAnimDurationGridCloseMs = 190;

struct AnimPulseState {
    HWND hwnd = nullptr;
    std::atomic<bool> active{ false };
    std::atomic<bool> queued{ false };
};

float EaseOutQuad(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    float inv = 1.0f - t;
    return 1.0f - inv * inv;
}

float EaseOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

std::shared_ptr<Gdiplus::Bitmap> MakePopupIcon48(const std::shared_ptr<Gdiplus::Bitmap>& src) {
    if (!src) return nullptr;
    if (src->GetWidth() == 48 && src->GetHeight() == 48) return src;
    auto out = std::make_shared<Gdiplus::Bitmap>(48, 48, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(out.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g.DrawImage(src.get(), 0, 0, 48, 48);
    return out;
}

std::wstring FormatMoreTextSafe(const std::wstring& tmpl, int count) {
    std::wstring fmt = tmpl.empty() ? L"%d more..." : tmpl;
    size_t pos = fmt.find(L"%d");
    if (pos != std::wstring::npos) {
        fmt.replace(pos, 2, std::to_wstring(count));
        return fmt;
    }
    return fmt + L" (" + std::to_wstring(count) + L")";
}

void AddCapsuleWindingPath(Gdiplus::GraphicsPath& path, float rx, float ry, float rw, float rh) {
    path.SetFillMode(Gdiplus::FillModeWinding);
    if (rw > 0.0f) {
        path.AddRectangle(Gdiplus::RectF(rx, ry, rw, rh));
    }
    path.AddEllipse(rx - rh * 0.5f, ry, rh, rh);
    path.AddEllipse(rx + rw - rh * 0.5f, ry, rh, rh);
}

void AddGridPanelPath32(Gdiplus::GraphicsPath& path, float x, float y, float w, float h,
                        int edge, float ptrX, float ptrY)
{
    path.SetFillMode(Gdiplus::FillModeWinding);
    constexpr float kDia = 20.0f; // 10px corner radius (matches 32-bit 0x41a00000)
    constexpr float kRad = 10.0f;
    constexpr float kPtr = 15.0f; // 15px pointer triangle (matches 32-bit 0x41700000)

    path.AddEllipse(x, y, kDia, kDia);
    path.AddEllipse(x + w - kDia, y, kDia, kDia);
    path.AddEllipse(x, y + h - kDia, kDia, kDia);
    path.AddEllipse(x + w - kDia, y + h - kDia, kDia, kDia);
    path.AddRectangle(Gdiplus::RectF(x + kRad, y, w - kDia, h));
    path.AddRectangle(Gdiplus::RectF(x, y + kRad, w, h - kDia));

    Gdiplus::PointF pts[3];
    if (edge == EDGE_BOTTOM) {
        float tx = std::clamp(ptrX, x + kRad + kPtr, x + w - kRad - kPtr);
        pts[0] = Gdiplus::PointF(tx - kPtr, y + h);
        pts[1] = Gdiplus::PointF(tx + kPtr, y + h);
        pts[2] = Gdiplus::PointF(tx, y + h + kPtr);
    } else if (edge == EDGE_TOP) {
        float tx = std::clamp(ptrX, x + kRad + kPtr, x + w - kRad - kPtr);
        pts[0] = Gdiplus::PointF(tx - kPtr, y);
        pts[1] = Gdiplus::PointF(tx + kPtr, y);
        pts[2] = Gdiplus::PointF(tx, y - kPtr);
    } else if (edge == EDGE_LEFT) {
        float ty = std::clamp(ptrY, y + kRad + kPtr, y + h - kRad - kPtr);
        pts[0] = Gdiplus::PointF(x, ty - kPtr);
        pts[1] = Gdiplus::PointF(x, ty + kPtr);
        pts[2] = Gdiplus::PointF(x - kPtr, ty);
    } else {
        float ty = std::clamp(ptrY, y + kRad + kPtr, y + h - kRad - kPtr);
        pts[0] = Gdiplus::PointF(x + w, ty - kPtr);
        pts[1] = Gdiplus::PointF(x + w, ty + kPtr);
        pts[2] = Gdiplus::PointF(x + w + kPtr, ty);
    }
    path.AddPolygon(pts, 3);
}

using GpRectF  = Gdiplus::RectF;
using GpPointF = Gdiplus::PointF;

struct PopupSlot {
    int itemIndex = -1; // >=0: inst->items[itemIndex]; -1: Open folder; -2: %d more
    int moreCount = 0;
    std::wstring label;
    std::wstring dispLabel;
    float dispW = 0.0f;
    float dispH = 0.0f;
    std::shared_ptr<Gdiplus::Bitmap> icon;
    std::shared_ptr<Gdiplus::Bitmap> icon48;
    std::shared_ptr<Gdiplus::Bitmap> labelBmp;

    // Grid coordinates (local to popup window)
    float cellX = 0.0f;
    float cellY = 0.0f;
    RECT  hitRect{};

    // Fan inverse-transform hit data
    Gdiplus::REAL fanMat[6]{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    Gdiplus::REAL pillMat[6]{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    GpRectF       pillHitLocal{};
    bool          hasFanMat = false;
    bool          hasPillMat = false;
};

struct DisplayEntryRef {
    int itemIndex = -1;
    int moreCount = 0;
    std::wstring label;
    std::shared_ptr<Gdiplus::Bitmap> icon;
};

} // namespace

class StackPopupWindow {
public:
    explicit StackPopupWindow(StackDockletInstance* inst)
        : inst_(inst)
    {
        QueryPerformanceFrequency(&qpcFreq_);
        LoadGlobalConfig(global_);
        openFolderBmp_ = LoadImageAsset(inst_, L"Open folder.png");
        shadowBmp_     = LoadImageAsset(inst_, L"Shadow.png");
        popupFont_     = MakePopupFont();
    }

    ~StackPopupWindow() {
        StopAnimTimer();
        DestroyGridTooltip();
        ReleaseFrameSurface();
        if (hwnd_ && IsWindow(hwnd_)) {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    bool Show(const POINT* ptCursor, const SIZE* dockletSize) {
        if (!RegisterPopupClass()) return false;

        RefreshStackItems(inst_);

        RECT rcAnchor{};
        if (inst_->host.getRect && inst_->hwndDocklet) {
            inst_->host.getRect(inst_->hwndDocklet, &rcAnchor);
        }
        if (IsRectEmpty(&rcAnchor)) {
            POINT pt{};
            if (ptCursor && (ptCursor->x != 0 || ptCursor->y != 0)) {
                pt = *ptCursor;
            } else {
                GetCursorPos(&pt);
            }
            int sw = (dockletSize && dockletSize->cx > 0) ? dockletSize->cx : 48;
            int sh = (dockletSize && dockletSize->cy > 0) ? dockletSize->cy : 48;
            rcAnchor = { pt.x - sw / 2, pt.y - sh / 2, pt.x + sw / 2, pt.y + sh / 2 };
        }
        anchorRect_ = rcAnchor;

        HMONITOR hMon = MonitorFromRect(&anchorRect_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(hMon, &mi)) {
            mi.rcWork = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        }
        workRect_ = mi.rcWork;

        int hostEdge = EDGE_BOTTOM;
        if (inst_->host.queryDockEdge && inst_->hwndDocklet) {
            hostEdge = inst_->host.queryDockEdge(inst_->hwndDocklet);
        }
        if (global_.followDock) {
            dockEdge_ = hostEdge;
        } else {
            dockEdge_ = (hostEdge == EDGE_TOP) ? EDGE_TOP : EDGE_BOTTOM;
        }

        BuildDisplayEntries();
        ComputeLayout();

        hwnd_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kPopupClass,
            L"StackDockletPopup",
            WS_POPUP,
            wndRect_.left, wndRect_.top,
            wndRect_.right - wndRect_.left,
            wndRect_.bottom - wndRect_.top,
            nullptr, nullptr, PluginModule(), this);
        if (!hwnd_) return false;

        inst_->activePopup = this;
        inst_->popupOpen   = true;
        if (inst_->host.lockMouseEffect && inst_->hwndDocklet) {
            inst_->host.lockMouseEffect(inst_->hwndDocklet, TRUE);
        }
        UpdateDockletIcon(inst_);

        closing_       = false;
        everActivated_ = false;
        openTick_      = GetTickCount();
        animProgress_  = useGrid_ ? 0.03f : 0.01f;
        RenderFrame();

        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        StartAnimTimer(animProgress_);
        SetTimer(hwnd_, kFocusWatchTimerId, 45, nullptr);
        return true;
    }

    void BeginClose(bool immediate) {
        if (!hwnd_) return;
        DestroyGridTooltip();
        if (closing_ && !immediate) return;
        closing_ = true;
        StopAnimTimer();
        KillTimer(hwnd_, kFocusWatchTimerId);

        if (inst_) {
            inst_->popupOpen = false;
            inst_->lastPopupCloseTick = GetTickCount64();
            if (inst_->host.lockMouseEffect && inst_->hwndDocklet) {
                inst_->host.lockMouseEffect(inst_->hwndDocklet, FALSE);
            }
            UpdateDockletIcon(inst_);
        }

        if (immediate || animProgress_ <= 0.01f) {
            FinishDestroy();
            return;
        }
        StartAnimTimer(animProgress_);
    }

private:

    static bool RegisterPopupClass() {
        static bool registered = false;
        if (registered) return true;
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = StaticWndProc;
        wc.hInstance     = PluginModule();
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPopupClass;
        bool ok1 = RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

        WNDCLASSEXW wct{};
        wct.cbSize        = sizeof(wct);
        wct.lpfnWndProc   = DefWindowProcW;
        wct.hInstance     = PluginModule();
        wct.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wct.lpszClassName = kGridTooltipClass;
        bool ok2 = RegisterClassExW(&wct) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

        if (ok1 && ok2) {
            registered = true;
            return true;
        }
        return false;
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* self = static_cast<StackPopupWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        auto* self = reinterpret_cast<StackPopupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }

    void DestroyGridTooltip() {
        tooltipSlotIdx_ = -1;
        if (tipHwnd_ && IsWindow(tipHwnd_)) {
            DestroyWindow(tipHwnd_);
        }
        tipHwnd_ = nullptr;
    }

    void HideGridTooltip() {
        tooltipSlotIdx_ = -1;
        if (tipHwnd_ && IsWindow(tipHwnd_) && IsWindowVisible(tipHwnd_)) {
            ShowWindow(tipHwnd_, SW_HIDE);
        }
    }

    void UpdateGridTooltip(int slotIdx) {
        if (!useGrid_ || closing_ || slotIdx < 0 || slotIdx >= static_cast<int>(slots_.size())) {
            HideGridTooltip();
            return;
        }
        if (slotIdx == tooltipSlotIdx_ && tipHwnd_ && IsWindowVisible(tipHwnd_)) {
            return;
        }
        tooltipSlotIdx_ = slotIdx;
        const auto& slot = slots_[slotIdx];
        if (slot.label.empty()) {
            HideGridTooltip();
            return;
        }

        if (!tipHwnd_) {
            tipHwnd_ = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                kGridTooltipClass,
                L"",
                WS_POPUP,
                0, 0, 1, 1,
                hwnd_, nullptr, PluginModule(), nullptr);
            if (!tipHwnd_) return;
        }

        Gdiplus::Bitmap measureBmp(1, 1, PixelFormat32bppPARGB);
        Gdiplus::Graphics measureG(&measureBmp);
        measureG.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        auto font = MakePopupFont();
        Gdiplus::StringFormat sfTyp(Gdiplus::StringFormat::GenericTypographic());
        sfTyp.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                             Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
        Gdiplus::RectF bounds{};
        measureG.MeasureString(slot.label.c_str(), static_cast<INT>(slot.label.size()),
                               font.get(), GpPointF(0.0f, 0.0f), &sfTyp, &bounds);

        float tw = bounds.Width;
        float th = bounds.Height;
        float rw = (std::max)(0.0f, tw + 3.0f);
        float rh = std::ceil(th) + 3.0f;
        int tipW = (std::max)(400, static_cast<int>(std::ceil(rw + rh + 16.0f)));
        int tipH = (std::max)(22, static_cast<int>(std::ceil(rh + 5.0f)));

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = tipW;
        bi.bmiHeader.biHeight      = -tipH;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC screenDC = GetDC(nullptr);
        HDC memDC    = CreateCompatibleDC(screenDC);
        void* bits   = nullptr;
        HBITMAP hDib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (hDib) {
            HGDIOBJ oldBmp = SelectObject(memDC, hDib);
            memset(bits, 0, static_cast<size_t>(tipW) * tipH * 4);

            {
                Gdiplus::Graphics g(memDC);
                g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

                // Match 32-bit tooltip pill: white (0xe0ffffff), black text (0xe0000000), centered cleanly
                float rx = (static_cast<float>(tipW) - rw) * 0.5f;
                float ry = 2.0f;

                Gdiplus::GraphicsPath pill;
                AddCapsuleWindingPath(pill, rx, ry, rw, rh);
                Gdiplus::SolidBrush bg(Gdiplus::Color(224, 255, 255, 255));
                g.FillPath(&bg, &pill);

                sfTyp.SetAlignment(Gdiplus::StringAlignmentCenter);
                sfTyp.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                Gdiplus::SolidBrush fg(Gdiplus::Color(224, 0, 0, 0));
                g.DrawString(slot.label.c_str(), static_cast<INT>(slot.label.size()),
                             font.get(), GpRectF(rx, ry, rw, rh), &sfTyp, &fg);
            }

            int sx = wndRect_.left + static_cast<int>(slot.cellX) + 40 - tipW / 2;
            int sy = wndRect_.top + static_cast<int>(slot.cellY) + 46;
            POINT ptSrc{ 0, 0 };
            POINT ptDst{ sx, sy };
            SIZE szWnd{ tipW, tipH };
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            SetWindowPos(tipHwnd_, HWND_TOPMOST, sx, sy, tipW, tipH,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            UpdateLayeredWindow(tipHwnd_, screenDC, &ptDst, &szWnd, memDC, &ptSrc, 0, &bf, ULW_ALPHA);

            SelectObject(memDC, oldBmp);
            DeleteObject(hDib);
        }
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
    }

    void FinishDestroy() {
        StopAnimTimer();
        DestroyGridTooltip();
        HWND h = hwnd_;
        hwnd_ = nullptr;
        if (h && IsWindow(h)) {
            KillTimer(h, kFocusWatchTimerId);
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            DestroyWindow(h);
        }
        if (inst_ && inst_->activePopup == this) {
            inst_->activePopup = nullptr;
            inst_->popupOpen = false;
        }
        delete this;
    }

    std::unique_ptr<Gdiplus::Font> MakePopupFont(float sizeScale = 1.0f) const {
        INT style = global_.fontBold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular;
        float pt = (std::max)(6.0f, static_cast<float>(global_.fontSize) * sizeScale);
        Gdiplus::FontFamily ff(global_.fontName.c_str());
        if (ff.IsAvailable()) {
            return std::make_unique<Gdiplus::Font>(&ff, pt, style, Gdiplus::UnitPoint);
        }
        Gdiplus::FontFamily fallback(L"Tahoma");
        if (fallback.IsAvailable()) {
            return std::make_unique<Gdiplus::Font>(&fallback, pt, style, Gdiplus::UnitPoint);
        }
        Gdiplus::FontFamily sans(L"Segoe UI");
        return std::make_unique<Gdiplus::Font>(&sans, pt, style, Gdiplus::UnitPoint);
    }

    static std::wstring TruncateGridLabelMiddle32(Gdiplus::Graphics& g, Gdiplus::Font* font,
                                                  const Gdiplus::StringFormat& sf,
                                                  const std::wstring& text,
                                                  float* outWidth, float* outHeight = nullptr)
    {
        auto measure = [&](const std::wstring& s, float* outH = nullptr) -> float {
            Gdiplus::RectF bounds{};
            g.MeasureString(s.c_str(), static_cast<INT>(s.size()), font, GpPointF(0.0f, 0.0f), &sf, &bounds);
            if (outH) *outH = bounds.Height;
            return bounds.Width;
        };
        float h = 0.0f;
        float w = measure(text, &h);
        if (w <= 80.0f) {
            if (outWidth) *outWidth = w;
            if (outHeight) *outHeight = h;
            return text;
        }
        std::wstring s = text;
        while (s.size() > 1) {
            size_t delIdx = (s.size() - 1) / 2;
            s.erase(delIdx, 1);
            w = measure(s, &h);
            if (w <= 70.0f) break;
        }
        s.insert(s.size() / 2, L"...");
        w = measure(s, &h);
        if (outWidth) *outWidth = w;
        if (outHeight) *outHeight = h;
        return s;
    }

    static std::shared_ptr<Gdiplus::Bitmap> MakeSlotLabelBitmap(Gdiplus::Font* font,
                                                                const Gdiplus::StringFormat& sf,
                                                                const std::wstring& label,
                                                                float dispW, float dispH)
    {
        if (!font || label.empty() || dispW <= 0.5f || dispH <= 0.5f) return nullptr;
        int bw = (std::max)(1, static_cast<int>(std::ceil(dispW)) + 6);
        int bh = (std::max)(1, static_cast<int>(std::ceil(dispH)) + 4);
        auto bmp = std::make_shared<Gdiplus::Bitmap>(bw, bh, PixelFormat32bppPARGB);
        Gdiplus::Graphics g(bmp.get());
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
        g.DrawString(label.c_str(), static_cast<INT>(label.size()), font, GpPointF(3.0f, 2.0f), &sf, &white);
        return bmp;
    }

    static std::wstring TruncateFanLabelEnd32(Gdiplus::Graphics& g, Gdiplus::Font* font,
                                               const Gdiplus::StringFormat& sf,
                                               const std::wstring& text, float* outWidth, float* outHeight)
    {
        auto measure = [&](const std::wstring& s, float& w, float& h) {
            Gdiplus::RectF bounds{};
            g.MeasureString(s.c_str(), static_cast<INT>(s.size()), font, GpPointF(0.0f, 0.0f), &sf, &bounds);
            w = bounds.Width;
            h = bounds.Height;
        };
        float w = 0.0f, h = 0.0f;
        measure(text, w, h);
        if (w <= 220.0f) {
            if (outWidth) *outWidth = w;
            if (outHeight) *outHeight = h;
            return text;
        }
        std::wstring s = text;
        while (s.size() > 1) {
            s.pop_back();
            measure(s, w, h);
            if (w <= 210.0f) break;
        }
        s += L"...";
        measure(s, w, h);
        if (outWidth) *outWidth = w;
        if (outHeight) *outHeight = h;
        return s;
    }

    void BuildDisplayEntries() {
        entries_.clear();
        for (int i = 0; i < static_cast<int>(inst_->items.size()); ++i) {
            DisplayEntryRef r;
            r.itemIndex = i;
            r.label     = inst_->items[i].name;
            r.icon      = inst_->items[i].icon;
            entries_.push_back(std::move(r));
        }
        if (!inst_->hideOpenFolder) {
            DisplayEntryRef openRef;
            openRef.itemIndex = -1;
            openRef.label     = global_.openFolderText.empty() ? L"Open folder" : global_.openFolderText;
            openRef.icon      = openFolderBmp_;
            entries_.push_back(std::move(openRef));
        }
    }

    void ComputeLayout() {
        int workW = workRect_.right - workRect_.left;
        int workH = workRect_.bottom - workRect_.top;
        bool horiz = (dockEdge_ == EDGE_LEFT || dockEdge_ == EDGE_RIGHT);
        // 32-bit 0x467024: maxSlots = monitorDim / 50; maxFanItems = maxSlots - 4
        int workDim = horiz ? workW : workH;
        maxMonitorSlots_ = (std::max)(6, workDim / 50);
        int maxFanItems  = (std::max)(2, maxMonitorSlots_ - 4);

        int effMode = inst_->mode;
        if (effMode == MODE_AUTOMATIC) {
            effMode = (static_cast<int>(entries_.size()) <= maxFanItems) ? MODE_FAN : MODE_GRID;
        }
        useGrid_ = (effMode == MODE_GRID);
        if (useGrid_) BuildGridLayout(workW, workH);
        else          BuildFanLayout(maxFanItems);
    }

    void BuildGridLayout(int workW, int workH) {
        slots_.clear();
        gridSteadyBmp_.reset();
        constexpr int kCellW   = 80;  // 0x50 in 32-bit
        constexpr int kCellH   = 80;
        constexpr int kBoxPad  = 20;  // 10px each side
        constexpr int kShadow2 = 96;  // 48px each side
        constexpr int kShadow  = 48;
        constexpr int kTail    = 15;  // 0xf

        int total = static_cast<int>(entries_.size());
        int n = (total > 0) ? total : 1;

        // Exact 32-bit 0x4642b7..0x46433f column/row expansion loop
        int cols = 2;
        int rows = 1;
        while (cols * rows < n) {
            bool grew = false;
            if (workW >= (cols + 1) * kCellW + kBoxPad + kShadow2 + kTail) {
                ++cols;
                grew = true;
            }
            if (cols * rows >= n) break;
            if (workH >= (rows + 1) * kCellH + kBoxPad + kShadow2 + kTail) {
                ++rows;
                grew = true;
            }
            if (!grew) break;
        }

        gridCols_ = cols;
        visibleRows_ = rows;
        totalRows_ = (total + cols - 1) / cols;
        if (totalRows_ < visibleRows_) totalRows_ = visibleRows_;
        scrollRow_ = std::clamp(scrollRow_, 0, (std::max)(0, totalRows_ - visibleRows_));

        int boxW = cols * kCellW + kBoxPad;
        int boxH = visibleRows_ * kCellH + kBoxPad;

        int leftPad   = kShadow + (dockEdge_ == EDGE_LEFT ? kTail : 0);
        int rightPad  = kShadow + (dockEdge_ == EDGE_RIGHT ? kTail : 0);
        int topPad    = kShadow + (dockEdge_ == EDGE_TOP ? kTail : 0);
        int bottomPad = kShadow + (dockEdge_ == EDGE_BOTTOM ? kTail : 0);

        int wndW = boxW + leftPad + rightPad;
        int wndH = boxH + topPad + bottomPad;

        int ax = (anchorRect_.left + anchorRect_.right) / 2;
        int ay = (anchorRect_.top + anchorRect_.bottom) / 2;
        if (dockEdge_ == EDGE_BOTTOM) ay = anchorRect_.top;
        else if (dockEdge_ == EDGE_TOP) ay = anchorRect_.bottom;
        else if (dockEdge_ == EDGE_LEFT) ax = anchorRect_.right;
        else ax = anchorRect_.left;

        int wx = 0, wy = 0;
        if (dockEdge_ == EDGE_BOTTOM) {
            wx = ax - leftPad - boxW / 2;
            wy = ay - topPad - boxH - kTail;
        } else if (dockEdge_ == EDGE_TOP) {
            wx = ax - leftPad - boxW / 2;
            wy = ay - kShadow; // pointer tip (y = kShadow) meets anchorRect_.bottom
        } else if (dockEdge_ == EDGE_LEFT) {
            wx = ax - kShadow; // pointer tip (x = kShadow) meets anchorRect_.right
            wy = ay - topPad - boxH / 2;
        } else {
            wx = ax - leftPad - boxW - kTail;
            wy = ay - topPad - boxH / 2;
        }

        // Clamp visible panel inside monitor workArea while keeping pointer apex aligned on dock icon
        int minWx = workRect_.left - leftPad;
        int maxWx = workRect_.right - leftPad - boxW;
        int minWy = workRect_.top - topPad;
        int maxWy = workRect_.bottom - topPad - boxH;
        if (minWx <= maxWx) wx = std::clamp(wx, minWx, maxWx);
        if (minWy <= maxWy) wy = std::clamp(wy, minWy, maxWy);

        wndRect_ = { wx, wy, wx + wndW, wy + wndH };
        gridBoxLocal_ = GpRectF(static_cast<float>(leftPad), static_cast<float>(topPad),
                                static_cast<float>(boxW), static_cast<float>(boxH));
        anchorLocal_ = GpPointF(static_cast<float>(ax - wx), static_cast<float>(ay - wy));

        if (!entries_.empty()) {
            Gdiplus::Bitmap measureBmp(1, 1, PixelFormat32bppPARGB);
            Gdiplus::Graphics measureG(&measureBmp);
            measureG.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            Gdiplus::StringFormat measureSf;
            measureSf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            Gdiplus::Font* font = popupFont_.get();

            int firstIdx = scrollRow_ * gridCols_;
            int maxSlots = visibleRows_ * gridCols_;
            int avail = (std::min)(static_cast<int>(entries_.size()) - firstIdx, maxSlots);
            for (int k = 0; k < avail; ++k) {
                int entryPos = firstIdx + k;
                // In 32-bit 0x464d94: if overflowing without scroll, last slot shows Open folder
                if (scrollRow_ == 0 && total > maxSlots && k == maxSlots - 1 && !inst_->hideOpenFolder) {
                    entryPos = static_cast<int>(entries_.size()) - 1;
                }
                int r = k / gridCols_;
                int c = k % gridCols_;
                float cx = static_cast<float>(kShadow + 10 + (dockEdge_ == EDGE_LEFT ? kTail : 0) + c * kCellW);
                float cy = static_cast<float>(kShadow + 15 + (dockEdge_ == EDGE_TOP ? kTail : 0) + r * kCellH);

                PopupSlot slot;
                slot.itemIndex = entries_[entryPos].itemIndex;
                slot.moreCount = entries_[entryPos].moreCount;
                slot.label     = entries_[entryPos].label;
                slot.icon      = entries_[entryPos].icon;
                slot.icon48    = MakePopupIcon48(slot.icon);
                slot.dispLabel = TruncateGridLabelMiddle32(measureG, font, measureSf, slot.label, &slot.dispW, &slot.dispH);
                slot.labelBmp  = MakeSlotLabelBitmap(font, measureSf, slot.dispLabel, slot.dispW, slot.dispH);
                slot.cellX     = cx;
                slot.cellY     = cy;
                slot.hitRect   = {
                    static_cast<LONG>(cx),
                    static_cast<LONG>(cy),
                    static_cast<LONG>(cx + kCellW),
                    static_cast<LONG>(cy + kCellH)
                };
                slots_.push_back(std::move(slot));
            }
        }
    }

    void BuildFanLayout(int maxFanSlots) {
        slots_.clear();
        gridSteadyBmp_.reset();
        std::vector<DisplayEntryRef> visible;
        int total = static_cast<int>(entries_.size());
        if (total <= maxFanSlots) {
            visible = entries_;
        } else {
            bool hasOpen = !inst_->hideOpenFolder && !entries_.empty() && entries_.back().itemIndex == -1;
            int reserve = hasOpen ? 2 : 1;
            int keep = (std::max)(0, maxFanSlots - reserve);
            for (int i = 0; i < keep; ++i) visible.push_back(entries_[i]);
            int remaining = (total - (hasOpen ? 1 : 0)) - keep;
            if (remaining > 0) {
                DisplayEntryRef moreRef;
                moreRef.itemIndex = -2;
                moreRef.moreCount = remaining;
                moreRef.label     = FormatMoreTextSafe(global_.dMoreText, remaining);
                moreRef.icon      = openFolderBmp_;
                visible.push_back(std::move(moreRef));
            }
            if (hasOpen) visible.push_back(entries_.back());
        }

        if (visible.empty()) {
            BuildGridLayout(workRect_.right - workRect_.left, workRect_.bottom - workRect_.top);
            useGrid_ = true;
            return;
        }

        int n = static_cast<int>(visible.size());
        int spanPx = (n + 1) * 50 + 180;
        constexpr int kCrossPx = 460;

        int ax = (anchorRect_.left + anchorRect_.right) / 2;
        int ay = (anchorRect_.top + anchorRect_.bottom) / 2;
        if (dockEdge_ == EDGE_BOTTOM) ay = anchorRect_.top;
        else if (dockEdge_ == EDGE_TOP) ay = anchorRect_.bottom;
        else if (dockEdge_ == EDGE_LEFT) ax = anchorRect_.right;
        else ax = anchorRect_.left;

        // 32-bit 0x46588a angleStep formula
        fanAngleStep_ = 1.5f;
        if (maxMonitorSlots_ > 18) {
            fanAngleStep_ *= 8.0f / static_cast<float>(maxMonitorSlots_);
        } else if (maxMonitorSlots_ > 12) {
            fanAngleStep_ *= 12.0f / static_cast<float>(maxMonitorSlots_);
        }

        int wx = 0, wy = 0, wndW = 0, wndH = 0;
        fanPillsOnRight_ = false;
        if (dockEdge_ == EDGE_BOTTOM || dockEdge_ == EDGE_TOP) {
            wndW = kCrossPx;
            wndH = spanPx;
            // Default to pills on the LEFT matching 32-bit unless too close to left monitor edge
            if (ax - 300 < workRect_.left) {
                fanPillsOnRight_ = true;
                wx = ax - 120;
                anchorLocal_ = GpPointF(120.0f, 0.0f);
            } else {
                wx = ax - (wndW - 140);
                anchorLocal_ = GpPointF(static_cast<float>(wndW - 140), 0.0f);
            }
            if (dockEdge_ == EDGE_BOTTOM) {
                wy = ay - wndH + 48;
                anchorLocal_.Y = static_cast<float>(wndH - 48);
            } else {
                wy = ay - 48;
                anchorLocal_.Y = 48.0f;
            }
        } else {
            wndW = spanPx;
            wndH = kCrossPx;
            if (dockEdge_ == EDGE_LEFT) {
                wx = ax - 48;
                anchorLocal_.X = 48.0f;
            } else {
                wx = ax - wndW + 48;
                anchorLocal_.X = static_cast<float>(wndW - 48);
            }
            if (ay - 300 < workRect_.top) {
                wy = ay - 120;
                anchorLocal_.Y = 120.0f;
            } else {
                wy = ay - (wndH - 140);
                anchorLocal_.Y = static_cast<float>(wndH - 140);
            }
        }

        wndRect_ = { wx, wy, wx + wndW, wy + wndH };
        Gdiplus::Bitmap measureBmp(1, 1, PixelFormat32bppPARGB);
        Gdiplus::Graphics measureG(&measureBmp);
        measureG.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        Gdiplus::StringFormat measureSf;
        measureSf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::Font* font = popupFont_.get();

        for (int i = 0; i < n; ++i) {
            PopupSlot s;
            s.itemIndex = visible[i].itemIndex;
            s.moreCount = visible[i].moreCount;
            s.label     = visible[i].label;
            s.icon      = visible[i].icon;
            s.icon48    = MakePopupIcon48(s.icon);
            s.dispLabel = TruncateFanLabelEnd32(measureG, font, measureSf, s.label, &s.dispW, &s.dispH);
            slots_.push_back(std::move(s));
        }
    }

    void ReleaseFrameSurface() {
        gridSteadyBmp_.reset();
        if (frameOldBmp_ && frameMemDC_) {
            SelectObject(frameMemDC_, frameOldBmp_);
        }
        if (frameDib_) {
            DeleteObject(frameDib_);
            frameDib_ = nullptr;
        }
        if (frameMemDC_) {
            DeleteDC(frameMemDC_);
            frameMemDC_ = nullptr;
        }
        frameOldBmp_ = nullptr;
        frameBits_   = nullptr;
        frameW_      = 0;
        frameH_      = 0;
    }

    bool EnsureFrameSurface(HDC screenDC, int w, int h) {
        if (frameDib_ && frameMemDC_ && frameW_ == w && frameH_ == h && frameBits_) {
            return true;
        }
        ReleaseFrameSurface();

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        frameMemDC_ = CreateCompatibleDC(screenDC);
        if (!frameMemDC_) return false;
        frameDib_ = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &frameBits_, nullptr, 0);
        if (!frameDib_ || !frameBits_) {
            ReleaseFrameSurface();
            return false;
        }
        frameOldBmp_ = SelectObject(frameMemDC_, frameDib_);
        frameW_ = w;
        frameH_ = h;
        return true;
    }

    void RenderFrame() {
        if (!hwnd_) return;
        int w = wndRect_.right - wndRect_.left;
        int h = wndRect_.bottom - wndRect_.top;
        if (w <= 0 || h <= 0) return;

        HDC screenDC = GetDC(nullptr);
        if (!EnsureFrameSurface(screenDC, w, h)) {
            ReleaseDC(nullptr, screenDC);
            return;
        }
        memset(frameBits_, 0, static_cast<size_t>(w) * h * 4);

        {
            Gdiplus::Graphics g(frameMemDC_);
            g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            g.SetInterpolationMode(useGrid_ ? Gdiplus::InterpolationModeHighQualityBilinear
                                            : Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

            float t = std::clamp(animProgress_, 0.0f, 1.0f);
            Gdiplus::Font* font = popupFont_.get();

            if (useGrid_) RenderGrid(g, font, t);
            else          RenderFan(g, font, t);
        }

        // 32-bit 0x464144: UpdateLayeredWindow always uses constant alpha = 255
        POINT ptSrc{ 0, 0 };
        POINT ptDst{ wndRect_.left, wndRect_.top };
        SIZE szWnd{ w, h };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(hwnd_, screenDC, &ptDst, &szWnd, frameMemDC_, &ptSrc, 0, &bf, ULW_ALPHA);

        ReleaseDC(nullptr, screenDC);
    }

    void RenderGrid(Gdiplus::Graphics& g, Gdiplus::Font* font, float t) {
        auto blitSteady1To1 = [&](Gdiplus::Bitmap* bmp) {
            if (!bmp) return;
            auto oldInterp = g.GetInterpolationMode();
            auto oldPx     = g.GetPixelOffsetMode();
            auto oldComp   = g.GetCompositingMode();
            g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);
            g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            g.DrawImage(bmp, 0, 0, frameW_, frameH_);
            g.SetCompositingMode(oldComp);
            g.SetPixelOffsetMode(oldPx);
            g.SetInterpolationMode(oldInterp);
        };

        if (!closing_ && t >= 0.999f && gridSteadyBmp_ &&
            static_cast<int>(gridSteadyBmp_->GetWidth()) == frameW_ &&
            static_cast<int>(gridSteadyBmp_->GetHeight()) == frameH_) {
            blitSteady1To1(gridSteadyBmp_.get());
            return;
        }

        const bool captureSteady = (!closing_ && t >= 0.999f && frameW_ > 0 && frameH_ > 0);
        std::unique_ptr<Gdiplus::Bitmap> steadyCanvas;
        std::unique_ptr<Gdiplus::Graphics> steadyGraphics;
        if (captureSteady) {
            steadyCanvas = std::make_unique<Gdiplus::Bitmap>(frameW_, frameH_, PixelFormat32bppPARGB);
            steadyGraphics = std::make_unique<Gdiplus::Graphics>(steadyCanvas.get());
            steadyGraphics->SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            steadyGraphics->SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            steadyGraphics->SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);
            steadyGraphics->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            steadyGraphics->Clear(Gdiplus::Color(0, 0, 0, 0));
        }
        Gdiplus::Graphics& targetG = captureSteady ? *steadyGraphics : g;

        const GpRectF& b = gridBoxLocal_;
        float e = closing_ ? std::clamp(t, 0.0f, 1.0f) : EaseOutQuad(t);
        if (e <= 0.0f) return;

        Gdiplus::GraphicsPath body;
        AddGridPanelPath32(body, b.X, b.Y, b.Width, b.Height, dockEdge_, anchorLocal_.X, anchorLocal_.Y);

        Gdiplus::Matrix oldTx;
        targetG.GetTransform(&oldTx);
        targetG.TranslateTransform(anchorLocal_.X, anchorLocal_.Y);
        targetG.ScaleTransform(e, e);
        targetG.TranslateTransform(-anchorLocal_.X, -anchorLocal_.Y);

        BYTE bgAlpha = static_cast<BYTE>(std::clamp(176.0f * (closing_ ? t : EaseOutCubic(t)), 0.0f, 176.0f));
        Gdiplus::SolidBrush bg(Gdiplus::Color(bgAlpha, 0, 0, 0));
        targetG.FillPath(&bg, &body);
        targetG.SetClip(&body, Gdiplus::CombineModeReplace);
        targetG.SetTransform(&oldTx);

        if (slots_.empty()) {
            Gdiplus::StringFormat emptySf;
            emptySf.SetAlignment(Gdiplus::StringAlignmentCenter);
            emptySf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            BYTE ta = static_cast<BYTE>(std::clamp(220.0f * e, 0.0f, 255.0f));
            Gdiplus::SolidBrush txt(Gdiplus::Color(ta, 255, 255, 255));
            targetG.DrawString(L"(Empty folder)", -1, font, b, &emptySf, &txt);
            targetG.ResetClip();
            if (captureSteady) {
                steadyGraphics.reset();
                gridSteadyBmp_ = std::move(steadyCanvas);
                blitSteady1To1(gridSteadyBmp_.get());
            }
            return;
        }

        // Fan-like transition: each icon+label glides and scales out together inside the expanding panel
        float slotScale = 0.32f + 0.68f * e;
        float labelFade = std::clamp((e - 0.16f) / 0.64f, 0.0f, 1.0f);
        const bool useFadeAttr = (labelFade > 0.01f && labelFade < 0.999f);
        Gdiplus::ImageAttributes fadeAttr;
        if (useFadeAttr) {
            Gdiplus::ColorMatrix cm = {{
                { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 0.0f, labelFade, 0.0f },
                { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f }
            }};
            fadeAttr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
        }
        Gdiplus::StringFormat sf;
        sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        BYTE txtAlpha = static_cast<BYTE>(std::clamp(255.0f * labelFade, 0.0f, 255.0f));
        Gdiplus::SolidBrush txtBrush(Gdiplus::Color(txtAlpha, 255, 255, 255));

        int n = static_cast<int>(slots_.size());
        bool drawAsc = (dockEdge_ == EDGE_BOTTOM || dockEdge_ == EDGE_RIGHT);
        for (int step = 0; step < n; ++step) {
            int i = drawAsc ? step : (n - 1 - step);
            const auto& slot = slots_[i];
            float dstCX = slot.cellX + 40.0f;
            float dstCY = slot.cellY + 24.0f;
            float curCX = (1.0f - e) * anchorLocal_.X + e * dstCX;
            float curCY = (1.0f - e) * anchorLocal_.Y + e * dstCY;

            Gdiplus::Matrix slotSaved;
            targetG.GetTransform(&slotSaved);
            targetG.TranslateTransform(curCX, curCY);
            if (slotScale < 0.999f) {
                targetG.ScaleTransform(slotScale, slotScale);
            }

            if (slot.icon48) {
                if (slotScale >= 0.999f) {
                    auto oldIconPx = targetG.GetPixelOffsetMode();
                    auto oldIconInterp = targetG.GetInterpolationMode();
                    targetG.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);
                    targetG.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                    targetG.DrawImage(slot.icon48.get(), -24.0f, -24.0f, 48.0f, 48.0f);
                    targetG.SetInterpolationMode(oldIconInterp);
                    targetG.SetPixelOffsetMode(oldIconPx);
                } else {
                    targetG.DrawImage(slot.icon48.get(), -24.0f, -24.0f, 48.0f, 48.0f);
                }
            }
            if (labelFade > 0.01f) {
                if (captureSteady || !slot.labelBmp) {
                    // Steady state uses direct vector DrawString at pixel-snapped X for razor-sharp glyphs
                    float drawX = std::floor(-slot.dispW * 0.5f + 0.5f);
                    targetG.DrawString(slot.dispLabel.c_str(), static_cast<INT>(slot.dispLabel.size()),
                                       font, GpPointF(drawX, 26.0f), &sf, &txtBrush);
                } else {
                    const float lx = std::floor(-slot.dispW * 0.5f - 3.0f + 0.5f);
                    const float ly = 26.0f - 2.0f;
                    const float lw = static_cast<float>(slot.labelBmp->GetWidth());
                    const float lh = static_cast<float>(slot.labelBmp->GetHeight());
                    if (useFadeAttr) {
                        targetG.DrawImage(slot.labelBmp.get(), GpRectF(lx, ly, lw, lh),
                                          0.0f, 0.0f, lw, lh, Gdiplus::UnitPixel, &fadeAttr);
                    } else {
                        targetG.DrawImage(slot.labelBmp.get(), lx, ly, lw, lh);
                    }
                }
            }

            targetG.SetTransform(&slotSaved);
        }

        targetG.ResetClip();

        if (totalRows_ > visibleRows_ && t >= 0.99f) {
            float trackH = b.Height - 24.0f;
            float ratio = static_cast<float>(visibleRows_) / static_cast<float>(totalRows_);
            float thumbH = (std::max)(24.0f, trackH * ratio);
            float thumbY = b.Y + 12.0f + (trackH - thumbH) *
                (static_cast<float>(scrollRow_) / static_cast<float>((std::max)(1, totalRows_ - visibleRows_)));
            float thumbX = b.X + b.Width - 7.0f;
            Gdiplus::GraphicsPath sbPath;
            AddRoundedRectPath(sbPath, thumbX, thumbY, 4.0f, thumbH, 2.0f);
            Gdiplus::SolidBrush sbBrush(Gdiplus::Color(150, 220, 230, 245));
            targetG.FillPath(&sbBrush, &sbPath);
        }

        if (captureSteady) {
            steadyGraphics.reset();
            gridSteadyBmp_ = std::move(steadyCanvas);
            blitSteady1To1(gridSteadyBmp_.get());
        }
    }

    void RenderFan(Gdiplus::Graphics& g, Gdiplus::Font* font, float t) {
        int n = static_cast<int>(slots_.size());
        if (n <= 0) return;

        Gdiplus::StringFormat sf;
        sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        Gdiplus::Matrix saved;
        g.GetTransform(&saved);

        // Origin aligned to dock edge (0x465902..0x465a7d)
        float bx = anchorLocal_.X;
        float by = anchorLocal_.Y;
        if (dockEdge_ == EDGE_BOTTOM)      by = anchorLocal_.Y + 32.0f * (1.0f - t);
        else if (dockEdge_ == EDGE_TOP)    by = anchorLocal_.Y - 32.0f * (1.0f - t);
        else if (dockEdge_ == EDGE_LEFT)   bx = anchorLocal_.X - 32.0f * (1.0f - t);
        else                               bx = anchorLocal_.X + 32.0f * (1.0f - t);
        g.TranslateTransform(bx, by);

        // Initial half-step along primary axis (0x465a82)
        float halfStep = 25.0f * t;
        float stepRot  = fanAngleStep_ * t;
        auto pushStep = [&]() {
            if (dockEdge_ == EDGE_BOTTOM) {
                g.TranslateTransform(0.0f, -halfStep);
                g.RotateTransform(fanPillsOnRight_ ? -stepRot : stepRot);
                g.TranslateTransform(0.0f, -halfStep);
            } else if (dockEdge_ == EDGE_TOP) {
                g.TranslateTransform(0.0f, halfStep);
                g.RotateTransform(fanPillsOnRight_ ? stepRot : -stepRot);
                g.TranslateTransform(0.0f, halfStep);
            } else if (dockEdge_ == EDGE_LEFT) {
                g.TranslateTransform(halfStep, 0.0f);
                g.RotateTransform(stepRot);
                g.TranslateTransform(halfStep, 0.0f);
            } else {
                g.TranslateTransform(-halfStep, 0.0f);
                g.RotateTransform(-stepRot);
                g.TranslateTransform(-halfStep, 0.0f);
            }
        };
        auto popStep = [&]() {
            if (dockEdge_ == EDGE_BOTTOM) {
                g.RotateTransform(fanPillsOnRight_ ? stepRot : -stepRot);
                g.TranslateTransform(0.0f, halfStep);
            } else if (dockEdge_ == EDGE_TOP) {
                g.RotateTransform(fanPillsOnRight_ ? -stepRot : stepRot);
                g.TranslateTransform(0.0f, -halfStep);
            } else if (dockEdge_ == EDGE_LEFT) {
                g.RotateTransform(-stepRot);
                g.TranslateTransform(-halfStep, 0.0f);
            } else {
                g.RotateTransform(stepRot);
                g.TranslateTransform(halfStep, 0.0f);
            }
        };

        if (dockEdge_ == EDGE_BOTTOM)      g.TranslateTransform(0.0f, -halfStep);
        else if (dockEdge_ == EDGE_TOP)    g.TranslateTransform(0.0f, halfStep);
        else if (dockEdge_ == EDGE_LEFT)   g.TranslateTransform(halfStep, 0.0f);
        else                               g.TranslateTransform(-halfStep, 0.0f);

        for (int i = 0; i < n; ++i) {
            pushStep();
        }

        float lastDenom = static_cast<float>((std::max)(1, n - 1));
        BYTE pillAlpha = static_cast<BYTE>(std::clamp(160.0f * t, 0.0f, 160.0f));
        BYTE textAlpha = static_cast<BYTE>(std::clamp(255.0f * t, 0.0f, 255.0f));
        Gdiplus::SolidBrush pillBrush(Gdiplus::Color(pillAlpha, 0, 0, 0));
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(textAlpha, 255, 255, 255));

        for (int i = n - 1; i >= 0; --i) {
            if (dockEdge_ == EDGE_BOTTOM)      g.TranslateTransform(0.0f, halfStep);
            else if (dockEdge_ == EDGE_TOP)    g.TranslateTransform(0.0f, -halfStep);
            else if (dockEdge_ == EDGE_LEFT)   g.TranslateTransform(-halfStep, 0.0f);
            else                               g.TranslateTransform(halfStep, 0.0f);

            auto& slot = slots_[i];

            // Record transform matrix for accurate inverse-transformed Fan hit testing
            Gdiplus::Matrix curTx;
            g.GetTransform(&curTx);
            curTx.GetElements(slot.fanMat);
            slot.hasFanMat = true;

            float frac = static_cast<float>(i) / lastDenom;
            float itemScale = frac * (0.7f * t + 0.3f) + (1.0f - frac);

            {
                Gdiplus::Matrix itemScaleTx;
                g.GetTransform(&itemScaleTx);
                g.ScaleTransform(itemScale, itemScale);
                if (shadowBmp_) {
                    g.DrawImage(shadowBmp_.get(), -24.0f, -18.0f, 48.0f, 48.0f);
                }
                if (slot.icon48) {
                    g.DrawImage(slot.icon48.get(), -24.0f, -24.0f, 48.0f, 48.0f);
                }
                g.SetTransform(&itemScaleTx);
            }

            if (t > 0.01f) {
                float tw = slot.dispW;
                float th = slot.dispH;
                const std::wstring& disp = slot.dispLabel;

                Gdiplus::Matrix pillSaved;
                g.GetTransform(&pillSaved);

                float rw = (std::max)(0.0f, tw - 5.0f);
                float rh = th + 4.0f;
                float ry = -th * 0.5f - 2.0f;

                if (dockEdge_ == EDGE_LEFT) {
                    g.RotateTransform(-90.0f);
                    g.TranslateTransform(tw + 68.0f, 0.0f);
                } else if (dockEdge_ == EDGE_RIGHT) {
                    g.RotateTransform(90.0f);
                }

                if (fanPillsOnRight_ && (dockEdge_ == EDGE_BOTTOM || dockEdge_ == EDGE_TOP)) {
                    g.TranslateTransform(34.0f, 0.0f);
                    g.ScaleTransform(t, t);
                    float rx = 2.5f;
                    Gdiplus::GraphicsPath pill;
                    AddCapsuleWindingPath(pill, rx, ry, rw, rh);
                    g.FillPath(&pillBrush, &pill);
                    g.DrawString(disp.c_str(), static_cast<INT>(disp.size()),
                                 font, GpPointF(0.0f, -th * 0.5f + 0.5f), &sf, &textBrush);
                    Gdiplus::Matrix pillTx;
                    g.GetTransform(&pillTx);
                    pillTx.GetElements(slot.pillMat);
                    slot.pillHitLocal = GpRectF(rx - rh * 0.5f, ry, rw + rh, rh);
                    slot.hasPillMat = true;
                } else {
                    g.TranslateTransform(-34.0f, 0.0f);
                    g.ScaleTransform(t, t);
                    float rx = -tw + 2.5f;
                    Gdiplus::GraphicsPath pill;
                    AddCapsuleWindingPath(pill, rx, ry, rw, rh);
                    g.FillPath(&pillBrush, &pill);
                    g.DrawString(disp.c_str(), static_cast<INT>(disp.size()),
                                 font, GpPointF(-tw, -th * 0.5f + 0.5f), &sf, &textBrush);
                    Gdiplus::Matrix pillTx;
                    g.GetTransform(&pillTx);
                    pillTx.GetElements(slot.pillMat);
                    slot.pillHitLocal = GpRectF(rx - rh * 0.5f, ry, rw + rh, rh);
                    slot.hasPillMat = true;
                }

                g.SetTransform(&pillSaved);
            }

            popStep();
        }

        g.SetTransform(&saved);
    }

    int HitTest(POINT ptClient) const {
        if (useGrid_) {
            for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
                if (PtInRect(&slots_[i].hitRect, ptClient)) return i;
            }
            return -1;
        }

        Gdiplus::PointF pt(static_cast<Gdiplus::REAL>(ptClient.x),
                           static_cast<Gdiplus::REAL>(ptClient.y));
        for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
            const auto& slot = slots_[i];
            if (slot.hasFanMat) {
                Gdiplus::Matrix mat(slot.fanMat[0], slot.fanMat[1], slot.fanMat[2],
                                   slot.fanMat[3], slot.fanMat[4], slot.fanMat[5]);
                if (mat.IsInvertible()) {
                    mat.Invert();
                    Gdiplus::PointF local = pt;
                    mat.TransformPoints(&local, 1);
                    if (local.X >= -25.0f && local.X <= 25.0f &&
                        local.Y >= -25.0f && local.Y <= 25.0f) {
                        return i;
                    }
                }
            }
            if (slot.hasPillMat) {
                Gdiplus::Matrix pmat(slot.pillMat[0], slot.pillMat[1], slot.pillMat[2],
                                    slot.pillMat[3], slot.pillMat[4], slot.pillMat[5]);
                if (pmat.IsInvertible()) {
                    pmat.Invert();
                    Gdiplus::PointF plocal = pt;
                    pmat.TransformPoints(&plocal, 1);
                    if (slot.pillHitLocal.Contains(plocal)) {
                        return i;
                    }
                }
            }
        }
        return -1;
    }

    void ActivateSlot(int slotIdx) {
        if (slotIdx < 0 || slotIdx >= static_cast<int>(slots_.size())) return;
        const auto& slot = slots_[slotIdx];
        int entryIdx = slot.itemIndex;
        StackDockletInstance* inst = inst_;
        BeginClose(true);
        if (!inst) return;
        if (entryIdx == -1 || entryIdx == -2) {
            OpenFolderTarget(inst);
        } else if (entryIdx >= 0 && entryIdx < static_cast<int>(inst->items.size())) {
            ExecuteStackEntry(inst, inst->items[entryIdx]);
        }
    }

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_APP_ANIM_STEP:
            if (pulseState_) {
                pulseState_->queued.store(false, std::memory_order_release);
                if (pulseState_->active.load(std::memory_order_acquire)) {
                    StepAnimation();
                }
            }
            return 0;

        case WM_TIMER:
            if (wParam == kAnimTimerId) {
                StepAnimation();
            } else if (wParam == kFocusWatchTimerId && !closing_ && !contextMenuOpen_) {
                if (GetForegroundWindow() != hwnd_ && (GetTickCount() - openTick_ > 180)) {
                    bool btnDown = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;
                    if (btnDown && !wasOutsideBtnDown_) {
                        POINT pt{};
                        GetCursorPos(&pt);
                        if (!PtInRect(&wndRect_, pt)) BeginClose(false);
                    }
                    wasOutsideBtnDown_ = btnDown;
                }
            }
            return 0;

        case WM_MOUSEMOVE: {
            POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            int idx = HitTest(pt);
            if (idx != hoverIdx_) {
                hoverIdx_ = idx;
                if (useGrid_) UpdateGridTooltip(hoverIdx_);
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return 0;
        }

        case WM_MOUSELEAVE:
            if (hoverIdx_ != -1) {
                hoverIdx_ = -1;
                if (useGrid_) HideGridTooltip();
            }
            return 0;

        case WM_LBUTTONUP: {
            POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            int idx = HitTest(pt);
            if (idx >= 0) {
                ActivateSlot(idx);
            } else {
                BeginClose(false);
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            int idx = HitTest(pt);
            if (idx != hoverIdx_) {
                hoverIdx_ = idx;
                if (useGrid_) UpdateGridTooltip(hoverIdx_);
            }
            ShowPopupItemContextMenu(hwnd, idx);
            return 0;
        }

        case WM_CANCELMODE:
            if (!contextMenuOpen_) BeginClose(false);
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE) {
                everActivated_ = true;
            } else if (everActivated_ && !closing_ && !contextMenuOpen_ && (GetTickCount() - openTick_ > 80)) {
                BeginClose(false);
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (useGrid_ && totalRows_ > visibleRows_) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int step = (delta > 0) ? -1 : 1;
                int nextScroll = std::clamp(scrollRow_ + step, 0, totalRows_ - visibleRows_);
                if (nextScroll != scrollRow_) {
                    scrollRow_ = nextScroll;
                    hoverIdx_ = -1;
                    HideGridTooltip();
                    BuildGridLayout(workRect_.right - workRect_.left, workRect_.bottom - workRect_.top);
                    RenderFrame();
                }
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                BeginClose(false);
                return 0;
            }
            if (wParam == VK_RETURN && hoverIdx_ >= 0) {
                ActivateSlot(hoverIdx_);
                return 0;
            }
            if (!slots_.empty()) {
                int step = 0;
                if (wParam == VK_DOWN || wParam == VK_RIGHT) step = 1;
                else if (wParam == VK_UP || wParam == VK_LEFT) step = -1;
                if (step != 0) {
                    int n = static_cast<int>(slots_.size());
                    hoverIdx_ = (hoverIdx_ < 0) ? 0 : (hoverIdx_ + step + n) % n;
                    if (useGrid_) UpdateGridTooltip(hoverIdx_);
                    return 0;
                }
            }
            break;

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    StackDockletInstance* inst_ = nullptr;
    GlobalConfig global_{};
    std::unique_ptr<Gdiplus::Font> popupFont_;
    std::shared_ptr<Gdiplus::Bitmap> openFolderBmp_;
    std::shared_ptr<Gdiplus::Bitmap> shadowBmp_;

    HDC     frameMemDC_  = nullptr;
    HBITMAP frameDib_    = nullptr;
    HGDIOBJ frameOldBmp_ = nullptr;
    void*   frameBits_   = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> gridSteadyBmp_;
    int     frameW_      = 0;
    int     frameH_      = 0;

    HWND hwnd_    = nullptr;
    HWND tipHwnd_ = nullptr;
    int tooltipSlotIdx_ = -1;
    RECT anchorRect_{};
    RECT workRect_{};
    RECT wndRect_{};
    GpRectF  gridBoxLocal_{};
    GpPointF anchorLocal_{};
    int dockEdge_ = EDGE_BOTTOM;

    bool useGrid_ = false;
    bool fanPillsOnRight_ = false;
    float fanAngleStep_ = 1.5f;
    int maxMonitorSlots_ = 16;
    int gridCols_ = 1;
    int visibleRows_ = 1;
    int totalRows_ = 1;
    int scrollRow_ = 0;

    std::vector<DisplayEntryRef> entries_;
    std::vector<PopupSlot> slots_;
    int hoverIdx_ = -1;

    void ShowPopupItemContextMenu(HWND hwnd, int slotIdx) {
        HMENU hMenu = CreatePopupMenu();
        if (!hMenu) return;

        bool hasItem = (slotIdx >= 0 && slotIdx < static_cast<int>(slots_.size()));
        int entryIdx = hasItem ? slots_[slotIdx].itemIndex : -3;
        std::wstring fullPath;
        if (entryIdx >= 0 && inst_ && entryIdx < static_cast<int>(inst_->items.size())) {
            fullPath = inst_->items[entryIdx].fullPath;
            AppendMenuW(hMenu, MF_STRING, 1, L"Open");
            SetMenuDefaultItem(hMenu, 1, FALSE);
            if (!fullPath.empty()) {
                AppendMenuW(hMenu, MF_STRING, 2, L"Open file location");
            }
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        }

        std::wstring openTxt = global_.openFolderText.empty() ? L"Open folder" : global_.openFolderText;
        AppendMenuW(hMenu, MF_STRING, 3, openTxt.c_str());
        AppendMenuW(hMenu, MF_STRING, 4, L"Stack Docklet Settings...");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, 5, L"Close stack");

        POINT spt{};
        GetCursorPos(&spt);
        contextMenuOpen_ = true;
        SetForegroundWindow(hwnd);
        UINT cmd = TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                    spt.x, spt.y, hwnd, nullptr);
        contextMenuOpen_ = false;
        DestroyMenu(hMenu);

        if (cmd == 1 && hasItem) {
            ActivateSlot(slotIdx);
            return;
        }
        StackDockletInstance* inst = inst_;
        if (cmd == 2 && !fullPath.empty()) {
            BeginClose(true);
            std::wstring args = L"/select,\"" + fullPath + L"\"";
            ShellExecuteW(inst ? inst->hwndDocklet : nullptr, L"open", L"explorer.exe",
                          args.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
        if (cmd == 3) {
            BeginClose(true);
            if (inst) OpenFolderTarget(inst);
            return;
        }
        if (cmd == 4) {
            BeginClose(true);
            if (inst && ShowStackConfigureDialog(inst)) {
                RefreshStackItems(inst);
                UpdateDockletIcon(inst);
                UpdateDockletLabelFromConfig(inst, true);
                if (HWND hRoot = inst->hwndDocklet ? GetAncestor(inst->hwndDocklet, GA_ROOTOWNER) : nullptr) {
                    PostMessageW(hRoot, WM_USER + 0x12f /* rk::WM_RK_SAVE_ITEMS */, 0, 0);
                }
            }
            return;
        }
        if (cmd == 5) {
            BeginClose(false);
        }
    }

    static VOID CALLBACK AnimQueueCallback(PVOID param, BOOLEAN) {
        auto* state = static_cast<AnimPulseState*>(param);
        if (!state || !state->active.load(std::memory_order_acquire)) return;
        if (!state->queued.exchange(true, std::memory_order_acq_rel)) {
            if (!PostMessageW(state->hwnd, WM_APP_ANIM_STEP, 0, 0)) {
                state->queued.store(false, std::memory_order_release);
            }
        }
    }

    void StepAnimation() {
        double elapsedMs = 0.0;
        if (qpcFreq_.QuadPart > 0) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            elapsedMs = static_cast<double>(now.QuadPart - animStartQpc_.QuadPart) * 1000.0 /
                        static_cast<double>(qpcFreq_.QuadPart);
        } else {
            elapsedMs = static_cast<double>(GetTickCount() - animStartTick_);
        }
        DWORD dur = useGrid_
            ? (closing_ ? kAnimDurationGridCloseMs : kAnimDurationGridOpenMs)
            : kAnimDurationFanMs;
        float frac = static_cast<float>(elapsedMs / static_cast<double>(dur));
        if (!closing_) {
            animProgress_ = (std::min)(1.0f, animStartVal_ + (1.0f - animStartVal_) * (std::max)(0.0f, frac));
            RenderFrame();
            if (animProgress_ >= 1.0f) StopAnimTimer();
        } else {
            float t = animStartVal_ * (1.0f - frac);
            if (t <= 0.0f) {
                FinishDestroy();
            } else {
                animProgress_ = t;
                RenderFrame();
            }
        }
    }

    void StartAnimTimer(float startVal) {
        StopAnimTimer();
        animStartVal_ = std::clamp(startVal, 0.0f, 1.0f);
        animStartTick_ = GetTickCount();
        QueryPerformanceCounter(&animStartQpc_);
        if (!animTimerPeriodSet_) {
            animTimerPeriodSet_ = (timeBeginPeriod(1) == TIMERR_NOERROR);
        }
        if (hwnd_ && IsWindow(hwnd_)) {
            if (!pulseState_) {
                pulseState_ = std::make_unique<AnimPulseState>();
            }
            pulseState_->hwnd = hwnd_;
            pulseState_->queued.store(false, std::memory_order_release);
            pulseState_->active.store(true, std::memory_order_release);
            if (CreateTimerQueueTimer(&animQueueTimer_, nullptr, AnimQueueCallback,
                                      pulseState_.get(), kAnimTimerIntervalMs,
                                      kAnimTimerIntervalMs, WT_EXECUTEDEFAULT)) {
                return;
            }
            pulseState_->active.store(false, std::memory_order_release);
            SetTimer(hwnd_, kAnimTimerId, kAnimTimerIntervalMs, nullptr);
        }
    }

    void StopAnimTimer() {
        if (pulseState_) {
            pulseState_->active.store(false, std::memory_order_release);
            pulseState_->queued.store(false, std::memory_order_release);
        }
        if (animQueueTimer_) {
            DeleteTimerQueueTimer(nullptr, animQueueTimer_, INVALID_HANDLE_VALUE);
            animQueueTimer_ = nullptr;
        }
        if (hwnd_ && IsWindow(hwnd_)) {
            KillTimer(hwnd_, kAnimTimerId);
        }
        if (animTimerPeriodSet_) {
            timeEndPeriod(1);
            animTimerPeriodSet_ = false;
        }
    }

    bool closing_ = false;
    bool everActivated_ = false;
    bool wasOutsideBtnDown_ = false;
    bool contextMenuOpen_ = false;
    bool animTimerPeriodSet_ = false;
    HANDLE animQueueTimer_ = nullptr;
    std::unique_ptr<AnimPulseState> pulseState_;
    DWORD openTick_ = 0;
    DWORD animStartTick_ = 0;
    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER animStartQpc_{};
    float animStartVal_  = 0.0f;
    float animProgress_  = 0.0f;
};

bool ToggleOrShowStackPopup(StackDockletInstance* inst, const POINT* cursorPt, const SIZE* dockletSize) {
    if (!inst) return false;
    if (inst->activePopup) {
        inst->activePopup->BeginClose(false);
        return true;
    }
    if (GetTickCount64() - inst->lastPopupCloseTick < 220) {
        return true;
    }
    auto* popup = new StackPopupWindow(inst);
    if (!popup->Show(cursorPt, dockletSize)) {
        delete popup;
        return false;
    }
    return true;
}

void CloseStackPopup(StackDockletInstance* inst, bool immediate) {
    if (inst && inst->activePopup) {
        inst->activePopup->BeginClose(immediate);
    }
}

namespace {

struct ConfigureContext {
    StackDockletInstance* inst = nullptr;
    std::wstring folder;
    std::wstring specialFolder;
    int folderType = FTYPE_FOLDER;
    std::wstring iconPath;
    std::wstring title;
    int mode = MODE_AUTOMATIC;
    int sortMode = SORT_NAME;
    int iconStyle = ISTYLE_WHEN_OPEN;
    bool hideOpenFolder = false;

    GlobalConfig global{};
    bool applied = false;
};

void UpdateSourceRadiosUI(HWND hDlg) {
    bool isFolder = (IsDlgButtonChecked(hDlg, IDC_RAD_FOLDER) == BST_CHECKED);
    EnableWindow(GetDlgItem(hDlg, IDC_EDIT_FOLDER), isFolder);
    EnableWindow(GetDlgItem(hDlg, IDC_BTN_BROWSE_FOLDER), isFolder);
    EnableWindow(GetDlgItem(hDlg, IDC_COMBO_SPECIAL), !isFolder);
}

bool BrowseForFolderModern(HWND hParent, const std::wstring& initial, std::wstring& outDir) {
    IFileOpenDialog* pfd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&pfd))) && pfd) {
        DWORD opts = 0;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        pfd->SetTitle(L"Select stack folder");
        if (!initial.empty()) {
            IShellItem* psiInit = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&psiInit))) && psiInit) {
                pfd->SetFolder(psiInit);
                psiInit->Release();
            }
        }
        bool ok = false;
        if (SUCCEEDED(pfd->Show(hParent))) {
            IShellItem* psiRes = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psiRes)) && psiRes) {
                PWSTR path = nullptr;
                if (SUCCEEDED(psiRes->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    outDir = path;
                    CoTaskMemFree(path);
                    ok = true;
                }
                psiRes->Release();
            }
        }
        pfd->Release();
        return ok;
    }

    BROWSEINFOW bi{};
    bi.hwndOwner = hParent;
    bi.lpszTitle = L"Select stack folder";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t buf[MAX_PATH] = {};
    bool ok = SHGetPathFromIDListW(pidl, buf) != FALSE;
    CoTaskMemFree(pidl);
    if (ok) outDir = buf;
    return ok;
}

INT_PTR CALLBACK CurrentTabProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INITDIALOG) {
        auto* psp = reinterpret_cast<PROPSHEETPAGEW*>(lParam);
        auto* ctx = reinterpret_cast<ConfigureContext*>(psp->lParam);
        SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));

        const auto& specials = GetSpecialFolderList();
        HWND hSpec = GetDlgItem(hDlg, IDC_COMBO_SPECIAL);
        int specSel = 0;
        for (int i = 0; i < static_cast<int>(specials.size()); ++i) {
            SendMessageW(hSpec, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(specials[i].name));
            if (_wcsicmp(specials[i].name, ctx->specialFolder.c_str()) == 0) specSel = i;
        }
        SendMessageW(hSpec, CB_SETCURSEL, specSel, 0);

        HWND hMode = GetDlgItem(hDlg, IDC_COMBO_MODE);
        SendMessageW(hMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic"));
        SendMessageW(hMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Fan"));
        SendMessageW(hMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Grid"));
        SendMessageW(hMode, CB_SETCURSEL, std::clamp(ctx->mode, 0, 2), 0);

        HWND hSort = GetDlgItem(hDlg, IDC_COMBO_SORT);
        SendMessageW(hSort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Name"));
        SendMessageW(hSort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Creation time"));
        SendMessageW(hSort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Last modified time"));
        SendMessageW(hSort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Last access time"));
        SendMessageW(hSort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Kind"));
        SendMessageW(hSort, CB_SETCURSEL, std::clamp(ctx->sortMode, 0, 4), 0);

        HWND hStyle = GetDlgItem(hDlg, IDC_COMBO_ICON_STYLE);
        SendMessageW(hStyle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"When stack is open"));
        SendMessageW(hStyle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Always, plus first 3 items"));
        SendMessageW(hStyle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Always"));
        SendMessageW(hStyle, CB_SETCURSEL, std::clamp(ctx->iconStyle, 0, 2), 0);

        SetDlgItemTextW(hDlg, IDC_EDIT_FOLDER, ctx->folder.c_str());
        SetDlgItemTextW(hDlg, IDC_EDIT_ICON, ctx->iconPath.c_str());
        SetDlgItemTextW(hDlg, IDC_EDIT_TITLE, ctx->title.c_str());
        CheckDlgButton(hDlg, ctx->folderType == FTYPE_SPECIAL ? IDC_RAD_SPECIAL : IDC_RAD_FOLDER, BST_CHECKED);
        CheckDlgButton(hDlg, IDC_CHK_HIDE_OPEN, ctx->hideOpenFolder ? BST_CHECKED : BST_UNCHECKED);
        UpdateSourceRadiosUI(hDlg);
        return TRUE;
    }

    auto* ctx = reinterpret_cast<ConfigureContext*>(GetWindowLongPtrW(hDlg, DWLP_USER));
    if (!ctx) return FALSE;

    switch (msg) {
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id == IDC_RAD_FOLDER || id == IDC_RAD_SPECIAL) {
            UpdateSourceRadiosUI(hDlg);
            return TRUE;
        }
        if (id == IDC_BTN_BROWSE_FOLDER) {
            wchar_t curBuf[MAX_PATH * 2] = {};
            GetDlgItemTextW(hDlg, IDC_EDIT_FOLDER, curBuf, static_cast<int>(std::size(curBuf)));
            std::wstring resolved = ResolvePathRelative(ctx->inst, curBuf);
            std::wstring picked;
            if (BrowseForFolderModern(hDlg, resolved, picked)) {
                SetDlgItemTextW(hDlg, IDC_EDIT_FOLDER, MakeRelativePathIfInsideRoot(ctx->inst, picked).c_str());
            }
            return TRUE;
        }
        if (id == IDC_BTN_BROWSE_ICON) {
            wchar_t fileBuf[MAX_PATH * 2] = {};
            wchar_t initBuf[MAX_PATH * 2] = {};
            GetDlgItemTextW(hDlg, IDC_EDIT_ICON, initBuf, static_cast<int>(std::size(initBuf)));
            std::wstring resolved = ResolvePathRelative(ctx->inst, initBuf);
            wcsncpy_s(fileBuf, resolved.c_str(), _TRUNCATE);

            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hDlg;
            ofn.lpstrFilter = L"Images (*.png;*.ico;*.bmp;*.jpg)\0*.png;*.ico;*.bmp;*.jpg;*.jpeg\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile   = fileBuf;
            ofn.nMaxFile    = static_cast<DWORD>(std::size(fileBuf));
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                SetDlgItemTextW(hDlg, IDC_EDIT_ICON, MakeRelativePathIfInsideRoot(ctx->inst, fileBuf).c_str());
            }
            return TRUE;
        }
        if (id == IDC_BTN_CLEAR_ICON) {
            SetDlgItemTextW(hDlg, IDC_EDIT_ICON, L"");
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY: {
        auto* nmh = reinterpret_cast<NMHDR*>(lParam);
        if (nmh && nmh->code == PSN_APPLY) {
            wchar_t buf[MAX_PATH * 2] = {};
            ctx->folderType = (IsDlgButtonChecked(hDlg, IDC_RAD_SPECIAL) == BST_CHECKED) ? FTYPE_SPECIAL : FTYPE_FOLDER;
            GetDlgItemTextW(hDlg, IDC_EDIT_FOLDER, buf, static_cast<int>(std::size(buf)));
            ctx->folder = buf;

            LRESULT sIdx = SendDlgItemMessageW(hDlg, IDC_COMBO_SPECIAL, CB_GETCURSEL, 0, 0);
            const auto& specials = GetSpecialFolderList();
            if (sIdx >= 0 && sIdx < static_cast<LRESULT>(specials.size())) {
                ctx->specialFolder = specials[static_cast<size_t>(sIdx)].name;
            }

            GetDlgItemTextW(hDlg, IDC_EDIT_ICON, buf, static_cast<int>(std::size(buf)));
            ctx->iconPath = buf;
            GetDlgItemTextW(hDlg, IDC_EDIT_TITLE, buf, static_cast<int>(std::size(buf)));
            ctx->title = buf;

            ctx->mode      = static_cast<int>(SendDlgItemMessageW(hDlg, IDC_COMBO_MODE, CB_GETCURSEL, 0, 0));
            ctx->sortMode  = static_cast<int>(SendDlgItemMessageW(hDlg, IDC_COMBO_SORT, CB_GETCURSEL, 0, 0));
            ctx->iconStyle = static_cast<int>(SendDlgItemMessageW(hDlg, IDC_COMBO_ICON_STYLE, CB_GETCURSEL, 0, 0));
            ctx->hideOpenFolder = (IsDlgButtonChecked(hDlg, IDC_CHK_HIDE_OPEN) == BST_CHECKED);
            ctx->applied = true;
            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }

    default:
        break;
    }
    return FALSE;
}

void UpdateFontDescText(HWND hDlg, const GlobalConfig& g) {
    std::wstring desc = g.fontName + L", " + std::to_wstring(g.fontSize) + L" pt";
    if (g.fontBold) desc += L", Bold";
    SetDlgItemTextW(hDlg, IDC_EDIT_FONT_DESC, desc.c_str());
}

INT_PTR CALLBACK GlobalTabProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INITDIALOG) {
        auto* psp = reinterpret_cast<PROPSHEETPAGEW*>(lParam);
        auto* ctx = reinterpret_cast<ConfigureContext*>(psp->lParam);
        SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));

        SetDlgItemTextW(hDlg, IDC_EDIT_OPEN_TEXT, ctx->global.openFolderText.c_str());
        SetDlgItemTextW(hDlg, IDC_EDIT_MORE_TEXT, ctx->global.dMoreText.c_str());
        UpdateFontDescText(hDlg, ctx->global);
        CheckDlgButton(hDlg, IDC_CHK_FOLLOW_DOCK, ctx->global.followDock ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }

    auto* ctx = reinterpret_cast<ConfigureContext*>(GetWindowLongPtrW(hDlg, DWLP_USER));
    if (!ctx) return FALSE;

    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_CHOOSE_FONT) {
            LOGFONTW lf{};
            wcsncpy_s(lf.lfFaceName, ctx->global.fontName.c_str(), _TRUNCATE);
            HDC hdc = GetDC(hDlg);
            lf.lfHeight = -MulDiv(ctx->global.fontSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
            ReleaseDC(hDlg, hdc);
            lf.lfWeight = ctx->global.fontBold ? FW_BOLD : FW_NORMAL;

            CHOOSEFONTW cf{};
            cf.lStructSize = sizeof(cf);
            cf.hwndOwner   = hDlg;
            cf.lpLogFont   = &lf;
            cf.Flags       = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;
            if (ChooseFontW(&cf)) {
                ctx->global.fontName = lf.lfFaceName;
                ctx->global.fontSize = std::clamp(cf.iPointSize / 10, 6, 36);
                ctx->global.fontBold = (lf.lfWeight >= FW_BOLD);
                UpdateFontDescText(hDlg, ctx->global);
            }
            return TRUE;
        }
        break;

    case WM_NOTIFY: {
        auto* nmh = reinterpret_cast<NMHDR*>(lParam);
        if (nmh && nmh->code == PSN_APPLY) {
            wchar_t buf[256] = {};
            GetDlgItemTextW(hDlg, IDC_EDIT_OPEN_TEXT, buf, static_cast<int>(std::size(buf)));
            ctx->global.openFolderText = *buf ? buf : L"Open folder";
            GetDlgItemTextW(hDlg, IDC_EDIT_MORE_TEXT, buf, static_cast<int>(std::size(buf)));
            ctx->global.dMoreText = *buf ? buf : L"%d more...";
            ctx->global.followDock = (IsDlgButtonChecked(hDlg, IDC_CHK_FOLLOW_DOCK) == BST_CHECKED);
            ctx->applied = true;
            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }

    default:
        break;
    }
    return FALSE;
}

} // namespace

bool ShowStackConfigureDialog(StackDockletInstance* inst) {
    if (!inst) return false;
    ScopedCom com;

    ConfigureContext ctx;
    ctx.inst = inst;
    ctx.folderType = inst->folderType;
    if (inst->folderType == FTYPE_SPECIAL) {
        ctx.specialFolder = inst->folder;
    } else {
        ctx.folder = inst->folder;
        ctx.specialFolder = L"Desktop";
    }
    ctx.iconPath       = inst->iconPath;
    ctx.title          = inst->title;
    ctx.mode           = inst->mode;
    ctx.sortMode       = inst->sortMode;
    ctx.iconStyle      = inst->iconStyle;
    ctx.hideOpenFolder = inst->hideOpenFolder;
    LoadGlobalConfig(ctx.global);

    PROPSHEETPAGEW pages[2]{};
    pages[0].dwSize      = sizeof(PROPSHEETPAGEW);
    pages[0].dwFlags     = PSP_DEFAULT | PSP_USETITLE;
    pages[0].hInstance   = PluginModule();
    pages[0].pszTemplate = MAKEINTRESOURCEW(IDD_STACK_CURRENT);
    pages[0].pszTitle    = L"Current docklet";
    pages[0].pfnDlgProc  = CurrentTabProc;
    pages[0].lParam      = reinterpret_cast<LPARAM>(&ctx);

    pages[1].dwSize      = sizeof(PROPSHEETPAGEW);
    pages[1].dwFlags     = PSP_DEFAULT | PSP_USETITLE;
    pages[1].hInstance   = PluginModule();
    pages[1].pszTemplate = MAKEINTRESOURCEW(IDD_STACK_GLOBAL);
    pages[1].pszTitle    = L"Global settings";
    pages[1].pfnDlgProc  = GlobalTabProc;
    pages[1].lParam      = reinterpret_cast<LPARAM>(&ctx);

    PROPSHEETHEADERW psh{};
    psh.dwSize     = sizeof(PROPSHEETHEADERW);
    psh.dwFlags    = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP;
    psh.hwndParent = inst->hwndDocklet ? GetAncestor(inst->hwndDocklet, GA_ROOTOWNER) : nullptr;
    psh.hInstance  = PluginModule();
    psh.pszCaption = L"Stack Docklet Settings";
    psh.nPages     = 2;
    psh.ppsp       = pages;

    INT_PTR res = PropertySheetW(&psh);
    if (res > 0 && ctx.applied) {
        inst->folderType     = std::clamp(ctx.folderType, 0, 1);
        inst->folder         = (inst->folderType == FTYPE_SPECIAL) ? ctx.specialFolder : ctx.folder;
        inst->iconPath       = ctx.iconPath;
        inst->title          = ctx.title;
        inst->mode           = std::clamp(ctx.mode, 0, 2);
        inst->sortMode       = std::clamp(ctx.sortMode, 0, 4);
        inst->iconStyle      = std::clamp(ctx.iconStyle, 0, 2);
        inst->hideOpenFolder = ctx.hideOpenFolder;
        inst->cachedBaseIconAttempted = false;
        inst->hasCompositedIcon = false;
        SaveGlobalConfig(ctx.global);
        return true;
    }
    return false;
}

} // namespace stackdock
