#include "rk_theme.h"
#include "rk_common.h"
#include "rk_ini.h"
#include "rk_options.h"
#include <algorithm>
#include <cmath>
#include <shlwapi.h>

namespace rk {

RKDockTheme::~RKDockTheme() {
    Unload();
}

void RKDockTheme::InvalidateRenderCaches() const {
    cacheBg_.rotKey = static_cast<Gdiplus::RotateFlipType>(-1);
    cacheBg_.bmp.reset();
    cacheSep_.rotKey = static_cast<Gdiplus::RotateFlipType>(-1);
    cacheSep_.bmp.reset();
    cacheInd_.rotKey = static_cast<Gdiplus::RotateFlipType>(-1);
    cacheInd_.bmp.reset();
    cachedBgBmp_.reset();
    cachedBgW_ = cachedBgH_ = 0;
    cachedBgEdge_ = cachedBgAlpha_ = cachedBgInterp_ = -1;
}

Gdiplus::Bitmap* RKDockTheme::GetOrientedBitmap(const std::unique_ptr<Gdiplus::Bitmap>& src,
                                                Gdiplus::RotateFlipType rot,
                                                OrientedBitmapCache& cache) {
    if (!src) return nullptr;
    if (cache.bmp && cache.rotKey == rot) {
        return cache.bmp.get();
    }
    std::unique_ptr<Gdiplus::Bitmap> oriented(src->Clone(
        0, 0, src->GetWidth(), src->GetHeight(), PixelFormat32bppPARGB));
    if (!oriented) return nullptr;
    if (rot != Gdiplus::RotateNoneFlipNone) {
        oriented->RotateFlip(rot);
    }
    cache.rotKey = rot;
    cache.bmp = std::move(oriented);
    return cache.bmp.get();
}

void RKDockTheme::Unload() {
    InvalidateRenderCaches();
    bmpBg_.reset();
    bmpSep_.reset();
    bmpInd_.reset();
    bmpPoof_.reset();
    margins_ = ThemeMargins{};
    fillMode_ = FillMode::Stretch;
    builtinFallback_ = true;
    poofFrames_ = 12;
}

std::unique_ptr<Gdiplus::Bitmap> RKDockTheme::LoadBitmapFile(const std::wstring& path) {
    if (path.empty() || !FileExists(path)) return nullptr;
    auto bmp = std::make_unique<Gdiplus::Bitmap>(path.c_str(), FALSE);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok || bmp->GetWidth() == 0 || bmp->GetHeight() == 0) {
        return nullptr;
    }
    return bmp;
}

