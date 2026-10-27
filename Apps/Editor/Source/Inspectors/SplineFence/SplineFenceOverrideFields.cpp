#include "Inspectors/SplineFence/SplineFenceOverrideFields.h"

#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "SplineRecipes/FenceRecipeLayout.h"
#include "SplineRecipes/FenceSpanOverrides.h"

#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "Types/ParseNumber.h"

#include <algorithm>
#include <optional>

namespace GameEngine::Editor
{
namespace
{

using Components::SplineSpanOverrideKind;

std::string MeshName(const Components::ModelRef& model)
{
    AssetMetadata metadata;
    if (EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(model.ToGuid(),
                                                                                      metadata))
    {
        if (!metadata.Name.empty())
            return metadata.Name;
        return metadata.Path.stem().string();
    }
    return model.ToGuid().ToString();
}

} // namespace

std::vector<Dropdown::Option> SpanOverrideKindOptions(std::string_view noneLabel)
{
    const ECS::FieldInfo* field =
        ECS::ComponentFieldRegistry::FindField(ECS::GetComponentTypeId<Components::SplineSpanOverride>(), "Kind");
    std::vector<Dropdown::Option> options;
    if (!field || field->EnumNames.empty())
    {
        Logger::Log::Error("SplineFenceOverrideFields: SplineSpanOverride::Kind has no reflected enumerators, so "
                           "the override kinds cannot show. Rebuild the component reflection.");
        return options;
    }
    for (const ECS::EnumNameValue& value : field->EnumNames)
    {
        const bool none = value.Value == static_cast<int64_t>(SplineSpanOverrideKind::None);
        if (none && noneLabel.empty())
            continue;
        options.push_back({std::to_string(value.Value), none ? std::string(noneLabel) : EnumDisplayName(value)});
    }
    return options;
}

int SpanOverrideKindIndex(const std::vector<Dropdown::Option>& options, SplineSpanOverrideKind kind)
{
    const std::string value = std::to_string(static_cast<int>(kind));
    const auto found = std::find_if(options.begin(), options.end(),
                                    [&value](const Dropdown::Option& option) { return option.value == value; });
    return found == options.end() ? 0 : static_cast<int>(found - options.begin());
}

std::optional<SplineSpanOverrideKind> SpanOverrideKindOfOption(const std::vector<Dropdown::Option>& options,
                                                               std::string_view value)
{
    const bool offered = std::any_of(options.begin(), options.end(),
                                     [value](const Dropdown::Option& option) { return option.value == value; });
    const std::optional<int64_t> parsed = ParseInt(value);
    if (!offered || !parsed)
        return std::nullopt;
    return static_cast<SplineSpanOverrideKind>(*parsed);
}

std::string FencePartKindName(Components::SplineFencePartKind kind)
{
    const ECS::FieldInfo* field =
        ECS::ComponentFieldRegistry::FindField(ECS::GetComponentTypeId<Components::SplineFencePart>(), "Kind");
    if (field)
    {
        for (const ECS::EnumNameValue& value : field->EnumNames)
        {
            if (value.Value == static_cast<int64_t>(kind))
                return EnumDisplayName(value);
        }
    }
    return std::to_string(static_cast<int>(kind));
}

OverridePartChoices OverridePartOptions(const Components::SplineFence& fence, Components::SplineFencePartKind kind,
                                        uint8 slot)
{
    OverridePartChoices choices;
    choices.Selected = -1;
    const std::string kindName = FencePartKindName(kind);
    std::vector<GUID> named;
    uint32 poolSlot = 0;
    for (const Components::SplineFencePart& part : fence.Parts)
    {
        if (part.Kind != kind || part.Mesh.IsNull())
            continue;
        const GUID mesh = part.Mesh.ToGuid();
        const auto sameMesh = std::count(named.begin(), named.end(), mesh);
        named.push_back(mesh);
        std::string label = kindName + ": " + MeshName(part.Mesh);
        if (sameMesh > 0)
            label += " (" + std::to_string(sameMesh + 1) + ")";
        if (poolSlot == slot)
            choices.Selected = static_cast<int>(choices.Options.size());
        choices.Options.push_back({std::to_string(poolSlot), std::move(label)});
        ++poolSlot;
    }
    if (choices.Selected < 0)
    {
        choices.Selected = static_cast<int>(choices.Options.size());
        choices.Options.push_back({std::to_string(slot), "Missing part"});
    }
    return choices;
}

std::optional<uint8> OverridePartOfOption(std::string_view value)
{
    const std::optional<uint64_t> picked = ParseUnsigned(value);
    if (!picked || *picked >= Components::kSplineFenceMaxParts)
        return std::nullopt;
    return static_cast<uint8>(*picked);
}

Dropdown* AddOverridePartRow(UIElement* parent, const std::string& label, const Components::SplineFence& fence,
                             Components::SplineFencePartKind kind, uint8 slot, const char* tooltip,
                             std::function<void(uint8)> onPicked)
{
    const OverridePartChoices choices = OverridePartOptions(fence, kind, slot);
    Dropdown* field = InspectorUI::AddDropdownRow(parent, label, choices.Options, choices.Selected, tooltip);
    field->SetOnValueChanged(
        [onPicked = std::move(onPicked)](const std::string& value)
        {
            if (const std::optional<uint8> picked = OverridePartOfOption(value))
                onPicked(*picked);
        });
    return field;
}

std::string SpanOverrideProblem(const Components::SplineFence& fence, uint32 slot, const FenceBuildState& build)
{
    if (slot >= Components::kSplineFenceMaxSpanOverrides)
        return {};
    const Components::SplineSpanOverride& entry = fence.Overrides[slot];
    if (entry.Kind == SplineSpanOverrideKind::None)
        return {};
    const bool gate = entry.Kind == SplineSpanOverrideKind::Gate;
    const std::string span = std::to_string(entry.SpanOrdinal);
    const std::string point = std::to_string(entry.PointIndex);
    const std::string name = (gate ? "The gate on span " : "The explicit part on span ") + span +
                             " of the segment from point " + point;

    if (SplineRecipes::FindSpanOverride(fence, entry.PointIndex, entry.SpanOrdinal) != &entry)
        return name + " places nothing: an override above it names the same span. Change its point or span, "
                      "or remove it.";
    if (build.SegmentCount && entry.PointIndex >= *build.SegmentCount)
    {
        const uint32 segments = *build.SegmentCount;
        const std::string starts = segments == 0u   ? "the spline has no segments"
                                   : segments == 1u ? "only point 0 starts a segment"
                                                    : "segments start at points 0 to " + std::to_string(segments - 1u);
        return "Point " + point + " starts no segment of the spline (" + starts +
               "). Change Point, or remove the override.";
    }

    const Components::SplineFencePartKind partKind = SplineRecipes::SpanOverridePartKind(entry.Kind);
    const std::string kindName = FencePartKindName(partKind);
    const uint32 pool = SplineRecipes::FencePoolOf(fence, partKind).Count;
    if (pool == 0u)
    {
        const bool hasPart = std::any_of(std::begin(fence.Parts), std::end(fence.Parts),
                                         [partKind](const Components::SplineFencePart& part)
                                         { return part.Kind == partKind; });
        const std::string fix = hasPart ? "Give the " + kindName + " part a mesh."
                                        : "Add a " + kindName + " part with Add Part.";
        const std::string lack = hasPart ? "the fence's " + kindName + " part has no mesh"
                                         : "the fence has no " + kindName + " part";
        return gate ? name + " is an opening: " + lack +
                          ". Nothing stands in it, so it cannot be selected in the scene view again. " + fix
                    : name + " places nothing: " + lack + ". " + fix;
    }
    if (entry.PoolSlot >= pool)
    {
        const std::string missing = name + " draws a " + kindName + " part that is not in the Parts list";
        const std::string fix = ". Choose one of the fence's " + kindName + " parts.";
        if (!build.Built)
            return missing + fix;
        return missing + (gate ? ", so the span is left open" : ", so the fence picks a Span part by Seed there") +
               fix;
    }

    if (!SpanOverrideMissesItsSpan(fence, slot, build))
        return {};
    const auto spanCount = static_cast<uint32>(
        std::count_if(build.StandingSpans.begin(), build.StandingSpans.end(),
                      [&entry](const std::pair<uint32, uint32>& standing) { return standing.first == entry.PointIndex; }));
    if (entry.SpanOrdinal < spanCount)
        return name + " places nothing in the last build. Change its point or span, or remove it.";
    const std::string spans = spanCount == 0u   ? "it has no spans"
                              : spanCount == 1u ? "span 0 only"
                                                : "spans 0 to " + std::to_string(spanCount - 1u);
    return "Span " + span + " is past the end of the segment from point " + point + " (" + spans +
           "). Change Span, or remove the override.";
}

bool SpanOverrideMissesItsSpan(const Components::SplineFence& fence, uint32 slot, const FenceBuildState& build)
{
    if (!build.Built || slot >= Components::kSplineFenceMaxSpanOverrides)
        return false;
    const Components::SplineSpanOverride& entry = fence.Overrides[slot];
    if (entry.Kind == SplineSpanOverrideKind::None ||
        SplineRecipes::FindSpanOverride(fence, entry.PointIndex, entry.SpanOrdinal) != &entry)
        return false;
    if (build.SegmentCount && entry.PointIndex >= *build.SegmentCount)
        return false;
    if (entry.PoolSlot >= SplineRecipes::FencePoolOf(fence, SplineRecipes::SpanOverridePartKind(entry.Kind)).Count)
        return false;
    const std::pair<uint32, uint32> address{entry.PointIndex, entry.SpanOrdinal};
    return std::find(build.StandingSpans.begin(), build.StandingSpans.end(), address) == build.StandingSpans.end();
}

} // namespace GameEngine::Editor
