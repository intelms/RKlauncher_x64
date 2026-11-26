#pragma once

#include "rk_gdip.h"
#include <memory>
#include <string>
#include <vector>

namespace rk {

struct RKDockOptions;

enum class FillMode {
    Stretch = 0,
    Tile    = 1,
};

struct ThemeMargins {
    int  outsideLeft     = 12;
    int  outsideRight    = 12;
    int  outsideTop      = 12;
    int  outsideBottom   = 4;
    int  borderLeft      = 10;
    int  borderRight     = 10;
    int  borderTop       = 8;
    int  borderBottom    = 8;
    int  margin          = 5;
    int  sepMarginTop    = 4;
    int  sepMarginBottom = 4;
    bool useShadowMargins = true;
};

class RKDockTheme {
public:
    RKDockTheme() = default;
    ~RKDockTheme();

    RKDockTheme(const RKDockTheme&) = delete;
    RKDockTheme& operator=(const RKDockTheme&) = delete;

    bool Load(const std::wstring& themeName);
    bool LoadFromYzIni(const std::wstring& iniPath, const std::wstring& saveAsThemeName = L"");
    void Unload();

    // Returns margins rotated appropriately for the given dock position (0..11)
    ThemeMargins GetEffectiveMargins(int position) const;

    // Render dock background into dstRect (shadow-clipped bounds) aligned around pillRect
    void DrawBackground(Gdiplus::Graphics& g, const Gdiplus::Rect& dstRect, const Gdiplus::Rect& pillRect,
                        int position, int alpha0to255) const;
    void DrawSeparator(Gdiplus::Graphics& g, const Gdiplus::Rect& dstRect, int position,
                       int alpha0to255 = 255) const;
    void DrawIndicator(Gdiplus::Graphics& g, const Gdiplus::Rect& itemRect, const Gdiplus::Rect& pillRect,
                       int position) const;
    void DrawPoofFrame(Gdiplus::Graphics& g, const Gdiplus::Rect& dstRect, int frameIndex) const;
    int  GetPoofFrameCount() const;

    // Shared icon-label renderer (used by hover tooltip and Settings -> Label preview)
    static void RenderStyledLabel(Gdiplus::Graphics& g, const std::wstring& text,
                                  const Gdiplus::RectF& bounds, const RKDockOptions& opts,
                                  float alphaMul = 1.0f);

    const std::wstring& Name() const { return themeName_; }

    static std::vector<std::wstring> EnumerateThemes();

private:
    std::wstring themeName_ = L"Default";
    ThemeMargins margins_;
    FillMode fillMode_ = FillMode::Stretch;
    bool builtinFallback_ = true;

    std::unique_ptr<Gdiplus::Bitmap> bmpBg_;
    std::unique_ptr<Gdiplus::Bitmap> bmpSep_;
    std::unique_ptr<Gdiplus::Bitmap> bmpInd_;
    std::unique_ptr<Gdiplus::Bitmap> bmpPoof_;
    int poofFrames_ = 12;

    struct OrientedBitmapCache {
        Gdiplus::RotateFlipType rotKey = static_cast<Gdiplus::RotateFlipType>(-1);
        std::unique_ptr<Gdiplus::Bitmap> bmp;
    };
    mutable OrientedBitmapCache cacheBg_;
    mutable OrientedBitmapCache cacheSep_;
    mutable OrientedBitmapCache cacheInd_;

    mutable std::unique_ptr<Gdiplus::Bitmap> cachedBgBmp_;
    mutable int cachedBgW_      = 0;
    mutable int cachedBgH_      = 0;
    mutable int cachedPillRelX_ = 0;
    mutable int cachedPillRelY_ = 0;
    mutable int cachedPillW_    = 0;
    mutable int cachedPillH_    = 0;
    mutable int cachedBgEdge_   = -1;
    mutable int cachedBgAlpha_  = -1;
    mutable int cachedBgInterp_ = -1;

    void InvalidateRenderCaches() const;
    static Gdiplus::Bitmap* GetOrientedBitmap(const std::unique_ptr<Gdiplus::Bitmap>& src,
                                              Gdiplus::RotateFlipType rot,
                                              OrientedBitmapCache& cache);
    static std::unique_ptr<Gdiplus::Bitmap> LoadBitmapFile(const std::wstring& path);
    static void DrawNinePatch(Gdiplus::Graphics& g, Gdiplus::Bitmap* bmp, const Gdiplus::Rect& dst,
                              int bL, int bT, int bR, int bB, FillMode mode, float alphaMul);
};

} // namespace rk