bool RKDockTheme::Load(const std::wstring& themeName) {
    Unload();
    themeName_ = themeName.empty() ? L"Default" : themeName;

    std::wstring baseDir = ResolveAppPath(L"themes\\" + themeName_);
    std::wstring confPath = baseDir + L"\\theme.conf";
    std::wstring yzIniPath = baseDir + L"\\background.ini";

    if (!FileExists(confPath) && FileExists(yzIniPath)) {
        return LoadFromYzIni(yzIniPath);
    }

    IniFile ini;
    if (!ini.Load(confPath)) {
        // Check if built-in glass theme should handle it seamlessly
        builtinFallback_ = true;
        return false;
    }

    margins_.outsideLeft   = ini.GetInt(L"Background", L"outsideMarginLeft", 12);
    margins_.outsideRight  = ini.GetInt(L"Background", L"outsideMarginRight", 12);
    margins_.outsideTop    = ini.GetInt(L"Background", L"outsideMarginTop", 12);
    margins_.outsideBottom = ini.GetInt(L"Background", L"outsideMarginBottom", 4);

    margins_.borderLeft    = ini.GetInt(L"Background", L"borderMarginLeft", 8);
    margins_.borderRight   = ini.GetInt(L"Background", L"borderMarginRight", 8);
    margins_.borderTop     = ini.GetInt(L"Background", L"borderMarginTop", 8);
    margins_.borderBottom  = ini.GetInt(L"Background", L"borderMarginBottom", 8);
    margins_.margin        = ini.GetInt(L"Background", L"margin", 5);
    margins_.useShadowMargins = true;

    std::wstring modeStr = ToLower(ini.GetString(L"Background", L"fillMode", L"stretch"));
    fillMode_ = (modeStr == L"tile") ? FillMode::Tile : FillMode::Stretch;

    margins_.sepMarginTop    = ini.GetInt(L"Separator", L"marginTop", 4);
    margins_.sepMarginBottom = ini.GetInt(L"Separator", L"marginBottom", 4);

    auto resolveThemeImg = [&](const std::wstring& rel) -> std::wstring {
        if (rel.empty()) return L"";
        std::wstring s = NormalizeSlashes(rel, L'\\');
        if (!PathIsRelativeW(s.c_str())) return s;
        if (FileExists(baseDir + L"\\" + s)) return baseDir + L"\\" + s;
        return ResolveAppPath(s);
    };

    bmpBg_   = LoadBitmapFile(resolveThemeImg(ini.GetString(L"Background", L"image")));
    bmpSep_  = LoadBitmapFile(resolveThemeImg(ini.GetString(L"Separator", L"image")));
    bmpInd_  = LoadBitmapFile(resolveThemeImg(ini.GetString(L"Indicator", L"image")));
    bmpPoof_ = LoadBitmapFile(resolveThemeImg(ini.GetString(L"Poof", L"image")));

    if (bmpPoof_ && bmpPoof_->GetWidth() > 0) {
        int fw = static_cast<int>(bmpPoof_->GetWidth());
        int fh = static_cast<int>(bmpPoof_->GetHeight());
        poofFrames_ = std::max(1, fh / fw);
    }

    builtinFallback_ = (bmpBg_ == nullptr);
    return true;
}

bool RKDockTheme::LoadFromYzIni(const std::wstring& iniPath, const std::wstring& /*saveAsThemeName*/) {
    InvalidateRenderCaches();
    IniFile ini;
    if (!ini.Load(iniPath)) return false;

    wchar_t dirBuf[MAX_PATH] = {};
    wcsncpy_s(dirBuf, iniPath.c_str(), _TRUNCATE);
    PathRemoveFileSpecW(dirBuf);
    std::wstring baseDir = dirBuf;

    margins_.outsideLeft   = ini.GetInt(L"Background", L"OutsideLeftMargin", ini.GetInt(L"Background", L"LeftMargin", 12));
    margins_.outsideRight  = ini.GetInt(L"Background", L"OutsideRightMargin", ini.GetInt(L"Background", L"RightMargin", 12));
    margins_.outsideTop    = ini.GetInt(L"Background", L"OutsideTopMargin", ini.GetInt(L"Background", L"TopMargin", 10));
    margins_.outsideBottom = ini.GetInt(L"Background", L"OutsideBottomMargin", ini.GetInt(L"Background", L"BottomMargin", 4));
    margins_.borderLeft    = ini.GetInt(L"Background", L"LeftMargin", 8);
    margins_.borderRight   = ini.GetInt(L"Background", L"RightMargin", 8);
    margins_.borderTop     = ini.GetInt(L"Background", L"TopMargin", 8);
    margins_.borderBottom  = ini.GetInt(L"Background", L"BottomMargin", 8);
    margins_.margin        = 0;
    margins_.useShadowMargins = false;

    int tile = ini.GetInt(L"Background", L"TileMode", 0);
    fillMode_ = (tile != 0) ? FillMode::Tile : FillMode::Stretch;

    std::wstring imgRel = NormalizeSlashes(ini.GetString(L"Background", L"Image", L"bg.png"), L'\\');
    bmpBg_ = LoadBitmapFile(PathIsRelativeW(imgRel.c_str()) ? (baseDir + L"\\" + imgRel) : imgRel);
    std::wstring sepRel = NormalizeSlashes(ini.GetString(L"Separator", L"Image", L""), L'\\');
    if (!sepRel.empty()) {
        bmpSep_ = LoadBitmapFile(PathIsRelativeW(sepRel.c_str()) ? (baseDir + L"\\" + sepRel) : sepRel);
    }
    builtinFallback_ = (bmpBg_ == nullptr);
    return !builtinFallback_;
}

