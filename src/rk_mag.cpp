#include "rk_mag.h"
#include "rk_common.h"
#include <algorithm>
#include <cmath>

namespace rk {

namespace {

// Match 32-bit AlgMagnification::UpdateLUT (0x401040) semi-major axis so only the top
// quarter-ellipse crown above smallSize magnifies nearby items rather than spreading across ~147px+.
float ComputeEllipseSemiMajor(float smallSz, float effBig, float gap, int spanCount) {
    const int halfCount = spanCount / 2 + 1;
    const float gain = std::max(0.0f, effBig - smallSz);
    const float step = gain / static_cast<float>(halfCount);
    float semiMajorA = effBig * 0.5f + gap * static_cast<float>(halfCount);
    for (int k = 0; k < halfCount; ++k) {
        semiMajorA += smallSz + step * static_cast<float>(k);
    }
    return std::max(1.0f, semiMajorA);
}

// Match 32-bit AlgMagnification::CalcItemRect (0x401270): intersect the smallSz-to-effBig ray
// of slope (slopeHalfW / smallSz) against y(d) = effBig * sqrt(1 - (d / a)^2), clamped at smallSz.
float EvalQuarterEllipseSize(float dist, float slopeHalfW, float smallSz, float effBig, float semiMajorA) {
    if (effBig <= smallSz + 0.001f || semiMajorA <= 1.0f) return smallSz;
    auto lutVal = [&](float d) -> float {
        if (d >= semiMajorA) return 0.0f;
        float r = d / semiMajorA;
        return effBig * std::sqrt(std::max(0.0f, 1.0f - r * r));
    };
    if (lutVal(dist) <= smallSz) return smallSz;
    const float slope = slopeHalfW / std::max(1.0f, smallSz);
    float lo = smallSz;
    float hi = effBig;
    for (int iter = 0; iter < 16; ++iter) {
        float mid = 0.5f * (lo + hi);
        if (lutVal(dist + (mid - smallSz) * slope) >= mid) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return std::max(smallSz, lo);
}

} // namespace

void AlgMagnification::BuildQuarterCircleLUT(int radius, std::vector<int>& lut) {
    lut.clear();
    if (radius <= 0) {
        lut.push_back(0);
        return;
    }
    lut.resize(static_cast<size_t>(radius) + 1, 0);
    int x = 0;
    int y = radius;
    int p = 1 - radius;
    lut[0] = radius;
    while (x < y) {
        ++x;
        if (p < 0) {
            p += 2 * x + 1;
        } else {
            --y;
            p += 2 * (x - y) + 1;
        }
        if (x <= radius) lut[x] = std::max(lut[x], y);
        if (y <= radius) lut[y] = std::max(lut[y], x);
    }
    lut[radius] = 0;
}

float AlgMagnification::SeparatorSlotWidth(const RKDockOptions& opts) {
    const float smallSz = static_cast<float>(std::max(16, opts.smallSize));
    return std::max(8.0f, smallSz * 0.5f);
}

DockEffectivePadding AlgMagnification::ComputeEffectivePadding(const ThemeMargins& margins, const RKDockOptions& opts) {
    DockEffectivePadding pad;
    if (margins.useShadowMargins) {
        if (opts.backgroundAlpha == 0) {
            const int freeGap   = std::max(4, margins.margin);
            const int screenGap = std::clamp(margins.margin, 4, 6);
            pad.pillLeft   = freeGap;
            pad.pillRight  = freeGap;
            pad.pillTop    = freeGap;
            pad.pillBottom = freeGap;
            if (IsTopEdge(opts.position))         pad.pillTop    = screenGap;
            else if (IsBottomEdge(opts.position)) pad.pillBottom = screenGap;
            else if (IsLeftEdge(opts.position))   pad.pillLeft   = screenGap;
            else if (IsRightEdge(opts.position))  pad.pillRight  = screenGap;
        } else {
            const bool flush = (opts.positionMargin == 0);
            pad.shadowLeft   = margins.outsideLeft;
            pad.shadowRight  = margins.outsideRight;
            pad.shadowTop    = margins.outsideTop;
            pad.shadowBottom = margins.outsideBottom;
            if (flush) {
                if (IsTopEdge(opts.position))         pad.shadowTop    = 0;
                else if (IsBottomEdge(opts.position)) pad.shadowBottom = 0;
                else if (IsLeftEdge(opts.position))   pad.shadowLeft   = 0;
                else if (IsRightEdge(opts.position))  pad.shadowRight  = 0;
            }
            const int inner = std::max(0, margins.margin);
            pad.pillLeft   = inner;
            pad.pillRight  = inner;
            pad.pillTop    = inner;
            pad.pillBottom = inner;
        }
    } else {
        pad.pillLeft   = std::max(0, margins.outsideLeft);
        pad.pillRight  = std::max(0, margins.outsideRight);
        pad.pillTop    = std::max(0, margins.outsideTop);
        pad.pillBottom = std::max(0, margins.outsideBottom);
        if (opts.backgroundAlpha == 0) {
            if (IsTopEdge(opts.position))         pad.pillTop    = std::min(pad.pillTop, 4);
            else if (IsBottomEdge(opts.position)) pad.pillBottom = std::min(pad.pillBottom, 4);
            else if (IsLeftEdge(opts.position))   pad.pillLeft   = std::min(pad.pillLeft, 4);
            else if (IsRightEdge(opts.position))  pad.pillRight  = std::min(pad.pillRight, 4);
        }
    }

    pad.totalLeft   = pad.shadowLeft   + pad.pillLeft;
    pad.totalRight  = pad.shadowRight  + pad.pillRight;
    pad.totalTop    = pad.shadowTop    + pad.pillTop;
    pad.totalBottom = pad.shadowBottom + pad.pillBottom;
    return pad;
}

DockLayoutResult AlgMagnification::ComputeLayout(
    const std::vector<MagItemInput>& items,
    const RKDockOptions& opts,
    const ThemeMargins& margins,
    const RECT& workArea,
    bool mouseInside,
    float mouseAxisLocal,
    float zoomProgress)
{
    DockLayoutResult res;
    const size_t n = items.size();
    res.items.resize(n);

    const float smallSz = static_cast<float>(std::max(16, opts.smallSize));
    const float bigSz   = static_cast<float>(std::max<int>(opts.smallSize, opts.magnifySize));
    const float sepBase = SeparatorSlotWidth(opts);
    const float gap     = static_cast<float>(std::max(0, opts.itemDistance));
    const bool  horiz   = IsHorizontalPos(opts.position);
    const float prog    = opts.magnification ? std::clamp(zoomProgress, 0.0f, 1.0f) : 0.0f;
    const float effBig  = smallSz + (bigSz - smallSz) * prog;

    // 1. Unmagnified nominal centers along main axis starting from 0
    std::vector<float> baseWidths(n, 0.0f);
    std::vector<float> baseCenters(n, 0.0f);
    float nominalSpan = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        if (items[i].hidden) continue;
        float w = items[i].isSeparator ? sepBase : smallSz;
        baseWidths[i] = w;
        if (nominalSpan > 0.0f) nominalSpan += gap;
        baseCenters[i] = nominalSpan + w * 0.5f;
        nominalSpan += w;
    }

