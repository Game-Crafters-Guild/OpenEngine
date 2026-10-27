#include "Inspectors/SplineFence/SplineFenceInspector.h"

#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/SplineFence/SplineFenceOverrideFields.h"
#include "Inspectors/SplineFence/SplineFenceOverrideRow.h"
#include "Inspectors/SplineFence/SplineFencePartRow.h"
#include "Inspectors/SplineFence/SplineFenceInspectorState.h"
#include "Inspectors/SplineFence/SplineFencePreview.h"
#include "Inspectors/Spline/SplineInspector.h"
#include "Inspectors/SplineRecipe/SplineRecipeStateNotice.h"
#include "Mathematics/Interpolation.h"
#include "SplineRecipes/FenceRecipeLayout.h"
#include "SplineRecipes/FenceSpanOverrides.h"
#include "SplineRecipes/RecipeControllers.h"
#include "SplineRecipes/SplineRecipeSystem.h"
#include "SplineRecipes/SplineRecipesScriptApi.h"
#include "Types/FormatNumber.h"
#include "UI/Controls/InspectorActionButton.h"
#include "UndoRedo/UndoRedoService.h"

#include "Assets/AssetRegistry.h"
#include "AssetCore/GUID.h"
#include "Components/Hierarchy.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineFence.h"
#include "Components/Spline/SplineFenceSpanPiece.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Systems.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "UI/Controls/Dropdown.h"

#include <algorithm>
#include <array>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

using Components::kSplineFenceMaxParts;
using Components::kSplineFenceMaxSpanOverrides;
using Components::SplineFence;
using Components::SplineFencePart;
using Components::SplineFencePartKind;
using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;

// The fence's mode fields, through the reflected rows: the enum control, display names and tooltips
// come from the component's field table.
constexpr std::array<std::string_view, 4> kModeFields = {"ConformMode", "ConformTarget", "PlantMode", "SpanGrade"};

// Under Pivot Plane, a part with more than this share of its height below its pivot sinks deeper than
// a planting skirt: the measured fence kit has 32 percent of its panel below its pivot.
constexpr float32 kBuriedShareForWarning = 0.4f;
constexpr int kBuriedDepthDecimals = 1;

// Moves the listed parts to the front of the list, in order, so that row n of the inspector is
// slot n. A scene can hold an empty slot between two parts.
void CompactParts(SplineFence& fence)
{
    uint32 count = 0;
    for (const SplineFencePart& part : fence.Parts)
    {
        if (part.Kind != SplineFencePartKind::None)
            fence.Parts[count++] = part;
    }
    for (uint32 i = count; i < kSplineFenceMaxParts; ++i)
        fence.Parts[i] = SplineFencePart{};
}

uint32 PartCount(const SplineFence& fence)
{
    return static_cast<uint32>(std::count_if(std::begin(fence.Parts), std::end(fence.Parts),
                                             [](const SplineFencePart& part)
                                             { return part.Kind != SplineFencePartKind::None; }));
}

using SplineRecipes::kNoPartRow;
using SplineRecipes::PartRowMoves;

PartRowMoves UnmovedRows()
{
    PartRowMoves rows{};
    std::iota(rows.begin(), rows.end(), 0u);
    return rows;
}

// A new part takes the kind of the last one, because a second variant of a role is the common
// addition; a fence with no part gets a Span.
void AddPart(SplineFence& fence)
{
    CompactParts(fence);
    const uint32 count = PartCount(fence);
    if (count >= kSplineFenceMaxParts)
        return;
    fence.Parts[count].Kind = count > 0u ? fence.Parts[count - 1u].Kind : SplineFencePartKind::Span;
}

// Each edit below keeps the span overrides on the parts they draw.
void RemovePart(SplineFence& fence, uint32 row)
{
    CompactParts(fence);
    if (row >= kSplineFenceMaxParts)
        return;
    const SplineRecipes::SpanOverridePartRows drawn = SplineRecipes::PartRowsOfSpanOverrides(fence);
    PartRowMoves moved = UnmovedRows();
    moved[row] = kNoPartRow;
    for (uint32 later = row + 1u; later < kSplineFenceMaxParts; ++later)
        moved[later] = later - 1u;
    fence.Parts[row] = SplineFencePart{};
    CompactParts(fence);
    SplineRecipes::KeepSpanOverridesOnTheirParts(fence, drawn, moved);
}