ThemeMargins RKDockTheme::GetEffectiveMargins(int position) const {
    ThemeMargins m = margins_;
    // Bottom is the canonical base orientation. Rotate margins for other edges.
    if (IsTopEdge(position)) {
        std::swap(m.outsideTop, m.outsideBottom);
        std::swap(m.borderTop, m.borderBottom);
        std::swap(m.sepMarginTop, m.sepMarginBottom);
    } else if (IsLeftEdge(position)) {
        m.outsideLeft   = margins_.outsideBottom;
        m.outsideRight  = margins_.outsideTop;
        m.outsideTop    = margins_.outsideLeft;
        m.outsideBottom = margins_.outsideRight;
        m.borderLeft    = margins_.borderBottom;
        m.borderRight   = margins_.borderTop;
        m.borderTop     = margins_.borderLeft;
        m.borderBottom  = margins_.borderRight;
    } else if (IsRightEdge(position)) {
        m.outsideLeft   = margins_.outsideTop;
        m.outsideRight  = margins_.outsideBottom;
        m.outsideTop    = margins_.outsideRight;
        m.outsideBottom = margins_.outsideLeft;
        m.borderLeft    = margins_.borderTop;
        m.borderRight   = margins_.borderBottom;
        m.borderTop     = margins_.borderRight;
        m.borderBottom  = margins_.borderLeft;
    }
    return m;
}

static Gdiplus::RotateFlipType EdgeRotateType(int position) {
    if (IsTopEdge(position))   return Gdiplus::Rotate180FlipNone;
    if (IsLeftEdge(position))  return Gdiplus::Rotate90FlipNone;
    if (IsRightEdge(position)) return Gdiplus::Rotate270FlipNone;
    return Gdiplus::RotateNoneFlipNone;
}

void RKDockTheme::DrawNinePatch(Gdiplus::Graphics& g, Gdiplus::Bitmap* bmp, const Gdiplus::Rect& dst,
                                int bL, int bT, int bR, int bB, FillMode mode, float alphaMul) {
    if (!bmp || dst.Width <= 0 || dst.Height <= 0) return;

    int sw = static_cast<int>(bmp->GetWidth());
    int sh = static_cast<int>(bmp->GetHeight());
    if (sw <= 0 || sh <= 0) return;

    bL = std::clamp(bL, 0, sw / 2);
    bR = std::clamp(bR, 0, sw - bL);
    bT = std::clamp(bT, 0, sh / 2);
    bB = std::clamp(bB, 0, sh - bT);

    int dbL = std::min(bL, dst.Width / 2);
    int dbR = std::min(bR, dst.Width - dbL);
    int dbT = std::min(bT, dst.Height / 2);
    int dbB = std::min(bB, dst.Height - dbT);

    Gdiplus::ImageAttributes attr;
    Gdiplus::ColorMatrix cm = {
        1, 0, 0, 0, 0,
        0, 1, 0, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 0, alphaMul, 0,
        0, 0, 0, 0, 1
    };
    attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
    attr.SetWrapMode(Gdiplus::WrapModeTileFlipXY);

    int sx[4] = { 0, bL, sw - bR, sw };
    int sy[4] = { 0, bT, sh - bB, sh };
    int dx[4] = { dst.X, dst.X + dbL, dst.X + dst.Width - dbR, dst.X + dst.Width };
    int dy[4] = { dst.Y, dst.Y + dbT, dst.Y + dst.Height - dbB, dst.Y + dst.Height };

    for (int row = 0; row < 3; ++row) {
        int sH = sy[row + 1] - sy[row];
        int dH = dy[row + 1] - dy[row];
        if (sH <= 0 || dH <= 0) continue;
        for (int col = 0; col < 3; ++col) {
            int sW = sx[col + 1] - sx[col];
            int dW = dx[col + 1] - dx[col];
            if (sW <= 0 || dW <= 0) continue;

            if (mode == FillMode::Tile && (row == 1 || col == 1)) {
                for (int y = dy[row]; y < dy[row + 1]; y += sH) {
                    int ch = std::min(sH, dy[row + 1] - y);
                    for (int x = dx[col]; x < dx[col + 1]; x += sW) {
                        int cw = std::min(sW, dx[col + 1] - x);
                        Gdiplus::Rect cell(x, y, cw, ch);
                        g.DrawImage(bmp, cell, sx[col], sy[row], cw, ch, Gdiplus::UnitPixel, &attr);
                    }
                }
            } else {
                Gdiplus::Rect cell(dx[col], dy[row], dW, dH);
                g.DrawImage(bmp, cell, sx[col], sy[row], sW, sH, Gdiplus::UnitPixel, &attr);
            }
        }
    }
}