    const DockEffectivePadding pad = ComputeEffectivePadding(margins, opts);
    const float leadPad      = horiz ? static_cast<float>(pad.totalLeft)  : static_cast<float>(pad.totalTop);
    const float trailPad     = horiz ? static_cast<float>(pad.totalRight) : static_cast<float>(pad.totalBottom);
    const float pillLeadPad  = horiz ? static_cast<float>(pad.pillLeft)   : static_cast<float>(pad.pillTop);
    const float pillTrailPad = horiz ? static_cast<float>(pad.pillRight)  : static_cast<float>(pad.pillBottom);

    // Convert mouse coordinate from local window axis to unmagnified item axis
    const float mouseNorm = mouseAxisLocal - leadPad;

    // 2. Compute magnified item sizes using 32-bit quarter-ellipse cap & outward neighbor cascade
    const int spanCount = std::max(3, opts.itemsMagnify);
    const float gain = effBig - smallSz;

    std::vector<float> effSizes(n, 0.0f);
    std::vector<size_t> visIdx;
    std::vector<float> visStart;
    std::vector<float> visEnd;
    visIdx.reserve(n);
    visStart.reserve(n);
    visEnd.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (items[i].hidden) continue;
        effSizes[i] = baseWidths[i];
        visIdx.push_back(i);
        visStart.push_back(baseCenters[i] - baseWidths[i] * 0.5f);
        visEnd.push_back(baseCenters[i] + baseWidths[i] * 0.5f);
    }