void SwapParts(SplineFence& fence, uint32 row, uint32 otherRow)
{
    CompactParts(fence);
    const uint32 count = PartCount(fence);
    if (row >= count || otherRow >= count)
        return;
    const SplineRecipes::SpanOverridePartRows drawn = SplineRecipes::PartRowsOfSpanOverrides(fence);
    PartRowMoves moved = UnmovedRows();
    std::swap(moved[row], moved[otherRow]);
    std::swap(fence.Parts[row], fence.Parts[otherRow]);
    SplineRecipes::KeepSpanOverridesOnTheirParts(fence, drawn, moved);
}

void SetPartKind(SplineFence& fence, uint32 row, SplineFencePartKind kind)
{
    CompactParts(fence);
    if (row >= PartCount(fence))
        return;
    const SplineRecipes::SpanOverridePartRows drawn = SplineRecipes::PartRowsOfSpanOverrides(fence);
    fence.Parts[row].Kind = kind;
    SplineRecipes::KeepSpanOverridesOnTheirParts(fence, drawn, UnmovedRows());
}

void SetPartMesh(SplineFence& fence, uint32 row, const GUID& mesh)
{
    CompactParts(fence);
    if (row >= PartCount(fence))
        return;
    const SplineRecipes::SpanOverridePartRows drawn = SplineRecipes::PartRowsOfSpanOverrides(fence);
    fence.Parts[row].Mesh = Components::ModelRef(mesh);
    SplineRecipes::KeepSpanOverridesOnTheirParts(fence, drawn, UnmovedRows());
}

bool HasPartOfKind(const SplineFence& fence, SplineFencePartKind kind)
{
    return std::any_of(std::begin(fence.Parts), std::end(fence.Parts),
                       [kind](const SplineFencePart& part) { return part.Kind == kind; });
}

// The part rows change with the list, so a commit re-renders this
// component's section. The controller needs no signal — it observes the recipe
// bytes each frame.
void NotifyInspectorRebuild(ECS::World* w, ECS::EntityHandle e,
                            Editor::EditorChangeNotifications* n)
{
    if (!n)
        return;
    Editor::EditorChangeNotifications::ComponentChangedEvent rebuild{};
    rebuild.world = w;
    rebuild.entity = e;
    rebuild.componentType = ECS::GetComponentTypeId<SplineFence>();
    rebuild.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
    n->NotifyComponentChanged(rebuild);
}

// Commits one edit of a list, then rebuilds the section: the rows follow the list.
template <typename EditFn>
void CommitListEdit(const InspectorContext& ctx, const char* undoName, EditFn edit)
{
    InspectorDrag::CommitComponentWithUndo<SplineFence>(ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo,
                                                        undoName, std::move(edit));
    NotifyInspectorRebuild(ctx.World, ctx.Entity, ctx.ChangeNotifications);
}

// Sets one field of the override at `row`.
template <typename FieldT>
void SetOverrideField(SplineFence& fence, uint32 row, FieldT SplineSpanOverride::*field, FieldT value)
{
    SplineRecipes::CompactSpanOverrides(fence);
    if (row < SplineRecipes::CountSpanOverrides(fence))
        fence.Overrides[row].*field = value;
}

// One row of the override list: each field and action edits the override at `row`.
void AddOverrideRow(const InspectorContext& ctx, const SplineFence& fence, uint32 row, uint32 count,
                    const std::string& problem)
{
    EditorUI::SplineFenceOverrideRow::Handlers handlers;
    handlers.KindChanged = [ctx, row](SplineSpanOverrideKind kind)
    { CommitListEdit(ctx, "Change Span Override", [row, kind](SplineFence& edited) { SplineRecipes::SetSpanOverrideKind(edited, row, kind); }); };
    handlers.PointChanged = [ctx, row](uint32 point)
    {
        CommitListEdit(ctx, "Change Span Override Point",
                       [row, point](SplineFence& edited) { SetOverrideField(edited, row, &SplineSpanOverride::PointIndex, point); });
    };
    handlers.SpanChanged = [ctx, row](uint16 span)
    {
        CommitListEdit(ctx, "Change Span Override Span",
                       [row, span](SplineFence& edited) { SetOverrideField(edited, row, &SplineSpanOverride::SpanOrdinal, span); });
    };
    handlers.PartChanged = [ctx, row](uint8 slot)
    {
        CommitListEdit(ctx, "Change Span Override Part",
                       [row, slot](SplineFence& edited) { SetOverrideField(edited, row, &SplineSpanOverride::PoolSlot, slot); });
    };
    handlers.Actions.MoveUp = [ctx, row]
    { CommitListEdit(ctx, "Move Span Override", [row](SplineFence& edited) { SplineRecipes::SwapSpanOverrides(edited, row, row - 1u); }); };
    handlers.Actions.MoveDown = [ctx, row]
    { CommitListEdit(ctx, "Move Span Override", [row](SplineFence& edited) { SplineRecipes::SwapSpanOverrides(edited, row, row + 1u); }); };
    handlers.Actions.Remove = [ctx, row]
    { CommitListEdit(ctx, "Remove Span Override", [row](SplineFence& edited) { SplineRecipes::RemoveSpanOverride(edited, row); }); };
    InspectorUI::AddControl(ctx.Parent, std::make_unique<EditorUI::SplineFenceOverrideRow>(
                                            fence, row, row == 0u, row + 1u == count, problem, std::move(handlers)));
}

