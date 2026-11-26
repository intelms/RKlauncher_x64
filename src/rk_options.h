#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace rk {

struct RKDockOptions {
    // [Language]
    std::wstring langResource = L"rkl_english.lang";

    // [Appearance]
    int          magnifySize     = 64;
    int          smallSize       = 48;
    int          itemDistance    = 0;
    bool         magnification   = true;
    int          backgroundAlpha = 150;
    int          itemsMagnify    = 7;
    std::wstring theme           = L"Default";
    bool         hideIndicators  = false;
    bool         hidePoofs       = false;

    // [Position]
    int  position            = 4; // Bottom Center default (0..2=Top, 3..5=Bottom, 6..8=Left, 9..11=Right)
    int  positionMargin      = 0;
    int  multiMonitorSupport = 0;

    // [Behavior]
    bool         alwaysOnTop          = false;
    bool         locked               = false;
    bool         automaticHide        = false;
    int          autoHideDelay        = 0;
    bool         saveOnExit           = true;
    bool         saveOnChange         = true;
    bool         saveItems            = true;
    bool         itemAnimation        = true;
    int          itemAnimationCount   = 5;
    int          itemAnimationNew     = 1;
    bool         minimizeToDock       = true;
    bool         useMinMaxEffect      = false;
    std::wstring minMaxEffect         = L"RKScaleEffect/RKScaleEffect.dll";
    int          minimizePosition     = -2;
    bool         newApplications      = false;
    int          newIconsPosition     = -4;
    bool         hideTaskbar          = false;
    bool         useMarginHotspot     = true;
    int          hotspotDelay         = 250;
    bool         hotspotFullMargin    = true;
    bool         neverTakeFocus       = false;
    bool         showMinimizedWndIcon = true;
    bool         startWithWindows     = false;
    int          swwRegistry          = 0;
    int          swwUser              = 0;

    // [Label]
    std::wstring font        = L"Tahoma";
    int          fontSize    = 12;
    bool         bold        = true;
    bool         italic      = false;
    COLORREF     colorLabel  = RGB(255, 255, 255);
    COLORREF     colorEdge   = RGB(0, 0, 0);
    COLORREF     colorShadow = RGB(0, 0, 0);
    int          alphaLabel  = 255;
    int          alphaEdge   = 225;
    int          alphaShadow = 205;
    bool         hideLabel   = false;
    bool         fadeLabel   = false;

    // [Menu]
    bool         showInExplorer       = true;
    std::wstring showInExplorerLabel  = L"Show in Explorer";
    bool         hideFileExtension    = true;
    bool         onlyRegistered       = false;
    bool         showSettingsAllItems = false;

    // [Performance]
    std::wstring quality              = L"Normal"; // Low / Normal / High
    int          magnifyTransitionIn  = 9;
    int          magnifyTransitionOut = 7;

    // Excluded apps loaded from excluded.conf
    std::vector<std::wstring> excludedApplications;
    std::vector<std::wstring> excludedMinimize;

    void ClampValid();
    bool LoadFromFile(const std::wstring& confPath);
    bool SaveToFile(const std::wstring& confPath) const;

    bool LoadExclusions(const std::wstring& exclPath);
    bool SaveExclusions(const std::wstring& exclPath) const;

    bool IsAppExcluded(const std::wstring& exeOrName) const;
    bool IsMinimizeExcluded(const std::wstring& exeOrName) const;
};

} // namespace rk