    const size_t visibleCount = visIdx.size();
    if (visibleCount > 0 && prog > 0.001f && gain > 0.001f) {
        const float semiMajorA = ComputeEllipseSemiMajor(smallSz, effBig, gap, spanCount);
        auto scaleVisibleItem = [&](size_t itemIdx, float dist, bool isHovered) -> float {
            const float bw = baseWidths[itemIdx];
            const float slopeHalfW = isHovered ? std::min(bw * 0.5f, dist) : (bw * 0.5f);
            const float s = EvalQuarterEllipseSize(dist, slopeHalfW, smallSz, effBig, semiMajorA);
            return items[itemIdx].isSeparator ? (bw * (s / smallSz)) : s;
        };

        size_t anchorPos = visibleCount - 1;
        bool inItem = false;
        for (size_t v = 0; v < visibleCount; ++v) {
            if (mouseNorm <= visEnd[v]) {
                anchorPos = v;
                inItem = (mouseNorm >= visStart[v]);
                break;
            }
            if (v + 1 < visibleCount && mouseNorm < visStart[v + 1]) {
                anchorPos = v;
                inItem = false;
                break;
            }
        }

        const size_t anchorIdx = visIdx[anchorPos];
        effSizes[anchorIdx] = scaleVisibleItem(anchorIdx, std::abs(baseCenters[anchorIdx] - mouseNorm), inItem);

        float hLeft = 0.0f;
        float hRight = 0.0f;
        if (mouseNorm < visStart[0]) {
            hLeft = visStart[0];
            hRight = hLeft + effSizes[visIdx[0]];
        } else if (mouseNorm > visEnd[visibleCount - 1]) {
            hRight = visEnd[visibleCount - 1];
            hLeft = hRight - effSizes[visIdx[visibleCount - 1]];
        } else if (inItem) {
            const float bw = baseWidths[anchorIdx];
            const float f = (bw > 0.0f) ? ((mouseNorm - visStart[anchorPos]) / bw) : 0.5f;
            hLeft = mouseNorm - f * effSizes[anchorIdx];
            hRight = hLeft + effSizes[anchorIdx];
        } else {
            hRight = visEnd[anchorPos];
            hLeft = hRight - effSizes[anchorIdx];
        }

        float leftEdge = hLeft;
        for (size_t k = anchorPos; k > 0; --k) {
            const size_t idx = visIdx[k - 1];
            const float bw = baseWidths[idx];
            const float virtRight = leftEdge - gap;
            effSizes[idx] = scaleVisibleItem(idx, std::abs(mouseNorm - (virtRight - bw * 0.5f)), false);
            leftEdge = virtRight - effSizes[idx];
        }

        float rightEdge = hRight;
        for (size_t v = anchorPos + 1; v < visibleCount; ++v) {
            const size_t idx = visIdx[v];
            const float bw = baseWidths[idx];
            const float virtLeft = rightEdge + gap;
            effSizes[idx] = scaleVisibleItem(idx, std::abs((virtLeft + bw * 0.5f) - mouseNorm), false);
            rightEdge = virtLeft + effSizes[idx];
        }
    }

    float magSpan = 0.0f;
    for (size_t v = 0; v < visibleCount; ++v) {
        if (v > 0) magSpan += gap;
        magSpan += effSizes[visIdx[v]];
    }

    // Fallback empty dock pill size
    if (visibleCount == 0) {
        nominalSpan = smallSz;
        magSpan = smallSz;
    }

    // 3. Position magnified items strictly adjacent without overlap, anchored smoothly around mouse or center
    float anchorNominal = nominalSpan * 0.5f;
    if (prog > 0.001f && nominalSpan > 1.0f) {
        anchorNominal = std::clamp(mouseNorm, 0.0f, nominalSpan);
    }