// One row per part: its kind, its mesh and the list actions edit the part at `row` of the list.
void AddPartRow(const InspectorContext& ctx, const SplineFencePart& part, uint32 row, uint32 count)
{
    EditorUI::SplineFencePartRow::Handlers handlers;
    handlers.KindChanged = [ctx, row](SplineFencePartKind kind)
    { CommitListEdit(ctx, "Change Fence Part", [row, kind](SplineFence& edited) { SetPartKind(edited, row, kind); }); };
    handlers.MeshChanged = [ctx, row](const GUID& picked)
    { CommitListEdit(ctx, "Change Fence Part Mesh", [row, picked](SplineFence& edited) { SetPartMesh(edited, row, picked); }); };
    handlers.Actions.MoveUp = [ctx, row]
    { CommitListEdit(ctx, "Move Fence Part", [row](SplineFence& edited) { SwapParts(edited, row, row - 1u); }); };
    handlers.Actions.MoveDown = [ctx, row]
    { CommitListEdit(ctx, "Move Fence Part", [row](SplineFence& edited) { SwapParts(edited, row, row + 1u); }); };
    handlers.Actions.Remove = [ctx, row]
    { CommitListEdit(ctx, "Remove Fence Part", [row](SplineFence& edited) { RemovePart(edited, row); }); };
    InspectorUI::AddControl(ctx.Parent, std::make_unique<EditorUI::SplineFencePartRow>(
                                            part, row == 0u, row + 1u == count, ctx.Thumbnails, std::move(handlers)));
}

// The Parts list: a row per part and an Add Part action. The parts of one kind are that role's
// pool, in list order.
void BuildPartRows(const InspectorContext& ctx, const SplineFence& fence)
{
    InspectorUI::AddLine(ctx.Parent, "Parts");
    SplineFence listed = fence;
    CompactParts(listed);
    const uint32 count = PartCount(listed);
    for (uint32 row = 0; row < count; ++row)
        AddPartRow(ctx, listed.Parts[row], row, count);

    UIElement* addCell = InspectorUI::AddActionRow(ctx.Parent, "spline-fence-add-part-row");
    auto add = std::make_unique<EditorUI::InspectorActionButton>("Add Part", "inspector-action-add");
    add->SetId("spline-fence-add-part");
    if (count >= kSplineFenceMaxParts)
    {
        add->SetEnabled(false);
        add->SetTooltip("The fence holds " + std::to_string(kSplineFenceMaxParts) +
                        " parts, the most it can hold. Remove a part to add another.");
    }
    else
    {
        add->SetTooltip("Adds a part of the same kind as the last one. Change its kind in the row.");
    }
    add->SetOnClick([ctx](UIEvent&) { CommitListEdit(ctx, "Add Fence Part", [](SplineFence& edited) { AddPart(edited); }); });
    InspectorUI::AddControl(addCell, std::move(add));
}

// The spans a fence's own pieces stand in, as (point, ordinal): an override
// naming a span no piece stands in places nothing, and its row says so. Read
// once per inspector build from the pieces the fence placed.
std::vector<std::pair<uint32, uint32>> StandingSpans(ECS::World& world, ECS::EntityHandle fence)
{
    std::vector<std::pair<uint32, uint32>> spans;
    world.Query<ECS::Read<Components::SplineFenceSpanPiece>, ECS::Read<Components::Parent>>().Each(
        [&spans, fence](const Components::SplineFenceSpanPiece& piece, const Components::Parent& parent)
        {
            if (parent.Entity == fence)
                spans.emplace_back(piece.Run, piece.OrdinalInRun);
        });
    return spans;
}

