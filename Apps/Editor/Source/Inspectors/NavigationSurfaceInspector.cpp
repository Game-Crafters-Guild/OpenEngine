#include "Inspectors/NavigationSurfaceInspector.h"

#include "Editor/Entities/EditorComponentTraits.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "SceneView/Gizmos/NavigationSurfaceGizmo.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Vector3Field.h"
#include "UI/InspectorSection.h"

#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Components/Transform.h"
#include "Mathematics/Vector3.h"
#include "Pathfinding/PathfindingTypes.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "Types/StringUtils.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{

using Components::NavigationBakePhase;
using Components::NavigationSurface;
using Components::NavigationSurfaceMode;
using InspectorDrag::CommitComponentWithUndo;

constexpr std::array<std::string_view, 1> kGridTypeField = {"GridType"};
constexpr std::array<std::string_view, 1> kFitBoundsField = {"FitBounds"};
constexpr std::array<std::string_view, 1> kAuthoredCellsField = {"AuthoredCells"};

// The agent and resolution rows, hand-built so each label carries its unit; tooltip and range come
// from the reflection.
struct AgentRow
{
    std::string_view Field;
    const char* Label;
    float32 NavigationSurface::*Member;
};
constexpr std::array<AgentRow, 5> kAgentRows = {{
    {"AgentRadius", "Agent Radius (m)", &NavigationSurface::AgentRadius},
    {"AgentHeight", "Agent Height (m)", &NavigationSurface::AgentHeight},
    {"MaxSlope", "Max Slope (\xC2\xB0)", &NavigationSurface::MaxSlope},
    {"StepHeight", "Step Height (m)", &NavigationSurface::StepHeight},
    {"CellSize", "Cell Size (m)", &NavigationSurface::CellSize},
}};

constexpr const char* kFittedBoundsReason =
    "Fit Bounds is on: the surface covers the terrain's footprint, or everything the bake reads when there is no "
    "terrain. The values show that box.";

// The one resolution dial starts at a different value per mode: a 0.5 m voxel follows the ground
// for a mesh, a 1 m cell keeps a grid's cell count down.
constexpr float32 kMeshCellSize = 0.5f;
constexpr float32 kGridCellSize = 1.0f;

const std::vector<Dropdown::Option>& ModeOptions()
{
    static const std::vector<Dropdown::Option> kOptions = {
        {"0", "Mesh"},
        {"1", "Grid"},
    };
    return kOptions;
}

// The values that decide which rows the inspector shows and how its notice reads.
struct SurfaceLayout
{
    NavigationSurfaceMode Mode = NavigationSurfaceMode::Mesh;
    bool FitBounds = true;
    bool Failed = false;

    bool operator==(const SurfaceLayout&) const = default;
};

SurfaceLayout LayoutOf(const NavigationSurface& surface)
{
    return {surface.Mode, surface.FitBounds, surface.BakePhase == NavigationBakePhase::Failed};
}

// The header badge: green when the bake is current, amber when it needs attention (not baked,
// waiting, out of date), red when it failed, neutral while it runs.
void SetBadge(InspectorSection& section, const NavigationSurface& surface)
{
    using Kind = InspectorSection::BadgeKind;
    switch (surface.BakePhase)
    {
    case NavigationBakePhase::NotBaked: section.SetHeaderBadge("Not baked", "", Kind::Warning); return;
    case NavigationBakePhase::Waiting: section.SetHeaderBadge("Waiting", "", Kind::Warning); return;
    case NavigationBakePhase::Baking: section.SetHeaderBadge("Baking", "", Kind::Neutral); return;
    case NavigationBakePhase::Baked:
        if (surface.SettingsChanged)
            section.SetHeaderBadge("Out of date", "", Kind::Warning);
        else
            section.SetHeaderBadge("Baked", "", Kind::Success);
        return;
    case NavigationBakePhase::Failed: section.SetHeaderBadge("Failed", "", Kind::Error); return;
    }
}

// The box the surface covers, as Navigation::SurfaceBounds gives it.
struct SurfaceBox
{
    bool Known = false;
    Mathematics::Vector3 Min;
    Mathematics::Vector3 Max;
};

SurfaceBox BoxOf(ECS::World& world, ECS::EntityHandle entity)
{
    SurfaceBox box;
    box.Known = Navigation::SurfaceBounds(world, entity, box.Min, box.Max);
    return box;
}

// What the Cell Size row amounts to over the current bounds, before any bake: "32 m tiles, 1,024
// tiles" for a mesh, "1,048,576 cells" for a grid. The last bake's counts are in the notice.
std::string CellSizeCaption(const NavigationSurface& surface, const SurfaceBox& box)
{
    const std::string count = box.Known ? FormatGroupedInteger(Navigation::BakeUnitCount(surface, box.Min, box.Max)) : "";
    if (surface.Mode == NavigationSurfaceMode::Grid)
        return box.Known ? count + " cells" : "";
    char text[64];
    std::snprintf(text, sizeof(text), "%g m tiles", static_cast<double>(Pathfinding::kNavMeshTileCells * surface.CellSize));
    std::string caption = text;
    if (box.Known)
        caption += ", " + count + " tiles";
    return caption;
}

