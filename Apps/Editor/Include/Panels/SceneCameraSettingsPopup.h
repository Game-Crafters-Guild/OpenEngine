#pragma once

#include "Components/Rendering/PostProcessVolume.h" // ExposureMeter
#include "Panels/ExposureReadingFilter.h"
#include "UI/Interaction/DismissablePopup.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{

class Dropdown;
class FloatField;
class Label;
class PopoverArrow;
class Slider;
class Toggle;

// Quick-settings popup for the Scene View camera, opened from the scene view
// toolbar's camera button (Unity-style scene camera popup). Hosts live controls
// for FOV, clip planes, fly speed/acceleration and the Scene View exposure
// override. Every change writes straight to SceneViewSettings — the controllers
// re-read it each frame, so edits preview immediately and persist automatically.
//
// Dismissal (outside press anywhere in the window, and Escape) comes from
// UIManager's dismissable-popup registry, so nothing here has to reason about
// window-space coordinates: every rectangle below is Scene View-local.
class SceneCameraSettingsPopup : public UIElement, public DismissablePopup
{
public:
    SceneCameraSettingsPopup();
    ~SceneCameraSettingsPopup() override;

    void OnOwnerManagerChanged(UIManager* owner) override { UpdatePopupRegistration(owner); }

    // DismissablePopup. This element is a zero-size anchor at the Scene View's
    // origin whose panel escapes it, so the default popup root (this) already
    // excludes the viewport around the panel.
    bool IsPopupOpen() const override { return m_Visible; }
    void DismissPopup() override { Hide(); }

    // Show below an anchor control: anchorCenterX is the control's horizontal
    // center and anchorBottomY its bottom edge, in the parent's local space.
    // The panel clamps to the parent bounds; the arrow stays on the anchor.
    void ShowAt(float anchorCenterX, float anchorBottomY, float parentWidth, float parentHeight);
    // Re-anchor an already-open popup after its owning dock panel is resized.
    void UpdatePlacement(float anchorCenterX, float anchorBottomY,
                         float parentWidth, float parentHeight);
    void Hide();
    bool IsVisible() const { return m_Visible; }
    // Feeds the owning view's exposure readings (its Auto exposure's settled EV100 and its lights
    // reading) to the Reading line while the popover is visible; the line changes only when the
    // shown value moves by a display step (ExposureReadingFilter).
    void RefreshReading(std::optional<float> meteredEv, std::optional<float> lightsReadingEv);

    // Re-reads SceneViewSettings into the controls. Runs on every ShowAt;
    // also called externally when settings change while the popup is open
    // (e.g. the post-fx context menu's exposure mode rows, whose switch to
    // Fixed seeds Fixed EV100 as a side effect).
    void SyncFromSettings();

private:
    struct SliderRow
    {
        UIElement* row = nullptr;
        Label* label = nullptr;
        Slider* slider = nullptr;
        FloatField* field = nullptr;
    };

    struct TickMark
    {
        float Value;
        const char* Label;
    };

    // onChanged fires per tick while dragging (commit=false) and once on release
    // or field commit (commit=true); callees save to disk only on commit.
    SliderRow AddSliderRow(UIElement& parent, const char* idPrefix, const char* labelText,
                           float minValue, float maxValue, float step,
                           std::function<void(float value, bool commit)> onChanged,
                           float defaultValue, const char* tooltip);
    void AddSectionHeader(UIElement& parent, const char* text, const char* tooltip);
    // Tick lines on the slider plus a row of small value labels under it,
    // positioned post-layout on the slider's own value->x mapping (spacer
    // columns mirror the slider row's label/value widths so the strip spans
    // exactly the slider box). One tick list drives both.
    UIElement* AddTickStrip(UIElement& parent, const char* idPrefix, Slider* slider,
                            std::initializer_list<TickMark> ticks);
    // Muted caption line aligned to the slider column (e.g. the EV legend).
    UIElement* AddCaption(UIElement& parent, const char* id, const char* text);
    // A labelled dropdown row; `onSelected` receives the chosen option's index. The
    // tooltip sits on the label only: the open list drops inside the row, and a row
    // tooltip would pop up over it.
    struct DropdownRow
    {
        UIElement* row = nullptr;
        Dropdown* dropdown = nullptr;
    };
    DropdownRow AddDropdownRow(UIElement& parent, const char* idPrefix, const char* labelText,
                               const char* tooltip, const std::vector<std::string>& labels,
                               std::function<void(int index)> onSelected);
    // Auto shows the meter, its lights reading, the delay and the clamps; Fixed the
    // fixed EV pin with its tick anchors. Compensation is visible in every mode.
    void UpdateExposureRowVisibility();

    bool m_Visible = false;
    bool m_HasPlacement = false;
    float m_LastAnchorCenterX = 0.0f;
    float m_LastAnchorBottomY = 0.0f;
    float m_LastParentWidth = 0.0f;
    float m_LastParentHeight = 0.0f;
    UIElement* m_Panel = nullptr;
    PopoverArrow* m_Arrow = nullptr;
    SliderRow m_FovRow;
    SliderRow m_NearRow;
    SliderRow m_FarRow;
    SliderRow m_SpeedRow;
    SliderRow m_FastMultiplierRow;
    SliderRow m_AccelerationRow;
    SliderRow m_ExposureEvRow;
    SliderRow m_AutoMinEvRow;
    SliderRow m_AutoMaxEvRow;
    SliderRow m_CompensationRow;
    UIElement* m_ExposureEvLegend = nullptr;
    UIElement* m_AdaptationDelayRow = nullptr;
    UIElement* m_ReadingRow = nullptr;
    Label* m_ReadingLabel = nullptr;
    Editor::ExposureReadingFilter m_ReadingFilter;
    // The EV100 the view is exposed at (the clamps and the Compensation Curve applied), to the Reading
    // line's precision; shown beside the Lights reading when they differ.
    std::optional<float> m_ExposedEv;
    Components::ExposureMeter m_ReadingMeter = Components::ExposureMeter::Picture;
    Dropdown* m_ExposureModeDropdown = nullptr;
    DropdownRow m_ExposureMeterRow;
    Toggle* m_UseAdaptationDelayToggle = nullptr;
    uint64_t m_SettingsChangedListenerId = 0;
};

} // namespace GameEngine
