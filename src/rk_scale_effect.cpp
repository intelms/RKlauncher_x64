#include "rk_gdip.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct ScaleEffectCtx {
    Gdiplus::Bitmap* srcBmp = nullptr;
    int              totalSteps = 8;
    int              step = 0;
    RECT             winRect{};
};

void FitAspectBox(RECT& box, const RECT& target) {
    int tw = std::abs(target.right - target.left);
    int th = std::abs(target.bottom - target.top);
    int maxT = (tw > 0 || th > 0) ? std::max(tw, th) : 1;

    int bw = std::abs(box.right - box.left);
    int bh = std::abs(box.bottom - box.top);
    int maxB = (bw > 0 || bh > 0) ? std::max(bw, bh) : 1;

    int newW = (maxB * tw) / maxT;
    int newH = (maxB * th) / maxT;

    box.left   += (bw / 2) - (newW / 2);
    box.right   = box.left + newW;
    box.top    += (bh / 2) - (newH / 2);
    box.bottom  = box.top + newH;
}

RECT LerpRect(const RECT& src, const RECT& dst, int step, int totalSteps) {
    RECT out{};
    out.left   = src.left   + ((dst.left   - src.left)   * step) / totalSteps;
    out.right  = src.right  + ((dst.right  - src.right)  * step) / totalSteps;
    out.top    = src.top    + ((dst.top    - src.top)    * step) / totalSteps;
    out.bottom = src.bottom + ((dst.bottom - src.bottom) * step) / totalSteps;
    return out;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

extern "C" {

void* __stdcall EffectCreate(HWND hwndTarget, Gdiplus::Bitmap* srcBmp, int /*durationHint*/, float /*unused*/) {
    auto* ctx = new ScaleEffectCtx();
    ctx->srcBmp = srcBmp;
    ctx->totalSteps = 8;
    ctx->step = 0;
    if (hwndTarget && IsWindow(hwndTarget)) {
        GetWindowRect(hwndTarget, &ctx->winRect);
    }
    return ctx;
}

void __stdcall EffectDestroy(void* pCtx) {
    delete static_cast<ScaleEffectCtx*>(pCtx);
}

void __stdcall EffectGetDisplayName(wchar_t* outBuf, int maxChars) {
    if (!outBuf || maxChars <= 0) return;
    static constexpr const wchar_t kName[] = L"Scale Effect";
    const size_t nameLen = (sizeof(kName) / sizeof(kName[0])) - 1;
    const size_t copyLen = std::min(static_cast<size_t>(maxChars - 1), nameLen);
    if (copyLen > 0) {
        std::wmemcpy(outBuf, kName, copyLen);
    }
    outBuf[copyLen] = L'\0';
}

BOOL __stdcall EffectNextStep(
    void* pCtx,
    Gdiplus::Bitmap** outBmp,
    BYTE* outAlpha,
    const Gdiplus::Rect* targetRect,
    RECT* outStepRect,
    float* outProgressPct)
{
    auto* ctx = static_cast<ScaleEffectCtx*>(pCtx);
    if (!ctx || !targetRect || ctx->totalSteps <= 0) return FALSE;

    RECT target{
        targetRect->X,
        targetRect->Y,
        targetRect->X + targetRect->Width,
        targetRect->Y + targetRect->Height
    };
    RECT fittedSrc = ctx->winRect;
    FitAspectBox(fittedSrc, target);

    RECT stepRc = LerpRect(fittedSrc, target, ctx->step, ctx->totalSteps);
    int w = std::max(1, static_cast<int>(std::abs(stepRc.right - stepRc.left)));
    int h = std::max(1, static_cast<int>(std::abs(stepRc.bottom - stepRc.top)));

    Gdiplus::Bitmap* stepBmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
    if (stepBmp && stepBmp->GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::Graphics g(stepBmp);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.SetInterpolationMode(Gdiplus::InterpolationModeLowQuality);
        if (ctx->srcBmp) {
            UINT sw = ctx->srcBmp->GetWidth();
            UINT sh = ctx->srcBmp->GetHeight();
            if (sw > 0 && sh > 0) {
                g.DrawImage(ctx->srcBmp, Gdiplus::Rect(0, 0, w, h),
                            0, 0, static_cast<int>(sw), static_cast<int>(sh),
                            Gdiplus::UnitPixel);
            }
        }
    } else {
        delete stepBmp;
        stepBmp = nullptr;
    }

    if (outBmp) *outBmp = stepBmp;
    if (outAlpha) {
        int a = ((ctx->totalSteps - (ctx->step / 3)) * 255) / ctx->totalSteps;
        *outAlpha = static_cast<BYTE>(std::clamp(a, 0, 255));
    }
    if (outStepRect) {
        *outStepRect = stepRc;
    }

    ++ctx->step;
    if (outProgressPct) {
        *outProgressPct = (static_cast<float>(ctx->step) * 100.0f) / static_cast<float>(ctx->totalSteps);
    }

    return (ctx->step < ctx->totalSteps) ? TRUE : FALSE;
}

} // extern "C"