void AddModeRow(UIElement* parent, const InspectorContext& ctx, ECS::ComponentTypeId typeId, NavigationSurfaceMode mode)
{
    // The row is hand-built for the cell-size follow below; its tooltip is the reflected one.
    const ECS::FieldInfo* field = ECS::ComponentFieldRegistry::FindField(typeId, "Mode");
    const std::string tooltip = field ? std::string(field->Tooltip) : std::string();
    Dropdown* dropdown = InspectorUI::AddDropdownRow(parent, "Mode", ModeOptions(), static_cast<int>(mode),
                                                     tooltip.empty() ? nullptr : tooltip.c_str());
    dropdown->SetId("navigation-surface-mode");
    dropdown->SetOnValueChanged(
        [w = ctx.World, e = ctx.Entity, n = ctx.ChangeNotifications, undo = ctx.Undo,
         refresh = ctx.RequestInspectorRefresh](const std::string& value)
        {
            const auto picked = static_cast<NavigationSurfaceMode>(std::stoi(value));
            CommitComponentWithUndo<NavigationSurface>(
                w, e, n, undo, "Change Navigation Surface Mode",
                [picked](NavigationSurface& surface)
                {
                    // A cell size still at the other mode's starting value follows the mode.
                    const float32 from = picked == NavigationSurfaceMode::Grid ? kMeshCellSize : kGridCellSize;
                    const float32 to = picked == NavigationSurfaceMode::Grid ? kGridCellSize : kMeshCellSize;
                    if (surface.Mode != picked && surface.CellSize == from)
                        surface.CellSize = to;
                    surface.Mode = picked;
                });
            if (refresh)
                refresh();
        });
}

const char* ReflectedTooltip(ECS::ComponentTypeId typeId, std::string_view fieldName)
{
    const ECS::FieldInfo* field = ECS::ComponentFieldRegistry::FindField(typeId, fieldName);
    return field && !field->Tooltip.empty() ? field->Tooltip.data() : nullptr;
}

void AddAgentRow(const InspectorContext& ctx, ECS::ComponentTypeId typeId, const NavigationSurface& surface,
                 const AgentRow& row)
{
    const ECS::FieldInfo* field = ECS::ComponentFieldRegistry::FindField(typeId, row.Field);
    const float minValue = field && field->HasRange ? field->MinValue : -std::numeric_limits<float>::infinity();
    const float maxValue = field && field->HasRange ? field->MaxValue : std::numeric_limits<float>::infinity();
    const auto member = row.Member;
    InspectorDrag::AddComponentFloatRowWithDrag<NavigationSurface>(
        ctx.Parent, row.Label, surface.*member, ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo,
        std::string("Change Navigation Surface ") + row.Label,
        [member](NavigationSurface& target, float value) { target.*member = value; }, NavigationSurface{}.*member,
        ReflectedTooltip(typeId, row.Field), {}, minValue, maxValue);
}

// Center or Extent as one Vector3 row. While Fit Bounds is on the row is inactive and shows the
// fitted box, the reason in its tooltip.
void AddBoundsVectorRow(const InspectorContext& ctx, ECS::ComponentTypeId typeId, const char* label,
                        std::string_view fieldName, const Mathematics::Vector3& value,
                        Mathematics::Vector3 NavigationSurface::*member, bool fitted)
{
    UIElement* row = InspectorUI::AddRow(ctx.Parent);
    InspectorUI::AddLabel(row, label, ReflectedTooltip(typeId, fieldName));
    auto field = std::make_unique<Vector3Field>();
    field->AddClass("inspector-vector3");
    field->SetValue(Rendering::Vector3(value.x, value.y, value.z));
    Vector3Field* raw = field.get();
    InspectorUI::AddFieldContainer(row)->AddChild(std::move(field));
    if (fitted)
    {
        InspectorUI::DisableRow(row, kFittedBoundsReason);
        return;
    }
    const Mathematics::Vector3 defaults = NavigationSurface{}.*member;
    raw->SetDefaultValue(Rendering::Vector3(defaults.x, defaults.y, defaults.z));
    auto handlers = InspectorDrag::MakeComponentInteractiveHandlers<NavigationSurface, Rendering::Vector3>(
        ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, std::string("Change Navigation Surface ") + label,
        [member](NavigationSurface& target, const Rendering::Vector3& v) { target.*member = {v.x, v.y, v.z}; }, {});
    raw->SetOnValueChanging([preview = std::move(handlers.first)](const Rendering::Vector3& v) { preview(v); });
    raw->SetOnValueChanged([commit = std::move(handlers.second)](const Rendering::Vector3& v) { commit(v); });
}