// The segments of the spline on `entity`, when its data is loaded.
std::optional<uint32> SegmentCountOf(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* spline = world.GetComponent<Components::SplineComponent>(entity);
    SplineECS::SplineService* service = SplineECS::SplineService::TryGet();
    if (!spline || !service)
        return std::nullopt;
    const Spline::SplineData* data =
        service->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
    if (!data)
        return std::nullopt;
    return data->GetSegmentCount();
}

// What the fence's last build shows. A fence that waits or failed placed no spans to count.
Editor::FenceBuildState BuildStateOf(const InspectorContext& ctx, SplineRecipes::BuildState state,
                                     std::string_view reason, std::span<const std::pair<uint32, uint32>> standing)
{
    Editor::FenceBuildState build;
    build.Built = state == SplineRecipes::BuildState::Built && reason.empty();
    build.StandingSpans = standing;
    build.SegmentCount = SegmentCountOf(*ctx.World, ctx.Entity);
    return build;
}

// The warning of part row `row` of `listed` when Pivot Plane sinks its measured mesh deeper than a
// planting skirt, or an empty string. A crest or a cap sits on the spans, not on the ground.
std::string BuriedPartWarning(const SplineFence& listed, uint32 row,
                              std::span<const SplineRecipes::FenceMeshBounds> measured)
{
    const SplineFencePart& part = listed.Parts[row];
    if (listed.PlantMode != Components::SplinePlantMode::PivotPlane || part.Mesh.IsNull() ||
        part.Kind == SplineFencePartKind::Crest || part.Kind == SplineFencePartKind::Cap)
        return {};
    const auto found = std::find_if(measured.begin(), measured.end(), [&part](const SplineRecipes::FenceMeshBounds& entry)
                                    { return entry.Mesh == part.Mesh; });
    if (found == measured.end())
        return {};
    const float32 depth = found->Bounds.SkirtDepth();
    if (depth <= kBuriedShareForWarning * found->Bounds.Height())
        return {};
    return "Part " + std::to_string(row + 1u) + ", a " + Editor::FencePartKindName(part.Kind) + " part, reaches " +
           FormatMeters(depth, kBuriedDepthDecimals) +
           " below the ground line (Plant Mode: Pivot Plane). Set Plant Mode to Bounds Min to stand it on the ground.";
}

// The problems of the fence's rows that a Built state names: a part with no mesh, a part that sinks
// into the ground, and each override that cannot do what it says. Each names its row.
std::vector<std::string> FenceWarnings(const SplineFence& fence, const Editor::FenceBuildState& build,
                                       std::span<const SplineRecipes::FenceMeshBounds> measured)
{
    std::vector<std::string> warnings;
    SplineFence listed = fence;
    CompactParts(listed);
    for (uint32 row = 0; row < PartCount(listed); ++row)
    {
        if (listed.Parts[row].Mesh.IsNull())
            warnings.push_back("Part " + std::to_string(row + 1u) + ", a " + Editor::FencePartKindName(listed.Parts[row].Kind) +
                               " part, has no mesh, so it places nothing. Give it a mesh, or remove it.");
        std::string buried = BuriedPartWarning(listed, row, measured);
        if (!buried.empty())
            warnings.push_back(std::move(buried));
    }
    SplineRecipes::CompactSpanOverrides(listed);
    for (uint32 slot = 0; slot < SplineRecipes::CountSpanOverrides(listed); ++slot)
    {
        std::string problem = Editor::SpanOverrideProblem(listed, slot, build);
        if (!problem.empty())
            warnings.push_back(std::move(problem));
    }
    return warnings;
}