static void AddRoundedRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    float d = radius * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    if (d <= 0.5f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void RKDockTheme::DrawBackground(Gdiplus::Graphics& g, const Gdiplus::Rect& dstRect, const Gdiplus::Rect& pillRect,
                                 int position, int alpha0to255) const {
    if (dstRect.Width <= 0 || dstRect.Height <= 0 || alpha0to255 <= 0) return;

    const int clampedAlpha = std::clamp(alpha0to255, 0, 255);
    const int edgeGroup = IsTopEdge(position) ? 1 : (IsLeftEdge(position) ? 2 : (IsRightEdge(position) ? 3 : 0));
    const int relPillX = pillRect.X - dstRect.X;
    const int relPillY = pillRect.Y - dstRect.Y;
    const int interpKey = static_cast<int>(g.GetInterpolationMode());

    auto blitCached = [&]() {
        auto oldInterp = g.GetInterpolationMode();
        auto oldPx     = g.GetPixelOffsetMode();
        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);
        g.DrawImage(cachedBgBmp_.get(), dstRect.X, dstRect.Y, dstRect.Width, dstRect.Height);
        g.SetPixelOffsetMode(oldPx);
        g.SetInterpolationMode(oldInterp);
    };

    if (cachedBgBmp_ &&
        cachedBgW_ == dstRect.Width && cachedBgH_ == dstRect.Height &&
        cachedPillRelX_ == relPillX && cachedPillRelY_ == relPillY &&
        cachedPillW_ == pillRect.Width && cachedPillH_ == pillRect.Height &&
        cachedBgEdge_ == edgeGroup && cachedBgAlpha_ == clampedAlpha &&
        cachedBgInterp_ == interpKey) {
        blitCached();
        return;
    }

    auto rendered = std::make_unique<Gdiplus::Bitmap>(dstRect.Width, dstRect.Height, PixelFormat32bppPARGB);
    const Gdiplus::Rect localDst(0, 0, dstRect.Width, dstRect.Height);
    const Gdiplus::Rect localPill(relPillX, relPillY, pillRect.Width, pillRect.Height);
    const float alphaMul = clampedAlpha / 255.0f;

    {
        Gdiplus::Graphics gc(rendered.get());
        gc.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        gc.SetInterpolationMode(g.GetInterpolationMode());
        gc.Clear(Gdiplus::Color(0, 0, 0, 0));

        if (Gdiplus::Bitmap* oriented = GetOrientedBitmap(bmpBg_, EdgeRotateType(position), cacheBg_)) {
            ThemeMargins eff = GetEffectiveMargins(position);
            if (eff.useShadowMargins) {
                Gdiplus::Rect drawPill = (localPill.Width > 0 && localPill.Height > 0) ? localPill : localDst;
                Gdiplus::Rect fullRect(
                    drawPill.X - eff.outsideLeft,
                    drawPill.Y - eff.outsideTop,
                    drawPill.Width + eff.outsideLeft + eff.outsideRight,
                    drawPill.Height + eff.outsideTop + eff.outsideBottom);
                Gdiplus::GraphicsState st = gc.Save();
                gc.SetClip(localDst);
                DrawNinePatch(gc, oriented, fullRect,
                              eff.outsideLeft + eff.borderLeft,
                              eff.outsideTop + eff.borderTop,
                              eff.outsideRight + eff.borderRight,
                              eff.outsideBottom + eff.borderBottom,
                              fillMode_, alphaMul);
                gc.Restore(st);
            } else {
                DrawNinePatch(gc, oriented, localDst,
                              eff.borderLeft, eff.borderTop, eff.borderRight, eff.borderBottom,
                              fillMode_, alphaMul);
            }
        } else {
            // Built-in frosted glass dock pill fallback renders strictly into the visible pill bounds
            const Gdiplus::Rect& fb = (localPill.Width > 0 && localPill.Height > 0) ? localPill : localDst;
            Gdiplus::RectF rf(static_cast<float>(fb.X), static_cast<float>(fb.Y),
                              static_cast<float>(fb.Width), static_cast<float>(fb.Height));
            float radius = 14.0f;

            Gdiplus::GraphicsPath sp;
            Gdiplus::RectF sr = rf;
            sr.Inflate(-0.5f, -0.5f);
            AddRoundedRectPath(sp, sr, radius);
            BYTE a1 = static_cast<BYTE>(std::clamp<int>(static_cast<int>(215 * alphaMul), 12, 235));
            BYTE a2 = static_cast<BYTE>(std::clamp<int>(static_cast<int>(165 * alphaMul), 10, 210));
            Gdiplus::LinearGradientBrush bgBrush(
                Gdiplus::PointF(rf.X, rf.Y),
                Gdiplus::PointF(IsHorizontalPos(position) ? rf.X : rf.X + rf.Width,
                                IsHorizontalPos(position) ? rf.Y + rf.Height : rf.Y),
                Gdiplus::Color(a1, 245, 248, 255),
                Gdiplus::Color(a2, 210, 222, 238));
            gc.FillPath(&bgBrush, &sp);

            // Specular top glass highlight band
            Gdiplus::GraphicsState st = gc.Save();
            gc.SetClip(&sp);
            Gdiplus::RectF hl = rf;
            if (IsHorizontalPos(position)) hl.Height *= 0.45f;
            else hl.Width *= 0.45f;
            BYTE ha = static_cast<BYTE>(std::clamp<int>(static_cast<int>(90 * alphaMul), 4, 110));
            Gdiplus::SolidBrush hlBrush(Gdiplus::Color(ha, 255, 255, 255));
            gc.FillRectangle(&hlBrush, hl);
            gc.Restore(st);

            // Outer border and crisp inner edge
            BYTE ba = static_cast<BYTE>(std::clamp<int>(static_cast<int>(160 * alphaMul), 16, 180));
            Gdiplus::Pen borderPen(Gdiplus::Color(ba, 90, 104, 128), 1.0f);
            gc.DrawPath(&borderPen, &sp);

            Gdiplus::GraphicsPath ip;
            Gdiplus::RectF ir = sr;
            ir.Inflate(-1.0f, -1.0f);
            AddRoundedRectPath(ip, ir, std::max(2.0f, radius - 1.0f));
            BYTE ia = static_cast<BYTE>(std::clamp<int>(static_cast<int>(190 * alphaMul), 12, 210));
            Gdiplus::Pen innerPen(Gdiplus::Color(ia, 255, 255, 255), 1.0f);
            gc.DrawPath(&innerPen, &ip);
        }
    }

    cachedBgBmp_    = std::move(rendered);
    cachedBgW_      = dstRect.Width;
    cachedBgH_      = dstRect.Height;
    cachedPillRelX_ = relPillX;
    cachedPillRelY_ = relPillY;
    cachedPillW_    = pillRect.Width;
    cachedPillH_    = pillRect.Height;
    cachedBgEdge_   = edgeGroup;
    cachedBgAlpha_  = clampedAlpha;
    cachedBgInterp_ = interpKey;
    blitCached();
}