void AddBoundsRows(const InspectorContext& ctx, ECS::ComponentTypeId typeId, const NavigationSurface& surface,
                   const SurfaceBox& box)
{
    RenderReflectedFieldRows(ctx, typeId, kFitBoundsField);
    Mathematics::Vector3 center = surface.Center;
    Mathematics::Vector3 extent = surface.Extent;
    if (surface.FitBounds)
    {
        // The fitted box in the fields' own terms: Center from the entity's position, Extent half the size.
        const auto* transform = ctx.World->GetComponent<Components::WorldTransform>(ctx.Entity);
        const Mathematics::Vector3 position = transform ? Mathematics::Vector3(transform->matrix[12], transform->matrix[13],
                                                                               transform->matrix[14])
                                                        : Mathematics::Vector3();
        center = box.Known ? (box.Min + box.Max) * 0.5f - position : Mathematics::Vector3();
        extent = box.Known ? (box.Max - box.Min) * 0.5f : Mathematics::Vector3();
    }
    AddBoundsVectorRow(ctx, typeId, "Center", "Center", center, &NavigationSurface::Center, surface.FitBounds);
    AddBoundsVectorRow(ctx, typeId, "Extent", "Extent", extent, &NavigationSurface::Extent, surface.FitBounds);
}

void AddBakeNotice(const InspectorContext& ctx, const NavigationSurface& surface, Label* cellSizeCaption,
                   const SurfaceBox& box)
{
    const auto kind = surface.BakePhase == NavigationBakePhase::Failed ? EditorUI::InspectorNotice::Kind::Error
                                                                       : EditorUI::InspectorNotice::Kind::Information;
    auto notice = std::make_unique<EditorUI::InspectorNotice>(Navigation::FormatBakeStatus(surface), kind);
    notice->SetId("navigation-surface-status");
    notice->SetAction("Bake",
                      "Bakes the surface again from the terrain, static collider shapes and mesh renderers inside its "
                      "bounds.",
                      [world = ctx.World, entity = ctx.Entity]
                      {
                          if (world)
                              Navigation::RequestBake(*world, entity);
                      });
    EditorUI::InspectorNotice* raw = notice.get();
    ctx.Parent->AddChild(std::move(notice));

    // The bake runs in NavigationSurfaceSystem; the notice and the header badge follow it. A change
    // to what the rows show rebuilds the inspector instead.
    if (!ctx.SimulationRefreshCallbacks)
        return;
    ctx.SimulationRefreshCallbacks->push_back(
        [raw, cellSizeCaption, box = box, world = ctx.World, entity = ctx.Entity, section = ctx.Section,
         refresh = ctx.RequestInspectorRefresh, built = LayoutOf(surface), requested = false]() mutable
        {
            const auto* current = world ? world->GetComponent<NavigationSurface>(entity) : nullptr;
            if (!current || requested)
                return;
            if (LayoutOf(*current) != built && refresh)
            {
                requested = true;
                refresh();
                return;
            }
            raw->SetText(Navigation::FormatBakeStatus(*current));
            // Explicit bounds are cheap to read again as they are edited; fitted ones keep the box
            // read when the inspector was built.
            if (!current->FitBounds)
                box = BoxOf(*world, entity);
            cellSizeCaption->SetText(CellSizeCaption(*current, box));
            if (section)
                SetBadge(*section, *current);
        });
}

void BuildNavigationSurfaceInspector(const InspectorContext& ctx)
{
    if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        return;
    const auto* current = ctx.World->GetComponent<NavigationSurface>(ctx.Entity);
    if (!current)
        return;
    const NavigationSurface surface = *current;
    const auto typeId = ECS::GetComponentTypeId<NavigationSurface>();

    const SurfaceBox box = BoxOf(*ctx.World, ctx.Entity);

    if (ctx.Section)
        SetBadge(*ctx.Section, surface);
    AddModeRow(ctx.Parent, ctx, typeId, surface.Mode);
    if (surface.Mode == NavigationSurfaceMode::Grid)
        RenderReflectedFieldRows(ctx, typeId, kGridTypeField);
    for (const AgentRow& row : kAgentRows)
        AddAgentRow(ctx, typeId, surface, row);
    // The caption sits in the value column under Cell Size.
    UIElement* captionCell = InspectorUI::AddActionRow(ctx.Parent, "navigation-surface-cell-size-caption");
    auto caption = std::make_unique<Label>();
    caption->AddClass("inspector-text");
    caption->SetText(CellSizeCaption(surface, box));
    Label* cellSizeCaption = caption.get();
    captionCell->AddChild(std::move(caption));
    AddBoundsRows(ctx, typeId, surface, box);
    if (surface.Mode == NavigationSurfaceMode::Grid)
        RenderReflectedFieldRows(ctx, typeId, kAuthoredCellsField);
    AddBakeNotice(ctx, surface, cellSizeCaption, box);
}

} // namespace

void RegisterNavigationSurfaceInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<NavigationSurface>(BuildNavigationSurfaceInspector);
    Editor::SceneTools::RegisterNavigationSurfaceGizmo();

    Editor::EditorComponentTraits traits;
    traits.DisplayName = "Navigation Surface";
    traits.InspectorCategory = "Navigation";
    traits.InspectorIconClass = "inspector-section-icon-navigation-surface";
    traits.InspectorTooltip = "A walkable surface for navigation agents, baked from the terrain, static collider shapes "
                              "and mesh renderers inside its bounds.";
    Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<NavigationSurface>(),
                                                          std::move(traits));
}

} // namespace GameEngine