// One row per override, in slot order, then an Add Override action. Overrides are also made by
// selecting a span in the scene view (Inspectors/SplineFence/SplineFenceSpanPieceInspector.cpp); this list is
// where the author sees them all and corrects one after the spline is edited.
void BuildOverrideRows(const InspectorContext& ctx, const SplineFence& fence, const Editor::FenceBuildState& build)
{
    InspectorUI::AddLine(ctx.Parent, "Span Overrides");
    SplineFence listed = fence;
    SplineRecipes::CompactSpanOverrides(listed);
    const uint32 count = SplineRecipes::CountSpanOverrides(listed);
    if (count == 0u)
    {
        InspectorUI::AddLine(ctx.Parent, "None. Select a span in the scene view to make it a gate or to give it "
                                         "an explicit part, or use Add Override.");
    }

    for (uint32 slot = 0; slot < count; ++slot)
        AddOverrideRow(ctx, listed, slot, count, Editor::SpanOverrideProblem(listed, slot, build));

    UIElement* addCell = InspectorUI::AddActionRow(ctx.Parent, "spline-fence-add-override-row");
    auto add = std::make_unique<EditorUI::InspectorActionButton>("Add Override", "inspector-action-add");
    add->SetId("spline-fence-add-override");
    if (count >= kSplineFenceMaxSpanOverrides)
    {
        add->SetEnabled(false);
        add->SetTooltip("The fence holds " + std::to_string(kSplineFenceMaxSpanOverrides) +
                        " span overrides, the most it can hold. Remove one to add another.");
    }
    else
    {
        add->SetTooltip("Adds an override like the last one, on the next span of the fence that has none. "
                        "Change its point and span in the row.");
    }
    std::vector<std::pair<uint32, uint32>> standing;
    if (build.Built)
        standing.assign(build.StandingSpans.begin(), build.StandingSpans.end());
    add->SetOnClick(
        [ctx, standing = std::move(standing)](UIEvent&)
        {
            CommitListEdit(ctx, "Add Span Override",
                           [&standing](SplineFence& edited) { SplineRecipes::AddSpanOverride(edited, standing); });
        });
    InspectorUI::AddControl(addCell, std::move(add));
}

// The fence's part bounds as the running recipe system's last rebuild resolved them; empty in a
// process with no recipe system.
std::span<const SplineRecipes::FenceMeshBounds> RunningFencePartBounds(ECS::EntityHandle entity)
{
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    auto* systems = loop ? loop->GetSystemManager() : nullptr;
    const auto* recipes = systems ? systems->GetSystem<SplineRecipes::SplineRecipeSystem>() : nullptr;
    return recipes ? recipes->GetControllers().FencePartBounds(entity) : std::span<const SplineRecipes::FenceMeshBounds>();
}

void FollowFence(EditorUI::SplineFencePreview& preview, ECS::World& world, ECS::EntityHandle entity,
                 std::span<const std::pair<uint32, uint32>> unplacedGates)
{
    if (const auto* fence = world.GetComponent<SplineFence>(entity))
        preview.SetFence(*fence, RunningFencePartBounds(entity), unplacedGates);
}

// The (point, span) of each gate override of `fence` that places nothing in the last build.
std::vector<std::pair<uint32, uint32>> UnplacedGates(const SplineFence& fence, const Editor::FenceBuildState& build)
{
    std::vector<std::pair<uint32, uint32>> gates;
    for (uint32 slot = 0; slot < kSplineFenceMaxSpanOverrides; ++slot)
    {
        const SplineSpanOverride& entry = fence.Overrides[slot];
        if (entry.Kind == SplineSpanOverrideKind::Gate && Editor::SpanOverrideMissesItsSpan(fence, slot, build))
            gates.emplace_back(entry.PointIndex, entry.SpanOrdinal);
    }
    return gates;
}

// The preview strip under the build notice. It reads the recipe every frame, so it redraws in the
// frame after an edit, during a drag too. It draws each gate in `unplacedGates` as not placed, as the
// notice's warnings name them.
void AddFencePreview(const InspectorContext& context, std::vector<std::pair<uint32, uint32>> unplacedGates)
{
    EditorUI::SplineFencePreview* preview =
        InspectorUI::AddPreviewRow(context.Parent, EditorUI::SplineFencePreview::Create());
    if (!preview)
        return;
    ECS::World* world = context.World;
    const ECS::EntityHandle entity = context.Entity;
    FollowFence(*preview, *world, entity, unplacedGates);
    if (context.FrameRefreshCallbacks)
    {
        context.FrameRefreshCallbacks->push_back([preview, world, entity, gates = std::move(unplacedGates)]()
                                                 { FollowFence(*preview, *world, entity, gates); });
    }
}

void BuildSplineFenceInspector(const InspectorContext& context)
{
    if (!context.World)
        return;
    BuildSplineFenceInspectorInState(context, SplineRecipes::StateOf(*context.World, context.Entity).State,
                                     SplineRecipes::ReasonOf(*context.World, context.Entity),
                                     RunningFencePartBounds(context.Entity));
}

} // namespace

