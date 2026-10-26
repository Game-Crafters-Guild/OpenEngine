#pragma once

#include <vector>

namespace GameEngine {

class UIElement;

// Central place for the editor-wide search bar settings: placement (top or bottom of the panel)
// and the accent focus outline. Panels register their search bar root so a settings change
// reaches every bar live. The settings persist in Editor preferences (Preferences.json).
//
// IMPORTANT: Registered elements MUST be unregistered before they are destroyed
// (e.g. in panel destructor or before rebuilding UI), since this stores raw pointers.
class EditorSearchBars {
public:
    static bool GetAtTop();
    static void SetAtTop(bool atTop);
    static bool GetAccentFocusOutline();
    static void SetAccentFocusOutline(bool enabled);

    // Applies the settings to the search bar now and on every change: search-bar-at-top on the
    // bar and search-bars-at-top on its parent while bars sit at the top, the focus outline class.
    static void Register(UIElement* searchBar);
    static void Unregister(UIElement* searchBar);
    static void RegisterFocusTarget(UIElement* searchBar);
    static void UnregisterFocusTarget(UIElement* searchBar);
    
    // Load the settings from preferences
    static void LoadFromPreferences();

private:
    static void ApplyPlacement(UIElement* searchBar);
    static void ApplyFocusOutline(UIElement* searchBar);
    static void PruneNulls();
    static void SaveToPreferences();

    // What a profile that never saved a value reads.
    static constexpr bool kDefaultAtTop = true;
    static constexpr bool kDefaultAccentFocusOutline = true;

    static inline bool s_AtTop = kDefaultAtTop;
    static inline bool s_AccentFocusOutline = kDefaultAccentFocusOutline;
    static inline bool s_Loaded = false;
    static inline std::vector<UIElement*> s_Bars;
    static inline std::vector<UIElement*> s_FocusTargets;
};

} // namespace GameEngine
