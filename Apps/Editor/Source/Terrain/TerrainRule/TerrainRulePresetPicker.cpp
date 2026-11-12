#include "Terrain/TerrainRule/TerrainRulePresetPicker.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainSurface/TerrainSurfaceRulePresets.h"
#include "Types/ParseNumber.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>

namespace GameEngine
{
namespace
{

// What the control's callback needs from the inspector context, copied once.
struct PresetEditTarget
{
    ECS::World* World = nullptr;
    ECS::EntityHandle Entity{};
    Editor::EditorChangeNotifications* Notifications = nullptr;
    Editor::UndoRedoService* Undo = nullptr;
    std::function<void()> Refresh;
    TerrainECS::TerrainHeightRangeMeters HeightRangeMeters{};
};

void ApplyPresetAtIndex(const PresetEditTarget& target, const std::string& value)
{
    const std::optional<uint64_t> index = ParseUnsigned(value);
    const auto presets = TerrainECS::TerrainSurfaceRulePresets();
    if (!index || *index >= presets.size())
        return;
    const TerrainECS::TerrainSurfaceRulePreset& preset = presets[static_cast<size_t>(*index)];
    const TerrainECS::TerrainHeightRangeMeters range = target.HeightRangeMeters;
    InspectorDrag::CommitComponentWithUndo<Components::TerrainSurfaceRulesEffect>(
        target.World, target.Entity, target.Notifications, target.Undo,
        "Replace rules with " + std::string(preset.Name),
        [&preset, range](Components::TerrainSurfaceRulesEffect& effect) { ApplyTerrainRulePreset(effect, preset, range); });
    if (target.Refresh)
        target.Refresh();
}

} // namespace

std::vector<Dropdown::Option> TerrainRulePresetOptions(Components::TerrainDomain domain)
{
    std::vector<Dropdown::Option> options;
    const auto presets = TerrainECS::TerrainSurfaceRulePresets();
    for (size_t i = 0; i < presets.size(); ++i)
    {
        if (presets[i].Domain == domain)
            options.push_back({std::to_string(i), std::string(presets[i].Name)});
    }
    return options;
}

void ApplyTerrainRulePreset(Components::TerrainSurfaceRulesEffect& effect,
                            const TerrainECS::TerrainSurfaceRulePreset& preset,
                            const TerrainECS::TerrainHeightRangeMeters& heightRangeMeters)
{
    const Components::TerrainSurfaceRulesEffect rows = preset.Make(heightRangeMeters.Min, heightRangeMeters.Max);
    effect.RuleCount = rows.RuleCount;
    std::copy(std::begin(rows.Rules), std::end(rows.Rules), std::begin(effect.Rules));
    effect.ProvisionedDefaults = false;
}

void AddTerrainRulePresetRow(const InspectorContext& ctx, Components::TerrainDomain domain,
                             const TerrainECS::TerrainHeightRangeMeters& heightRangeMeters)
{
    if (!ctx.Parent)
        return;
    std::string tooltip = "Replaces every rule with the rule set's rules. Undo restores them.";
    for (const TerrainECS::TerrainSurfaceRulePreset& preset : TerrainECS::TerrainSurfaceRulePresets())
    {
        if (preset.Domain == domain)
            tooltip += "\n" + std::string(preset.Name) + ": " + std::string(preset.Description);
    }

    Dropdown* dropdown = InspectorUI::AddActionDropdown(ctx.Parent, TerrainRulePresetOptions(domain),
                                                        "Replace rules with...");
    dropdown->SetTooltip(tooltip);
    dropdown->Style()
        .Set(Style::Width, StyleLength::Px(kTerrainRuleActionWidthPx))
        .Set(Style::MaxWidth, StyleLength::Percent(100.0f))
        .Set(Style::AlignSelf, AlignItems::Center)
        .Set(Style::MarginTop, StyleLength::Px(kTerrainRuleActionMarginTopPx));

    const PresetEditTarget target{ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo,
                                  ctx.RequestInspectorRefresh, heightRangeMeters};
    dropdown->SetOnValueChanged([target](const std::string& value) { ApplyPresetAtIndex(target, value); });
}

} // namespace GameEngine