void BuildSplineFenceInspectorInState(const InspectorContext& context, SplineRecipes::BuildState state,
                                      std::string_view reason,
                                      std::span<const SplineRecipes::FenceMeshBounds> measured)
{
    using namespace Components;
    using namespace InspectorDrag;

    auto* comp = context.World ? context.World->GetComponent<SplineFence>(context.Entity) : nullptr;
    if (!comp)
        return;
    const std::vector<std::pair<uint32, uint32>> standing = StandingSpans(*context.World, context.Entity);
    const Editor::FenceBuildState build = BuildStateOf(context, state, reason, standing);
    AddBuildStateNotice(context, state, reason, FenceWarnings(*comp, build, measured));
    AddFencePreview(context, UnplacedGates(*comp, build));

    ECS::World* world = context.World;
    ECS::EntityHandle entity = context.Entity;
    Editor::EditorChangeNotifications* notifications = context.ChangeNotifications;
    Editor::UndoRedoService* undo = context.Undo;

    BuildPartRows(context, *comp);

    if (HasPartOfKind(*comp, SplineFencePartKind::Crest))
    {
        AddComponentFloatRowWithDrag<SplineFence>(
            context.Parent, "Crest Pitch", comp->CrestPitch, world, entity, notifications, undo,
            "Change Crest Pitch", [](SplineFence& u, float v) { u.CrestPitch = std::max(v, 0.0f); }, 0.0f,
            "The distance between crest parts, in meters. 0 uses the length of the longest crest part, so "
            "the parts meet end to end. A smaller value is raised to that length.",
            {}, 0.0f);
    }

    InspectorUI::AddAssetFieldRow(
        context.Parent, "Material Override", comp->OverrideMaterial.ToGuid(),
        {AssetType::Material}, &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
        [world, entity, notifications, undo](const GUID& picked)
        {
            InspectorDrag::CommitComponentWithUndo<SplineFence>(
                world, entity, notifications, undo, "Change Material Override",
                [picked](SplineFence& u)
                { u.OverrideMaterial.Set(picked); });
        },
        context.Thumbnails,
        "Material applied to every part the fence places: posts, spans, crest parts and "
        "caps. Empty keeps each mesh's own embedded model material, which on many kits is "
        "an untextured placeholder.");

    AddComponentFloatRowWithDrag<SplineFence>(
        context.Parent, "Span Length", comp->PostPitch, world, entity, notifications, undo, "Change Span Length",
        [](SplineFence& u, float v)
        { u.PostPitch = std::max(v, 0.05f); }, 2.4f,
        "The length of each span along the ground, in meters, which is also the distance between posts. "
        "Between two points of the spline the fence changes it a little, so a span ends on each point.",
        {}, 0.05f);

    RenderReflectedFieldRows(context, ECS::GetComponentTypeId<SplineFence>(), kModeFields);

    AddComponentFloatRowWithDrag<SplineFence>(
        context.Parent, "Slope Blend", comp->SlopeBlend, world, entity, notifications, undo, "Change Slope Blend",
        [](SplineFence& u, float v)
        { u.SlopeBlend = Math::Clamp01(v); }, 0.0f,
        "0 = plumb posts, which is what a real fence does; 1 = posts lean into the slope",
        {}, 0.0f, 1.0f);

    AddComponentFloatRowWithDrag<SplineFence>(
        context.Parent, "Span Max Stretch", comp->SpanMaxStretch, world, entity, notifications, undo,
        "Change Span Max Stretch",
        [](SplineFence& u, float v)
        { u.SpanMaxStretch = std::max(v, 1.0f); }, 1.25f,
        "How much a span can stretch before the fence adds or removes a span between two points of the "
        "spline. A stretch that no span count can keep under this is reported, not hidden.",
        {}, 1.0f);

    AddComponentIntRowWithDrag<SplineFence>(
        context.Parent, "Seed", static_cast<int>(comp->Seed), world, entity, notifications, undo, "Change Seed",
        [](SplineFence& u, int v)
        { u.Seed = static_cast<uint32>(std::max(v, 0)); }, 0,
        "Changes which part of each kind the fence picks at each post, span and crest place. The same "
        "seed gives the same picks on every rebuild. Caps are chosen by length, not by the seed.");

    BuildOverrideRows(context, *comp, build);
}

void RegisterSplineFenceInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<Components::SplineFence>(BuildSplineFenceInspector);
}

} // namespace GameEngine