    // Find cumulative span up to anchorNominal in magnified layout so anchor stays aligned
    float magAnchor = 0.0f;
    {
        float curNom = 0.0f;
        float curMag = 0.0f;
        bool first = true;
        bool placed = false;
        for (size_t i = 0; i < n; ++i) {
            if (items[i].hidden) continue;
            if (!first) {
                if (anchorNominal <= curNom + gap) {
                    float f = (gap > 0.0f) ? (anchorNominal - curNom) / gap : 0.0f;
                    magAnchor = curMag + gap * f;
                    placed = true;
                    break;
                }
                curNom += gap;
                curMag += gap;
            }
            first = false;
            float bw = baseWidths[i];
            float mw = effSizes[i];
            if (anchorNominal <= curNom + bw) {
                float f = (bw > 0.0f) ? (anchorNominal - curNom) / bw : 0.5f;
                magAnchor = curMag + mw * f;
                placed = true;
                break;
            }
            curNom += bw;
            curMag += mw;
        }
        if (!placed) magAnchor = curMag;
    }

    // Shift so mouse cursor remains glued under the same point, blending back to centered when idle
    float magStartAxis = leadPad + (anchorNominal - magAnchor);
    // Keep background visually stable around nominal center when mouse not magnifying
    float bgAxisStart = leadPad;
    float bgSpan = magSpan;
    (void)mouseInside;

    // Re-base coordinates cleanly so bitmap x=0..totalW always fits leftmost item and background
    float minItemStart = magStartAxis;
    float maxItemEnd   = magStartAxis + magSpan;
    float bgMin = std::min(minItemStart - leadPad, 0.0f);
    float totalMainAxis = std::max(maxItemEnd + trailPad, bgSpan + leadPad + trailPad) - bgMin;
    float shift = -bgMin;

    // Cross-axis sizing: tall enough for magnified icons + bounce headroom + margins
    const int bounceHeadroom = static_cast<int>(std::ceil(smallSz * 0.45f));
    const int pillCrossDepth = static_cast<int>(std::ceil(smallSz)) +
                               (horiz ? (pad.pillTop + pad.pillBottom) : (pad.pillLeft + pad.pillRight));
    const int bgCrossDepth = pillCrossDepth +
                             (horiz ? (pad.shadowTop + pad.shadowBottom) : (pad.shadowLeft + pad.shadowRight));
    const int maxIconCross = static_cast<int>(std::ceil(std::max(bigSz, effBig))) + bounceHeadroom + 20;
    const int totalCrossAxis = std::max(bgCrossDepth, maxIconCross);

    if (horiz) {
        res.dockWidth  = std::max(64, static_cast<int>(std::ceil(totalMainAxis)));
        res.dockHeight = totalCrossAxis;
        int pillL = static_cast<int>(std::round(magStartAxis + shift - pillLeadPad));
        int pillW = static_cast<int>(std::ceil(bgSpan + pillLeadPad + pillTrailPad));
        int pillT = IsBottomEdge(opts.position)
                        ? (res.dockHeight - pad.shadowBottom - pillCrossDepth)
                        : pad.shadowTop;
        res.pillLocalRect = { pillL, pillT, pillL + pillW, pillT + pillCrossDepth };
        res.bgLocalRect   = { pillL - pad.shadowLeft, pillT - pad.shadowTop,
                              pillL + pillW + pad.shadowRight, pillT + pillCrossDepth + pad.shadowBottom };
    } else {
        res.dockWidth  = totalCrossAxis;
        res.dockHeight = std::max(64, static_cast<int>(std::ceil(totalMainAxis)));
        int pillT = static_cast<int>(std::round(magStartAxis + shift - pillLeadPad));
        int pillH = static_cast<int>(std::ceil(bgSpan + pillLeadPad + pillTrailPad));
        int pillL = IsLeftEdge(opts.position)
                        ? pad.shadowLeft
                        : (res.dockWidth - pad.shadowRight - pillCrossDepth);
        res.pillLocalRect = { pillL, pillT, pillL + pillCrossDepth, pillT + pillH };
        res.bgLocalRect   = { pillL - pad.shadowLeft, pillT - pad.shadowTop,
                              pillL + pillCrossDepth + pad.shadowRight, pillT + pillH + pad.shadowBottom };
    }

    // Assign exact rects to each visible item
    float cursor = magStartAxis + shift;
    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        if (items[i].hidden) continue;
        if (!first) cursor += gap;
        first = false;

        float sz = effSizes[i];
        float bouncePx = items[i].bounceNorm * smallSz * 0.35f;
        res.items[i].size = sz;
        res.items[i].centerAxis = cursor + sz * 0.5f;

