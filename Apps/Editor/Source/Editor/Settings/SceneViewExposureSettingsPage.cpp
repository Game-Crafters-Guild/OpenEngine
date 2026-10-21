#include "Editor/Settings/SceneViewExposureSettingsPage.h"

#include "Components/Rendering/PostProcessVolume.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "UI/ExposureMeterText.h"

#include <array>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr std::array kModes = {SceneViewExposureMode::Auto, SceneViewExposureMode::Fixed};
constexpr std::array kMeters = {Components::ExposureMeter::Picture, Components::ExposureMeter::Lights};
constexpr const char* kSearchKeywords = "scene view exposure ev100 auto fixed meter picture lights";

std::string ModeLabel()
{
    return SceneViewExposureModeLabel(SceneViewSettings::Get().GetExposureMode());
}

// The row fires once with the value it was seeded from, so an unchanged mode must not write the
// preferences: opening the page is not an edit.
void SetModeFromLabel(const std::string& label)
{
    for (const SceneViewExposureMode mode : kModes)
    {
        if (label != SceneViewExposureModeLabel(mode))
            continue;
        if (SceneViewSettings::Get().GetExposureMode() != mode)
            SceneViewSettings::Get().SetExposureMode(mode, /*fromUserSwitch=*/true);
        return;
    }
}

SettingsFieldDescriptor ModeRow()
{
    SettingsFieldDescriptor row;
    row.Label = "Exposure Mode";
    row.Tooltip = "How the Scene View sets its exposure. Auto meters the view with the Meter below, within "
                  "Auto Min and Max EV100. Fixed holds the Fixed EV100 below.";
    row.SearchKeywords = kSearchKeywords;
    SettingsFieldDescriptor::DropdownField dropdown;
    for (const SceneViewExposureMode mode : kModes)
        dropdown.Options.emplace_back(SceneViewExposureModeLabel(mode));
    dropdown.DefaultValue = SceneViewExposureModeLabel(SceneViewSettings::kDefaultExposureMode);
    dropdown.Get = ModeLabel;
    dropdown.Set = SetModeFromLabel;
    row.Control = std::move(dropdown);
    return row;
}

std::string MeterLabel()
{
    return ExposureMeterLabel(SceneViewSettings::Get().GetExposureMeter());
}

void SetMeterFromLabel(const std::string& label)
{
    for (const Components::ExposureMeter meter : kMeters)
        if (label == ExposureMeterLabel(meter))
            SceneViewSettings::Get().SetExposureMeter(meter);
}

SettingsFieldDescriptor MeterRow()
{
    SettingsFieldDescriptor row;
    row.Label = "Meter";
    row.Tooltip = kExposureMeterTooltip;
    row.SearchKeywords = kSearchKeywords;
    SettingsFieldDescriptor::DropdownField dropdown;
    for (const Components::ExposureMeter meter : kMeters)
        dropdown.Options.emplace_back(ExposureMeterLabel(meter));
    dropdown.DefaultValue = ExposureMeterLabel(SceneViewSettings::kDefaultExposureMeter);
    dropdown.Get = MeterLabel;
    dropdown.Set = SetMeterFromLabel;
    row.Control = std::move(dropdown);
    return row;
}

SettingsFieldDescriptor EvRow(const char* label, const char* tooltip, float defaultValue, float minValue,
                              float maxValue,
                              float (SceneViewSettings::*get)() const,
                              void (SceneViewSettings::*set)(float, bool))
{
    SettingsFieldDescriptor row;
    row.Label = label;
    row.Tooltip = tooltip;
    row.SearchKeywords = kSearchKeywords;
    SettingsFieldDescriptor::SliderField slider;
    slider.DefaultValue = defaultValue;
    slider.MinValue = minValue;
    slider.MaxValue = maxValue;
    slider.Step = SceneViewSettings::kExposureSliderStep;
    slider.Get = [get]() { return (SceneViewSettings::Get().*get)(); };
    slider.Set = [get, set](float value)
    {
        SceneViewSettings& settings = SceneViewSettings::Get();
        if ((settings.*get)() != value)
            (settings.*set)(value, true);
    };
    row.Control = std::move(slider);
    return row;
}
} // namespace

void RegisterSceneViewExposureSettingsCategory()
{
    SettingsCategoryDescriptor category;
    category.CategoryId = "sceneViewExposure";
    category.Title = "Exposure";
    category.Group = SettingsCategoryGroup::SceneView;
    category.SearchKeywords = kSearchKeywords;
    category.Description =
        "How the editor's Scene View sets its exposure; the Game View always uses its camera's. Auto meters "
        "the view with the Meter: Lights reads the light falling on the world from the directional lights and "
        "the sky, Picture meters the image like a camera in Auto. Either is clamped to the Auto Min and Max "
        "EV100, which ship wide open so a night scene stays workable. Fixed holds the Fixed EV100. Only the "
        "rows for the selected mode apply; EV Compensation applies in every mode.";

    category.Fields.push_back(ModeRow());
    category.Fields.push_back(MeterRow());
    category.Fields.push_back(EvRow(
        "Fixed EV100",
        "Fixed mode: the absolute photographic EV100 the Scene View holds. Sunny ~15, overcast ~12, "
        "interior ~7-9, full moon ~-4.",
        Components::kDefaultManualExposureEv, SceneViewSettings::kMinExposureEv, SceneViewSettings::kMaxExposureEv,
        &SceneViewSettings::GetPostFxFixedExposureEv, &SceneViewSettings::SetPostFxFixedExposureEv));
    category.Fields.push_back(EvRow(
        "Auto Min EV100",
        "Auto mode, either meter: the brightest the metering may expose to (lower EV = more brightening "
        "in dark scenes).",
        SceneViewSettings::kDefaultAutoExposureMinEv, SceneViewSettings::kMinExposureEv, SceneViewSettings::kMaxExposureEv,
        &SceneViewSettings::GetPostFxAutoExposureMinEv, &SceneViewSettings::SetPostFxAutoExposureMinEv));
    category.Fields.push_back(EvRow(
        "Auto Max EV100",
        "Auto mode, either meter: the darkest the metering may expose to (higher EV = more darkening in "
        "bright scenes).",
        SceneViewSettings::kDefaultAutoExposureMaxEv, SceneViewSettings::kMinExposureEv, SceneViewSettings::kMaxExposureEv,
        &SceneViewSettings::GetPostFxAutoExposureMaxEv, &SceneViewSettings::SetPostFxAutoExposureMaxEv));
    category.Fields.push_back(EvRow(
        "EV Compensation",
        "Every mode: +/- stops applied on top of the Scene View's exposure (+ brightens).", 0.0f,
        -SceneViewSettings::kMaxExposureCompensation, SceneViewSettings::kMaxExposureCompensation,
        &SceneViewSettings::GetPostFxExposureCompensation, &SceneViewSettings::SetPostFxExposureCompensation));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(category));
}

} // namespace GameEngine::Editor