void RKDockTheme::DrawSeparator(Gdiplus::Graphics& g, const Gdiplus::Rect& dstRect, int position,
                                int alpha0to255) const {
    if (dstRect.Width <= 0 || dstRect.Height <= 0) return;

    const float sepAlphaMul = (alpha0to255 <= 0) ? 0.85f : (std::clamp(alpha0to255, 0, 255) / 255.0f);
    const bool horiz = IsHorizontalPos(position);
    if (Gdiplus::Bitmap* oriented = GetOrientedBitmap(bmpSep_, EdgeRotateType(position), cacheSep_)) {
        const int sw = static_cast<int>(oriented->GetWidth());
        const int sh = static_cast<int>(oriented->GetHeight());
        const ThemeMargins eff = GetEffectiveMargins(position);

        Gdiplus::ImageAttributes attr;
        Gdiplus::ColorMatrix cm = {
            1, 0, 0, 0, 0,
            0, 1, 0, 0, 0,
            0, 0, 1, 0, 0,
            0, 0, 0, sepAlphaMul, 0,
            0, 0, 0, 0, 1
        };
        attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
        attr.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        Gdiplus::GraphicsState st = g.Save();
        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        if (horiz) {
            const int drawW = std::min(sw, dstRect.Width);
            const int drawX = dstRect.X + (dstRect.Width - drawW) / 2;
            const int capT  = std::clamp(eff.sepMarginTop, 0, sh / 2);
            const int capB  = std::clamp(eff.sepMarginBottom, 0, sh - capT);
            const int dCapT = std::min(capT, dstRect.Height / 2);
            const int dCapB = std::min(capB, dstRect.Height - dCapT);
            const int sy[4] = { 0, capT, sh - capB, sh };
            const int dy[4] = { dstRect.Y, dstRect.Y + dCapT, dstRect.Y + dstRect.Height - dCapB, dstRect.Y + dstRect.Height };
            for (int row = 0; row < 3; ++row) {
                int sH = sy[row + 1] - sy[row];
                int dH = dy[row + 1] - dy[row];
                if (sH <= 0 || dH <= 0) continue;
                g.DrawImage(oriented, Gdiplus::Rect(drawX, dy[row], drawW, dH),
                            0, sy[row], sw, sH, Gdiplus::UnitPixel, &attr);
            }
        } else {
            const int drawH = std::min(sh, dstRect.Height);
            const int drawY = dstRect.Y + (dstRect.Height - drawH) / 2;
            const int leadSrcCap  = IsLeftEdge(position) ? margins_.sepMarginBottom : margins_.sepMarginTop;
            const int trailSrcCap = IsLeftEdge(position) ? margins_.sepMarginTop : margins_.sepMarginBottom;
            const int capL  = std::clamp(leadSrcCap, 0, sw / 2);
            const int capR  = std::clamp(trailSrcCap, 0, sw - capL);
            const int dCapL = std::min(capL, dstRect.Width / 2);
            const int dCapR = std::min(capR, dstRect.Width - dCapL);
            const int sx[4] = { 0, capL, sw - capR, sw };
            const int dx[4] = { dstRect.X, dstRect.X + dCapL, dstRect.X + dstRect.Width - dCapR, dstRect.X + dstRect.Width };
            for (int col = 0; col < 3; ++col) {
                int sW = sx[col + 1] - sx[col];
                int dW = dx[col + 1] - dx[col];
                if (sW <= 0 || dW <= 0) continue;
                g.DrawImage(oriented, Gdiplus::Rect(dx[col], drawY, dW, drawH),
                            sx[col], 0, sW, sh, Gdiplus::UnitPixel, &attr);
            }
        }
        g.Restore(st);
        return;
    }

    // Built-in crisp hairline separator fallback
    Gdiplus::GraphicsState st = g.Save();
    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    BYTE da = static_cast<BYTE>(std::clamp(static_cast<int>(160 * sepAlphaMul), 24, 200));
    BYTE la = static_cast<BYTE>(std::clamp(static_cast<int>(190 * sepAlphaMul), 32, 220));
    Gdiplus::Pen dark(Gdiplus::Color(da, 45, 58, 78), 1.0f);
    Gdiplus::Pen light(Gdiplus::Color(la, 255, 255, 255), 1.0f);
    if (horiz) {
        int cx = dstRect.X + dstRect.Width / 2;
        int y1 = dstRect.Y + 2;
        int y2 = dstRect.Y + dstRect.Height - 2;
        if (y2 > y1) {
            g.DrawLine(&dark,  cx - 1, y1, cx - 1, y2);
            g.DrawLine(&light, cx,     y1, cx,     y2);
        }
    } else {
        int cy = dstRect.Y + dstRect.Height / 2;
        int x1 = dstRect.X + 2;
        int x2 = dstRect.X + dstRect.Width - 2;
        if (x2 > x1) {
            g.DrawLine(&dark,  x1, cy - 1, x2, cy - 1);
            g.DrawLine(&light, x1, cy,     x2, cy);
        }
    }
    g.Restore(st);
}

