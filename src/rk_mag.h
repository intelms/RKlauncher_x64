#pragma once

#include <windows.h>
#include <cmath>
#include <vector>
#include "rk_options.h"
#include "rk_theme.h"

namespace rk {

struct MagItemInput {
    bool isSeparator = false;
    float bounceNorm = 0.0f; // 0..1 perpendicular jump offset
    bool hidden      = false;
};

struct MagItemOutput {
    RECT  iconRect{};   // position inside dock bitmap
    float size = 0.0f;  // current magnified size in pixels (width/height for icons; thickness for separators)
    float centerAxis = 0.0f;
};

struct DockEffectivePadding {
    int shadowLeft   = 0;
    int shadowRight  = 0;
    int shadowTop    = 0;
    int shadowBottom = 0;
    int pillLeft     = 0;
    int pillRight    = 0;
    int pillTop      = 0;
    int pillBottom   = 0;
    int totalLeft    = 0;
    int totalRight   = 0;
    int totalTop     = 0;
    int totalBottom  = 0;
};

struct DockLayoutResult {
    RECT  windowScreenRect{}; // total layered window rect on monitor
    RECT  bgLocalRect{};      // shadow-clipped background draw bounds inside bitmap
    RECT  pillLocalRect{};    // visible glass dock pill bounds inside bitmap
    std::vector<MagItemOutput> items;
    int   dockWidth  = 0;
    int   dockHeight = 0;
};

class AlgMagnification {
public:
    // Exact midpoint quarter-circle LUT builder matching 0x4010d0
    static void BuildQuarterCircleLUT(int radius, std::vector<int>& lut);

    static float SeparatorSlotWidth(const RKDockOptions& opts);
    static DockEffectivePadding ComputeEffectivePadding(const ThemeMargins& margins, const RKDockOptions& opts);

    // Compute full dock item layout & background/screen geometry
    static DockLayoutResult ComputeLayout(
        const std::vector<MagItemInput>& items,
        const RKDockOptions& opts,
        const ThemeMargins& margins,
        const RECT& workArea,
        bool mouseInside,
        float mouseAxisLocal,
        float zoomProgress); // 0.0 (unmagnified) .. 1.0 (full magnification)
};

// Compute screen (x, y) origin after sliding off-edge according to hideOffset01 [0..1]
POINT ComputeAutoHideOrigin(const DockLayoutResult& layout, int position, float hideOffset01);

// Return true when cursor screen point hits the monitor-edge hotspot or visible peek strip
bool IsPointInDockWakeZone(POINT pt, const RKDockOptions& opts, const RECT& workArea,
                           const RECT& dockNominalRect, const RECT& currentWinRect);

// Return true when cursor sits directly on the configured screen-edge margin hotspot (for occluded/non-topmost dock)
bool IsPointInDockMarginHotspot(POINT pt, const RKDockOptions& opts, const RECT& workArea,
                                const RECT& dockNominalRect);

// Tracks physical screen cursor & zoom progress so window self-displacement
// (including synthetic WM_MOUSEMOVE) cannot feed oscillation back into layout.
struct DockHoverTracker {
    bool  hasLastCursor = false;
    POINT lastScreenPt{};
    float lastZoomProgress = -1.0f;

    void Reset() {
        hasLastCursor = false;
        lastScreenPt = POINT{};
        lastZoomProgress = -1.0f;
    }

    template <typename MapFn>
    bool UpdateAxisIfChanged(POINT screenPt, POINT clientPt, float zoomProgress,
                             float minAxisDelta, bool trackZoomTransition,
                             bool forceSample, float& inOutAxis, bool& inOutDirty,
                             MapFn&& mapToNominalFn) {
        bool cursorMoved = !hasLastCursor ||
                           (screenPt.x != lastScreenPt.x || screenPt.y != lastScreenPt.y);
        bool zoomChanged = trackZoomTransition && (std::abs(zoomProgress - lastZoomProgress) > 0.001f);
        if (!cursorMoved && !zoomChanged && !forceSample) {
            return false;
        }
        float newAxis = mapToNominalFn(clientPt);
        bool updated = false;
        if (!hasLastCursor || forceSample || std::abs(newAxis - inOutAxis) > minAxisDelta) {
            inOutAxis = newAxis;
            inOutDirty = true;
            updated = true;
        }
        hasLastCursor = true;
        lastScreenPt = screenPt;
        lastZoomProgress = zoomProgress;
        return updated;
    }
};

} // namespace rk
