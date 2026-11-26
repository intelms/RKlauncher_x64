#pragma once

#include "rk_gdip.h"
#include <oleidl.h>
#include <memory>
#include <string>
#include <vector>

#include "rk_options.h"
#include "rk_theme.h"
#include "rk_mag.h"
#include "rk_items.h"

namespace rk {

// Reusable 32bpp premultiplied ARGB DIB surface for UpdateLayeredWindow
class LayeredSurface {
public:
    LayeredSurface() = default;
    ~LayeredSurface();

    bool EnsureSize(int w, int h);
    void Present(HWND hwnd, int screenX, int screenY, BYTE globalAlpha = 255);
    HDC  MemoryDC() const { return memDC_; }
    int  Width() const { return width_; }
    int  Height() const { return height_; }

private:
    HDC     memDC_ = nullptr;
    HBITMAP bmp_   = nullptr;
    HBITMAP old_   = nullptr;
    void*   bits_  = nullptr;
    int     width_ = 0;
    int     height_= 0;
};

class RKDockTextWnd {
public:
    bool Create(HWND owner);
    void Destroy();
    void ShowLabel(const std::wstring& text, const RECT& anchorScreenRect,
                   const RECT& monitorWorkArea, const RKDockOptions& opts);
    void HideLabel();
    void TickFade(const RKDockOptions& opts);
    void InvalidateCache() { renderedText_.clear(); }
    HWND Hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    LayeredSurface surf_;
    std::wstring text_;
    RECT anchorRect_{};
    RECT workArea_{};
    float alpha01_ = 0.0f;
    bool visibleTarget_ = false;
    DWORD lastTick_ = 0;

    // Cached rendered label bitmap state (avoids re-measuring/redrawing on icon zoom moves & fade steps)
    std::wstring renderedText_;
    std::wstring renderedFont_;
    int          renderedFontSize_    = 0;
    bool         renderedBold_        = false;
    bool         renderedItalic_      = false;
    COLORREF     renderedColorLabel_  = 0;
    COLORREF     renderedColorEdge_   = 0;
    COLORREF     renderedColorShadow_ = 0;
    int          renderedAlphaLabel_  = -1;
    int          renderedAlphaEdge_   = -1;
    int          renderedAlphaShadow_ = -1;

    bool MatchesRenderedStyle(const RKDockOptions& opts) const;
    POINT ComputeLabelScreenPos(const RKDockOptions& opts, int w, int h) const;
    void RenderNow(const RKDockOptions& opts);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

class RKDockDragWnd {
public:
    bool Create(HWND owner);
    void Destroy();
    void Begin(Gdiplus::Bitmap* bmp, int sizePx, POINT screenPt);
    void Move(POINT screenPt, bool willDelete);
    void End();
    HWND Hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    LayeredSurface surf_;
    Gdiplus::Bitmap* bmp_ = nullptr; // non-owning
    int size_ = 64;
    bool deleteMode_ = false;

    void Render(POINT screenPt);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

using FnEffectCreate   = void* (__stdcall*)(HWND hwndTarget, Gdiplus::Bitmap* srcBmp, int param3, float param4);
using FnEffectNextStep = BOOL  (__stdcall*)(void* ctx, Gdiplus::Bitmap** outBmp, BYTE* outAlpha,
                                            const Gdiplus::Rect* targetRect, RECT* outStepRect, float* outProgressPct);
using FnEffectDestroy  = void  (__stdcall*)(void* ctx);

class RKDockEffectWnd {
public:
    bool Create(HWND owner);
    void Destroy();
    void StartPoof(POINT screenCenter, int sizePx, RKDockTheme* theme);
    void StartMinMax(HWND target, const RECT& dockIconScreenRect,
                     FnEffectCreate fnCreate, FnEffectNextStep fnNext, FnEffectDestroy fnDestroy);
    void StopMinMax();
    void Tick();
    bool IsPlaying() const { return playing_ || effPlaying_; }
    HWND Hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    LayeredSurface surf_;
    RKDockTheme* theme_ = nullptr;
    POINT center_{};
    int size_ = 96;
    int frame_ = 0;
    int totalFrames_ = 12;
    DWORD lastFrameTick_ = 0;
    bool playing_ = false;

    // External Min/Max effect playback state
    bool effPlaying_ = false;
    void* effCtx_ = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> effSrcBmp_;
    Gdiplus::Rect effDstRect_{};
    FnEffectNextStep fnEffNext_ = nullptr;
    FnEffectDestroy  fnEffDestroy_ = nullptr;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

class RKDockWnd : public IDropTarget {
public:
    RKDockWnd();
    ~RKDockWnd();