void RKDockTheme::DrawIndicator(Gdiplus::Graphics& g, const Gdiplus::Rect& itemRect, const Gdiplus::Rect& pillRect,
                                int position) const {
    if (itemRect.Width <= 0 || itemRect.Height <= 0) return;

    if (Gdiplus::Bitmap* oriented = GetOrientedBitmap(bmpInd_, EdgeRotateType(position), cacheInd_)) {
        int iw = static_cast<int>(oriented->GetWidth());
        int ih = static_cast<int>(oriented->GetHeight());
        int x = itemRect.X + (itemRect.Width - iw) / 2;
        int y = itemRect.Y + (itemRect.Height - ih) / 2;
        if (IsBottomEdge(position)) {
            y = (pillRect.Height > 0) ? std::max(itemRect.Y + itemRect.Height - ih / 2, pillRect.Y + pillRect.Height - ih - 1)
                                      : (itemRect.Y + itemRect.Height - ih / 2);
        } else if (IsTopEdge(position)) {
            int minTop = (pillRect.Height > 0) ? std::max(0, pillRect.Y + 1) : 0;
            y = std::max(minTop, itemRect.Y - ih);
        } else if (IsLeftEdge(position)) {
            int minLeft = (pillRect.Width > 0) ? std::max(0, pillRect.X + 1) : 0;
            x = std::max(minLeft, itemRect.X - iw);
        } else if (IsRightEdge(position)) {
            x = (pillRect.Width > 0) ? std::max(itemRect.X + itemRect.Width - iw / 2, pillRect.X + pillRect.Width - iw - 1)
                                     : (itemRect.X + itemRect.Width - iw / 2);
        }
        g.DrawImage(oriented, Gdiplus::Rect(x, y, iw, ih));
        return;
    }

    // Built-in glowing dot indicator
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = itemRect.X + itemRect.Width * 0.5f;
    float cy = itemRect.Y + itemRect.Height * 0.5f;
    if (IsBottomEdge(position))     cy = static_cast<float>(itemRect.Y + itemRect.Height + 3);
    else if (IsTopEdge(position))   cy = std::max(3.5f, static_cast<float>(itemRect.Y - 3));
    else if (IsLeftEdge(position))  cx = std::max(3.5f, static_cast<float>(itemRect.X - 3));
    else if (IsRightEdge(position)) cx = static_cast<float>(itemRect.X + itemRect.Width + 3);

    Gdiplus::SolidBrush glow(Gdiplus::Color(95, 70, 165, 255));
    Gdiplus::SolidBrush core(Gdiplus::Color(235, 35, 130, 245));
    g.FillEllipse(&glow, cx - 5.0f, cy - 5.0f, 10.0f, 10.0f);
    g.FillEllipse(&core, cx - 2.6f, cy - 2.6f, 5.2f, 5.2f);
}