        if (horiz) {
            int w = static_cast<int>(std::round(sz));
            int x = static_cast<int>(std::round(cursor));
            if (items[i].isSeparator) {
                int yTop = (opts.backgroundAlpha > 0)
                               ? (res.pillLocalRect.top + margins.sepMarginTop)
                               : ((IsBottomEdge(opts.position)
                                       ? (res.pillLocalRect.bottom - pad.pillBottom - static_cast<int>(std::round(smallSz)))
                                       : (res.pillLocalRect.top + pad.pillTop)) + margins.sepMarginTop);
                int yBot = (opts.backgroundAlpha > 0)
                               ? (res.pillLocalRect.bottom - margins.sepMarginBottom)
                               : (yTop + std::max(12, static_cast<int>(std::round(smallSz)) - margins.sepMarginTop - margins.sepMarginBottom));
                if (yBot <= yTop) {
                    yTop = res.pillLocalRect.top;
                    yBot = res.pillLocalRect.bottom;
                }
                res.items[i].iconRect = { x, yTop, x + w, yBot };
            } else {
                int h = static_cast<int>(std::round(sz));
                int y = IsBottomEdge(opts.position)
                            ? (res.pillLocalRect.bottom - pad.pillBottom - static_cast<int>(std::round(bouncePx)) - h)
                            : (res.pillLocalRect.top + pad.pillTop + static_cast<int>(std::round(bouncePx)));
                res.items[i].iconRect = { x, y, x + w, y + h };
            }
        } else {
            int h = static_cast<int>(std::round(sz));
            int y = static_cast<int>(std::round(cursor));
            if (items[i].isSeparator) {
                int leadCap  = IsLeftEdge(opts.position) ? margins.sepMarginBottom : margins.sepMarginTop;
                int trailCap = IsLeftEdge(opts.position) ? margins.sepMarginTop : margins.sepMarginBottom;
                int xLeft = (opts.backgroundAlpha > 0)
                                ? (res.pillLocalRect.left + leadCap)
                                : ((IsRightEdge(opts.position)
                                        ? (res.pillLocalRect.right - pad.pillRight - static_cast<int>(std::round(smallSz)))
                                        : (res.pillLocalRect.left + pad.pillLeft)) + leadCap);
                int xRight = (opts.backgroundAlpha > 0)
                                 ? (res.pillLocalRect.right - trailCap)
                                 : (xLeft + std::max(12, static_cast<int>(std::round(smallSz)) - leadCap - trailCap));
                if (xRight <= xLeft) {
                    xLeft = res.pillLocalRect.left;
                    xRight = res.pillLocalRect.right;
                }
                res.items[i].iconRect = { xLeft, y, xRight, y + h };
            } else {
                int w = static_cast<int>(std::round(sz));
                int x = IsLeftEdge(opts.position)
                            ? (res.pillLocalRect.left + pad.pillLeft + static_cast<int>(std::round(bouncePx)))
                            : (res.pillLocalRect.right - pad.pillRight - static_cast<int>(std::round(bouncePx)) - w);
                res.items[i].iconRect = { x, y, x + w, y + h };
            }
        }

        cursor += sz;
    }

    // 4. Align windowScreenRect on workArea according to opts.position (0..11) + positionMargin
    const int waW = workArea.right - workArea.left;
    const int waH = workArea.bottom - workArea.top;
    const int margin = opts.positionMargin;

    int sx = workArea.left;
    int sy = workArea.top;

    // Horizontal edges (Top 0..2, Bottom 3..5)
    if (horiz) {
        int align = opts.position % 3; // 0=Left, 1=Center, 2=Right
        if (align == 0)      sx = workArea.left;
        else if (align == 1) sx = workArea.left + (waW - res.dockWidth) / 2;
        else                 sx = workArea.right - res.dockWidth;

        // Keep nominal center invariant during zoom if centered
        if (align == 1) {
            int nomBgCenter = (res.bgLocalRect.left + res.bgLocalRect.right) / 2;
            sx = workArea.left + waW / 2 - nomBgCenter;
        }
        sy = IsBottomEdge(opts.position)
                 ? (workArea.bottom - res.dockHeight - margin)
                 : (workArea.top + margin);
    } else {
        int align = opts.position % 3; // 0=Top, 1=Center, 2=Bottom (for Left 6..8 & Right 9..11)
        if (align == 0)      sy = workArea.top;
        else if (align == 1) {
            int nomBgCenter = (res.bgLocalRect.top + res.bgLocalRect.bottom) / 2;
            sy = workArea.top + waH / 2 - nomBgCenter;
        }
        else                 sy = workArea.bottom - res.dockHeight;

        sx = IsLeftEdge(opts.position)
                 ? (workArea.left + margin)
                 : (workArea.right - res.dockWidth - margin);
    }

    res.windowScreenRect = { sx, sy, sx + res.dockWidth, sy + res.dockHeight };
    return res;
}