    bool Create();
    void Destroy();

    HWND Hwnd() const { return hwnd_; }
    RKDockOptions& Options() { return options_; }
    const RKDockOptions& Options() const { return options_; }

    void ApplyOptionsAndReload(bool reloadTheme = false);
    void SaveAllConfig();
    void RequestLayoutAndRedraw();

    void AddItemAtDropPoint(const std::wstring& filePath, POINT screenPt);
    void AddSeparatorNear(int index);
    void AddDockletOD(const std::wstring& relDllPath, const std::wstring& label);
    void RemoveItemAt(size_t idx, bool playPoof, POINT poofScreenCenter);
    void SyncTaskbarVisibility();

    // IUnknown / IDropTarget
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;

private:
    ULONG refCount_ = 1;
    HWND hwnd_ = nullptr;
    bool destroyed_ = false;
    bool taskbarHiddenByUs_ = false;

    RKDockOptions options_;
    RKDockTheme   theme_;
    std::vector<std::unique_ptr<RKDockItem>> items_;
    std::vector<MagItemInput> magInScratch_;
    DockLayoutResult lastLayout_{};
    LayeredSurface surface_;

    RKDockTextWnd   textWnd_;
    RKDockDragWnd   dragWnd_;
    RKDockEffectWnd effectWnd_;

    // Hook DLL & registered messages
    HMODULE hHookDll_ = nullptr;
    UINT msgHookMouse_    = 0;
    UINT msgHookCbt_      = 0;
    UINT msgTaskbarCreated_ = 0;
    UINT msgShellHook_    = 0;

    // Interaction / animation state
    bool  dirty_            = true;
    bool  mouseInside_      = false;
    HWND  dockletLockOwner_ = nullptr;
    UINT  hoverTimerIntervalMs_ = 0;
    bool IsDockletMouseLocked() const { return dockletLockOwner_ != nullptr; }
    void SetHoverTimerInterval(UINT ms);
    float mouseAxisLocal_   = 0.0f;
    float zoomProgress_     = 0.0f;
    DockHoverTracker hoverTracker_;
    int   hoveredIndex_     = -1;
    int   contextMenuIndex_ = -1;
    bool  menuOpen_         = false;

    // AutoHide slide state
    bool  autoHidden_           = false;
    float hideOffset01_         = 0.0f; // 0=visible, 1=hidden
    DWORD mouseLeaveTick_       = 0;
    DWORD hotspotEnterTick_     = 0;
    DWORD autoWakeTick_         = 0;
    bool  hotspotWaiting_       = false;
    bool  hotspotPoppedTop_     = false;
    bool  raisedFront32_        = false;
    HWND  prevFgBeforePopup_    = nullptr;

    // On-demand high-precision render timer & drag mouse hook state
    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER lastRenderQpc_{};
    bool  renderTimerActive_    = false;
    bool  renderPeriodSet_      = false;
    bool  mouseHookActive_      = false;

    // Drag reordering state
    bool  leftDown_         = false;
    bool  dragging_         = false;
    int   dragIndex_        = -1;
    POINT dragDownScreen_{};

    // Min/Max external x64 effect plugin (optional)
    HMODULE hEffectDll_ = nullptr;
    FnEffectCreate   fnEffCreate_ = nullptr;
    FnEffectNextStep fnEffNext_   = nullptr;
    FnEffectDestroy  fnEffDestroy_= nullptr;

    void LoadConfigAndItems();
    void InstallHooks();
    void UninstallHooks();
    void SetMouseHookActive(bool active);
    void MarkDirty();
    bool NeedActiveRenderTimer() const;
    void EnsureRenderTimer();
    void StopRenderTimer();
    void LoadOptionalEffectPlugin();

    RECT GetTargetWorkArea() const;
    int  HitTestItem(POINT clientPt) const;
    float MapCursorToNominalAxis(POINT clientPt) const;
    void WakeFromAutoHide();
    void SyncAutoHideZOrder();
    void UpdateHoverFromCursor();
    void RenderFrame();
    void OnTimerTick(UINT_PTR timerId);
    void OnCommand(UINT cmdId);
    void OnHookMouse(WPARAM wParam, LPARAM lParam);
    void OnHookCBT(WPARAM wParam, LPARAM lParam);
    void OnShellHook(WPARAM wParam, LPARAM lParam);
    void SyncRunningWindows();
    void RefreshIndicatorsAndWindows();
    void RestoreDockMinimizedWindows();
    void ShowDockletOrItemContextMenu(POINT screenPt, bool allowPluginOverride = true);

    static LRESULT CALLBACK DockWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace rk