int RKDockTheme::GetPoofFrameCount() const {
    return poofFrames_;
}

void RKDockTheme::DrawPoofFrame(Gdiplus::Graphics& g, const Gdiplus::Rect& dstRect, int frameIndex) const {
    if (frameIndex < 0 || frameIndex >= poofFrames_) return;
    if (bmpPoof_) {
        int fw = static_cast<int>(bmpPoof_->GetWidth());
        int fh = fw;
        int sy = frameIndex * fh;
        if (sy + fh <= static_cast<int>(bmpPoof_->GetHeight())) {
            g.DrawImage(bmpPoof_.get(), dstRect, 0, sy, fw, fh, Gdiplus::UnitPixel);
            return;
        }
    }

    // Built-in soft radial cloud puff sequence
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float t = static_cast<float>(frameIndex) / std::max(1, poofFrames_ - 1);
    float alpha = (t < 0.3f) ? (t / 0.3f) : (1.0f - (t - 0.3f) / 0.7f);
    int a = static_cast<int>(std::clamp(alpha * 220.0f, 0.0f, 220.0f));
    float cx = dstRect.X + dstRect.Width * 0.5f;
    float cy = dstRect.Y + dstRect.Height * 0.5f;
    float baseR = std::min(dstRect.Width, dstRect.Height) * (0.25f + 0.25f * t);

    for (int k = 0; k < 6; ++k) {
        double ang = k * 1.0471975 + t * 1.4;
        float dist = baseR * (0.3f + 0.8f * t);
        float px = cx + static_cast<float>(std::cos(ang) * dist);
        float py = cy + static_cast<float>(std::sin(ang) * dist);
        float pr = baseR * (0.65f - 0.25f * t);
        Gdiplus::SolidBrush br(Gdiplus::Color(static_cast<BYTE>(a / 2), 240, 244, 250));
        g.FillEllipse(&br, px - pr, py - pr, pr * 2.0f, pr * 2.0f);
    }
}

