#include "rk_options.h"
#include "rk_ini.h"
#include "rk_common.h"
#include <algorithm>
#include <shlwapi.h>

namespace rk {

void RKDockOptions::ClampValid() {
    if (smallSize <= 0) smallSize = 48;
    if (smallSize < 16) smallSize = 16;
    if (smallSize > 256) smallSize = 256;
    if (magnifySize < smallSize) magnifySize = smallSize;
    if (magnifySize > 256) magnifySize = 256;
    if (itemDistance < 0) itemDistance = 0;
    if (itemDistance > 64) itemDistance = 64;
    if (backgroundAlpha < 0) backgroundAlpha = 0;
    if (backgroundAlpha > 255) backgroundAlpha = 255;
    if (itemsMagnify < 3) itemsMagnify = 7;
    if (itemsMagnify > 21) itemsMagnify = 21;
    if (theme.empty()) theme = L"Default";
    if (font.empty()) font = L"Tahoma";
    if (fontSize <= 0) fontSize = 12;
    if (position < 0 || position > 11) position = 4;
    if (alphaLabel < 0 || alphaLabel > 255) alphaLabel = 255;
    if (alphaEdge < 0 || alphaEdge > 255) alphaEdge = 225;
    if (alphaShadow < 0 || alphaShadow > 255) alphaShadow = 205;
    if (magnifyTransitionIn < 1) magnifyTransitionIn = 9;
    if (magnifyTransitionOut < 1) magnifyTransitionOut = 7;
}

bool RKDockOptions::LoadFromFile(const std::wstring& confPath) {
    IniFile ini;
    if (!ini.Load(confPath)) {
        ClampValid();
        return false;
    }

    langResource = ini.GetString(L"Language", L"langResource", langResource);

    magnifySize     = ini.GetInt(L"Appearance", L"magnifySize", magnifySize);
    smallSize       = ini.GetInt(L"Appearance", L"smallSize", smallSize);
    itemDistance    = ini.GetInt(L"Appearance", L"itemDistance", itemDistance);
    magnification   = ini.GetBool(L"Appearance", L"magnification", magnification);
    backgroundAlpha = ini.GetInt(L"Appearance", L"backgroundAlpha", backgroundAlpha);
    itemsMagnify    = ini.GetInt(L"Appearance", L"itemsMagnify", itemsMagnify);
    theme           = ini.GetString(L"Appearance", L"theme", theme);
    hideIndicators  = ini.GetBool(L"Appearance", L"hideIndicators", hideIndicators);
    hidePoofs       = ini.GetBool(L"Appearance", L"hidePoofs", hidePoofs);

    position            = ini.GetInt(L"Position", L"position", position);
    positionMargin      = ini.GetInt(L"Position", L"positionMargin", positionMargin);
    multiMonitorSupport = ini.GetInt(L"Position", L"multiMonitorSupport", multiMonitorSupport);

    alwaysOnTop          = ini.GetBool(L"Behavior", L"alwaysOnTop", alwaysOnTop);
    locked               = ini.GetBool(L"Behavior", L"locked", locked);
    automaticHide        = ini.GetBool(L"Behavior", L"automaticHide", automaticHide);
    autoHideDelay        = ini.GetInt(L"Behavior", L"autoHideDelay", autoHideDelay);
    saveOnExit           = ini.GetBool(L"Behavior", L"saveOnExit", saveOnExit);
    saveOnChange         = ini.GetBool(L"Behavior", L"saveOnChange", saveOnChange);
    saveItems            = ini.GetBool(L"Behavior", L"saveItems", saveItems);
    itemAnimation        = ini.GetBool(L"Behavior", L"itemAnimation", itemAnimation);
    itemAnimationCount   = ini.GetInt(L"Behavior", L"itemAnimationCount", itemAnimationCount);
    itemAnimationNew     = ini.GetInt(L"Behavior", L"itemAnimationNew", itemAnimationNew);
    minimizeToDock       = ini.GetBool(L"Behavior", L"minimizeToDock", minimizeToDock);
    useMinMaxEffect      = ini.GetBool(L"Behavior", L"useMinMaxEffect", useMinMaxEffect);
    minMaxEffect         = ini.GetString(L"Behavior", L"minMaxEffect", minMaxEffect);
    minimizePosition     = ini.GetInt(L"Behavior", L"minimizePosition", minimizePosition);
    newApplications      = ini.GetBool(L"Behavior", L"newApplications", newApplications);
    newIconsPosition     = ini.GetInt(L"Behavior", L"newIconsPosition", newIconsPosition);
    hideTaskbar          = ini.GetBool(L"Behavior", L"hideTaskbar", hideTaskbar);
    useMarginHotspot     = ini.GetBool(L"Behavior", L"useMarginHotspot", useMarginHotspot);
    hotspotDelay         = ini.GetInt(L"Behavior", L"hotspotDelay", hotspotDelay);
    hotspotFullMargin    = ini.GetBool(L"Behavior", L"hotspotFullMargin", hotspotFullMargin);
    neverTakeFocus       = ini.GetBool(L"Behavior", L"neverTakeFocus", neverTakeFocus);
    showMinimizedWndIcon = ini.GetBool(L"Behavior", L"showMinimizedWndIcon", showMinimizedWndIcon);
    startWithWindows     = ini.GetBool(L"Behavior", L"startWithWindows", startWithWindows);
    swwRegistry          = ini.GetInt(L"Behavior", L"swwRegistry", swwRegistry);
    swwUser              = ini.GetInt(L"Behavior", L"swwUser", swwUser);

    font        = ini.GetString(L"Label", L"font", font);
    fontSize    = ini.GetInt(L"Label", L"fontSize", fontSize);
    bold        = ini.GetBool(L"Label", L"bold", bold);
    italic      = ini.GetBool(L"Label", L"italic", italic);
    colorLabel  = static_cast<COLORREF>(ini.GetInt(L"Label", L"colorLabel", static_cast<int>(colorLabel)));
    colorEdge   = static_cast<COLORREF>(ini.GetInt(L"Label", L"colorEdge", static_cast<int>(colorEdge)));
    colorShadow = static_cast<COLORREF>(ini.GetInt(L"Label", L"colorShadow", static_cast<int>(colorShadow)));
    alphaLabel  = ini.GetInt(L"Label", L"alphaLabel", alphaLabel);
    alphaEdge   = ini.GetInt(L"Label", L"alphaEdge", alphaEdge);
    alphaShadow = ini.GetInt(L"Label", L"alphaShadow", alphaShadow);
    hideLabel   = ini.GetBool(L"Label", L"hideLabel", hideLabel);
    fadeLabel   = ini.GetBool(L"Label", L"fadeLabel", fadeLabel);

    showInExplorer       = ini.GetBool(L"Menu", L"showInExplorer", showInExplorer);
    showInExplorerLabel  = ini.GetString(L"Menu", L"showInExplorerLabel", showInExplorerLabel);
    hideFileExtension    = ini.GetBool(L"Menu", L"hideFileExtension", hideFileExtension);
    onlyRegistered       = ini.GetBool(L"Menu", L"onlyRegistered", onlyRegistered);
    showSettingsAllItems = ini.GetBool(L"Menu", L"showSettingsAllItems", showSettingsAllItems);

    quality              = ini.GetString(L"Performance", L"quality", quality);
    magnifyTransitionIn  = ini.GetInt(L"Performance", L"magnifyTransitionIn", magnifyTransitionIn);
    magnifyTransitionOut = ini.GetInt(L"Performance", L"magnifyTransitionOut", magnifyTransitionOut);

    ClampValid();
    return true;
}

bool RKDockOptions::SaveToFile(const std::wstring& confPath) const {
    IniFile ini;
    ini.SetString(L"Language", L"langResource", langResource);

    ini.SetInt(L"Appearance", L"magnifySize", magnifySize);
    ini.SetInt(L"Appearance", L"smallSize", smallSize);
    ini.SetInt(L"Appearance", L"itemDistance", itemDistance);
    ini.SetBool(L"Appearance", L"magnification", magnification);
    ini.SetInt(L"Appearance", L"backgroundAlpha", backgroundAlpha);
    ini.SetInt(L"Appearance", L"itemsMagnify", itemsMagnify);
    ini.SetString(L"Appearance", L"theme", theme);
    ini.SetBool(L"Appearance", L"hideIndicators", hideIndicators);
    ini.SetBool(L"Appearance", L"hidePoofs", hidePoofs);

    ini.SetInt(L"Position", L"position", position);
    ini.SetInt(L"Position", L"positionMargin", positionMargin);
    ini.SetInt(L"Position", L"multiMonitorSupport", multiMonitorSupport);

    ini.SetBool(L"Behavior", L"alwaysOnTop", alwaysOnTop);
    ini.SetBool(L"Behavior", L"locked", locked);
    ini.SetBool(L"Behavior", L"automaticHide", automaticHide);
    ini.SetInt(L"Behavior", L"autoHideDelay", autoHideDelay);
    ini.SetBool(L"Behavior", L"saveOnExit", saveOnExit);
    ini.SetBool(L"Behavior", L"saveOnChange", saveOnChange);
    ini.SetBool(L"Behavior", L"saveItems", saveItems);
    ini.SetBool(L"Behavior", L"itemAnimation", itemAnimation);
    ini.SetInt(L"Behavior", L"itemAnimationCount", itemAnimationCount);
    ini.SetInt(L"Behavior", L"itemAnimationNew", itemAnimationNew);
    ini.SetBool(L"Behavior", L"minimizeToDock", minimizeToDock);
    ini.SetBool(L"Behavior", L"useMinMaxEffect", useMinMaxEffect);
    ini.SetString(L"Behavior", L"minMaxEffect", minMaxEffect);
    ini.SetInt(L"Behavior", L"minimizePosition", minimizePosition);
    ini.SetBool(L"Behavior", L"newApplications", newApplications);
    ini.SetInt(L"Behavior", L"newIconsPosition", newIconsPosition);
    ini.SetBool(L"Behavior", L"hideTaskbar", hideTaskbar);
    ini.SetBool(L"Behavior", L"useMarginHotspot", useMarginHotspot);
    ini.SetInt(L"Behavior", L"hotspotDelay", hotspotDelay);
    ini.SetBool(L"Behavior", L"hotspotFullMargin", hotspotFullMargin);
    ini.SetBool(L"Behavior", L"neverTakeFocus", neverTakeFocus);
    ini.SetBool(L"Behavior", L"showMinimizedWndIcon", showMinimizedWndIcon);
    ini.SetBool(L"Behavior", L"startWithWindows", startWithWindows);
    ini.SetInt(L"Behavior", L"swwRegistry", swwRegistry);
    ini.SetInt(L"Behavior", L"swwUser", swwUser);

    ini.SetString(L"Label", L"font", font);
    ini.SetInt(L"Label", L"fontSize", fontSize);
    ini.SetBool(L"Label", L"bold", bold);
    ini.SetBool(L"Label", L"italic", italic);
    ini.SetInt(L"Label", L"colorLabel", static_cast<int>(colorLabel));
    ini.SetInt(L"Label", L"colorEdge", static_cast<int>(colorEdge));
    ini.SetInt(L"Label", L"colorShadow", static_cast<int>(colorShadow));
    ini.SetInt(L"Label", L"alphaLabel", alphaLabel);
    ini.SetInt(L"Label", L"alphaEdge", alphaEdge);
    ini.SetInt(L"Label", L"alphaShadow", alphaShadow);
    ini.SetBool(L"Label", L"hideLabel", hideLabel);
    ini.SetBool(L"Label", L"fadeLabel", fadeLabel);

    ini.SetBool(L"Menu", L"showInExplorer", showInExplorer);
    ini.SetString(L"Menu", L"showInExplorerLabel", showInExplorerLabel);
    ini.SetBool(L"Menu", L"hideFileExtension", hideFileExtension);
    ini.SetBool(L"Menu", L"onlyRegistered", onlyRegistered);
    ini.SetBool(L"Menu", L"showSettingsAllItems", showSettingsAllItems);

    ini.SetString(L"Performance", L"quality", quality);
    ini.SetInt(L"Performance", L"magnifyTransitionIn", magnifyTransitionIn);
    ini.SetInt(L"Performance", L"magnifyTransitionOut", magnifyTransitionOut);

    return ini.Save(confPath, L"; RK Launcher configuration file");
}

bool RKDockOptions::LoadExclusions(const std::wstring& exclPath) {
    excludedApplications.clear();
    excludedMinimize.clear();
    IniFile ini;
    if (!ini.Load(exclPath)) return false;

    for (const auto& sec : ini.Sections()) {
        if (_wcsicmp(sec.name.c_str(), L"RKDockExcludedApplication") == 0) {
            for (const auto& e : sec.entries) {
                if (_wcsicmp(e.key.c_str(), L"application") == 0 && !e.value.empty()) {
                    excludedApplications.push_back(e.value);
                }
            }
        } else if (_wcsicmp(sec.name.c_str(), L"RKDockExcludedMinimize") == 0) {
            for (const auto& e : sec.entries) {
                if (_wcsicmp(e.key.c_str(), L"application") == 0 && !e.value.empty()) {
                    excludedMinimize.push_back(e.value);
                }
            }
        }
    }
    return true;
}

bool RKDockOptions::SaveExclusions(const std::wstring& exclPath) const {
    IniFile ini;
    auto& sApp = ini.AddSection(L"RKDockExcludedApplication");
    for (const auto& v : excludedApplications) {
        sApp.entries.push_back({L"application", v});
    }
    auto& sMin = ini.AddSection(L"RKDockExcludedMinimize");
    for (const auto& v : excludedMinimize) {
        sMin.entries.push_back({L"application", v});
    }
    return ini.Save(exclPath, L"; RK Launcher exclusions");
}

static bool MatchExclusionList(const std::vector<std::wstring>& list, const std::wstring& pathOrName) {
    if (pathOrName.empty()) return false;
    std::wstring lowFull = ToLower(pathOrName);
    const wchar_t* fileOnlyPtr = PathFindFileNameW(pathOrName.c_str());
    std::wstring lowBase = ToLower(fileOnlyPtr ? fileOnlyPtr : pathOrName);
    for (const auto& pat : list) {
        std::wstring p = ToLower(pat);
        if (p == lowFull || p == lowBase) return true;
        if (!lowFull.empty() && lowFull.find(p) != std::wstring::npos) return true;
    }
    return false;
}

bool RKDockOptions::IsAppExcluded(const std::wstring& exeOrName) const {
    return MatchExclusionList(excludedApplications, exeOrName);
}

bool RKDockOptions::IsMinimizeExcluded(const std::wstring& exeOrName) const {
    return MatchExclusionList(excludedMinimize, exeOrName);
}

} // namespace rk