POINT ComputeAutoHideOrigin(const DockLayoutResult& layout, int position, float hideOffset01) {
    int sx = layout.windowScreenRect.left;
    int sy = layout.windowScreenRect.top;
    if (hideOffset01 > 0.001f) {
        const float clamped = std::clamp(hideOffset01, 0.0f, 1.0f);
        if (IsBottomEdge(position))     sy += static_cast<int>(std::max(0, layout.dockHeight - 2) * clamped);
        else if (IsTopEdge(position))   sy -= static_cast<int>(std::max(0, layout.dockHeight - 2) * clamped);
        else if (IsLeftEdge(position))  sx -= static_cast<int>(std::max(0, layout.dockWidth  - 2) * clamped);
        else if (IsRightEdge(position)) sx += static_cast<int>(std::max(0, layout.dockWidth  - 2) * clamped);
    }
    return POINT{ sx, sy };
}

namespace {

bool IsPointOnDockEdgeMargin(POINT pt, const RKDockOptions& opts, const RECT& workArea,
                             const RECT& spanRc, int edgeThresh) {
    const bool fullMargin = opts.useMarginHotspot && opts.hotspotFullMargin;
    bool edgeMatch = false;
    bool inSpan = false;
    if (IsHorizontalPos(opts.position)) {
        if (IsTopEdge(opts.position)) {
            edgeMatch = (pt.y >= workArea.top - 2 && pt.y <= workArea.top + edgeThresh);
        } else {
            edgeMatch = (pt.y >= workArea.bottom - edgeThresh - 1 && pt.y <= workArea.bottom + 2);
        }
        inSpan = (pt.x >= workArea.left && pt.x <= workArea.right) &&
                 (fullMargin || (spanRc.right > spanRc.left && pt.x >= spanRc.left - 4 && pt.x <= spanRc.right + 4));
    } else {
        if (IsLeftEdge(opts.position)) {
            edgeMatch = (pt.x >= workArea.left - 2 && pt.x <= workArea.left + edgeThresh);
        } else {
            edgeMatch = (pt.x >= workArea.right - edgeThresh - 1 && pt.x <= workArea.right + 2);
        }
        inSpan = (pt.y >= workArea.top && pt.y <= workArea.bottom) &&
                 (fullMargin || (spanRc.bottom > spanRc.top && pt.y >= spanRc.top - 4 && pt.y <= spanRc.bottom + 4));
    }
    return edgeMatch && inSpan;
}

} // namespace

bool IsPointInDockWakeZone(POINT pt, const RKDockOptions& opts, const RECT& workArea,
                           const RECT& dockNominalRect, const RECT& currentWinRect) {
    constexpr int kEdgeThresh = 4;
    const RECT& spanRc = (dockNominalRect.right > dockNominalRect.left &&
                          dockNominalRect.bottom > dockNominalRect.top)
                             ? dockNominalRect
                             : currentWinRect;
    const bool onEdge = IsPointOnDockEdgeMargin(pt, opts, workArea, spanRc, kEdgeThresh);

    RECT visiblePeek{};
    const bool overPeek = IntersectRect(&visiblePeek, &currentWinRect, &workArea) &&
                          (PtInRect(&visiblePeek, pt) != FALSE);
    return onEdge || overPeek;
}

bool IsPointInDockMarginHotspot(POINT pt, const RKDockOptions& opts, const RECT& workArea,
                                const RECT& dockNominalRect) {
    if (!opts.useMarginHotspot) return false;
    constexpr int kHotspotEdgeThresh = 2;
    return IsPointOnDockEdgeMargin(pt, opts, workArea, dockNominalRect, kHotspotEdgeThresh);
}

} // namespace rk