void RKDockTheme::RenderStyledLabel(Gdiplus::Graphics& g, const std::wstring& text,
                                    const Gdiplus::RectF& bounds, const RKDockOptions& opts,
                                    float alphaMul) {
    if (text.empty() || bounds.Width <= 0.0f || bounds.Height <= 0.0f) return;

    const int fontSize = std::clamp(opts.fontSize, 7, 72);
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

    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    const float dpiY = (g.GetDpiY() > 0.0f) ? g.GetDpiY() : 96.0f;
    const float emPx = static_cast<float>(fontSize) * dpiY / 72.0f;

    Gdiplus::GraphicsPath textPath;
    textPath.AddString(text.c_str(), -1, &ff, style, emPx, bounds, &sf);

    if (opts.alphaShadow > 0) {
        Gdiplus::Matrix mx;
        mx.Translate(1.5f, 1.5f);
        Gdiplus::GraphicsPath* sh = textPath.Clone();
        sh->Transform(&mx);
        BYTE sa = static_cast<BYTE>(std::clamp(opts.alphaShadow, 0, 255) * alphaMul);
        Gdiplus::SolidBrush sb(Gdiplus::Color(sa, GetRValue(opts.colorShadow), GetGValue(opts.colorShadow), GetBValue(opts.colorShadow)));
        g.FillPath(&sb, sh);
        delete sh;
    }
    if (opts.alphaEdge > 0) {
        BYTE ea = static_cast<BYTE>(std::clamp(opts.alphaEdge, 0, 255) * alphaMul);
        Gdiplus::Pen ep(Gdiplus::Color(ea, GetRValue(opts.colorEdge), GetGValue(opts.colorEdge), GetBValue(opts.colorEdge)), 2.2f);
        ep.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&ep, &textPath);
    }
    BYTE la = static_cast<BYTE>(std::clamp(opts.alphaLabel, 0, 255) * alphaMul);
    Gdiplus::SolidBrush lb(Gdiplus::Color(la, GetRValue(opts.colorLabel), GetGValue(opts.colorLabel), GetBValue(opts.colorLabel)));
    g.FillPath(&lb, &textPath);
}

std::vector<std::wstring> RKDockTheme::EnumerateThemes() {
    std::vector<std::wstring> list;
    std::wstring themesRoot = ResolveAppPath(L"themes");
    if (DirExists(themesRoot)) {
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW((themesRoot + L"\\*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                    wcscmp(fd.cFileName, L".") != 0 &&
                    wcscmp(fd.cFileName, L"..") != 0) {
                    std::wstring p = themesRoot + L"\\" + fd.cFileName;
                    if (FileExists(p + L"\\theme.conf") || FileExists(p + L"\\background.ini")) {
                        list.emplace_back(fd.cFileName);
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }
    bool hasDefault = false;
    for (const auto& s : list) {
        if (_wcsicmp(s.c_str(), L"Default") == 0) { hasDefault = true; break; }
    }
    if (!hasDefault) list.insert(list.begin(), L"Default");
    return list;
}

} // namespace rk
