#include "Inspectors/Spline/SplinePlacementInspector.h"

#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/Spline/SplineInspector.h"
#include "Mathematics/Interpolation.h"
#include "UndoRedo/UndoRedoService.h"

#include "Assets/AssetRegistry.h"
#include "AssetCore/GUID.h"
#include "Components/Spline/SplinePlacement.h"
#include "Components/Spline/SplinePoolSelection.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{

using Components::kSplinePoolCapacity;
using Components::SplinePlacement;
using PoolArray = Components::ModelRef[kSplinePoolCapacity];

// Write one slot, then compact the pool so active entries stay a dense prefix.
// Compaction is what keeps the selection contract (leading non-empty slots)
// and the row layout in agreement after a clear — and it heals any hole a
// hand-edited scene carried the moment the pool is touched here.
void SetPoolSlotCompacted(PoolArray& pool, uint32 slot, const GUID& guid)
{
    if (slot >= kSplinePoolCapacity)
        return;
    pool[slot] = Components::ModelRef(guid);
    Components::ModelRef compacted[kSplinePoolCapacity];
    uint32 n = 0;
    for (const Components::ModelRef& m : pool)
    {
        if (!m.IsNull())
            compacted[n++] = m;
    }
    for (uint32 i = 0; i < kSplinePoolCapacity; ++i)
        pool[i] = i < n ? compacted[i] : Components::ModelRef{};
}

// The inspector rebuild event: pool row counts change with their content, so a
// commit re-renders this component's section (the controller needs no signal —
// it observes the recipe bytes each frame).
void NotifyInspectorRebuild(ECS::World* w, ECS::EntityHandle e,
                            Editor::EditorChangeNotifications* n)
{
    if (!n)
        return;
    Editor::EditorChangeNotifications::ComponentChangedEvent rebuild{};
    rebuild.world = w;
    rebuild.entity = e;
    rebuild.componentType = ECS::GetComponentTypeId<SplinePlacement>();
    rebuild.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
    n->NotifyComponentChanged(rebuild);
}

// Apply one mutation to the primary (with undo) and mirror it to every other
// selected entity carrying the component — the multi-selection contract the
// generic fallback provides, kept for this dedicated inspector.
template <typename ApplyFn>
void CommitToSelection(ECS::World* w, ECS::EntityHandle e,
                       const std::vector<ECS::EntityHandle>& extras,
                       Editor::EditorChangeNotifications* n, Editor::UndoRedoService* undo,
                       const std::string& name, ApplyFn&& apply)
{
    InspectorDrag::CommitComponentWithUndo<SplinePlacement>(w, e, n, undo, name, apply);
    for (ECS::EntityHandle ex : extras)
    {
        auto* c = w->GetComponent<SplinePlacement>(ex);
        if (!c)
            continue;
        SplinePlacement u = *c;
        apply(u);
        Editor::CommitComponentUpdate(w, ex, n, u);
    }
}

// Rows of Model asset fields for one pool: active entries, one add row while
// capacity remains, and validation lines — a full pool and a hole are reported
// by name, never silently dropped or reshuffled.
void BuildPoolRows(const InspectorContext& ctx, const std::string& poolLabel,
                   PoolArray SplinePlacement::* poolMember, const char* tooltip,
                   const std::vector<ECS::EntityHandle>& extras)
{
    auto* comp = ctx.World->GetComponent<SplinePlacement>(ctx.Entity);
    if (!comp)
        return;
    const PoolArray& pool = comp->*poolMember;
    const uint32 active = Components::ActivePoolCount(pool);
    const bool hole = Components::PoolHasHole(pool);

    // With a hole, show every slot so the ignored tail entries are visible and
    // fixable; otherwise the dense prefix plus one empty add row.
    const uint32 rows = hole ? kSplinePoolCapacity
                             : std::min(active + 1u, kSplinePoolCapacity);

    ECS::World* w = ctx.World;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    const std::string undoName = "Change " + poolLabel;

    for (uint32 slot = 0; slot < rows; ++slot)
    {
        InspectorUI::AddAssetFieldRow(
            ctx.Parent, poolLabel + " " + std::to_string(slot + 1), pool[slot].ToGuid(),
            {AssetType::Model}, &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
            [w, e, extras, n, undo, undoName, poolMember, slot](const GUID& picked)
            {
                CommitToSelection(w, e, extras, n, undo, undoName,
                                  [poolMember, slot, picked](SplinePlacement& u)
                                  { SetPoolSlotCompacted(u.*poolMember, slot, picked); });
                NotifyInspectorRebuild(w, e, n);
            },
            ctx.Thumbnails, tooltip);
    }

    if (active == kSplinePoolCapacity)
    {
        InspectorUI::AddInfoCard(ctx.Parent,
                                 poolLabel + " is full (" + std::to_string(kSplinePoolCapacity) +
                                     "/" + std::to_string(kSplinePoolCapacity) +
                                     "). Remove a mesh to add another.");
    }
    if (hole)
    {
        InspectorUI::AddInfoCard(ctx.Parent,
                                 poolLabel + " has an empty row before a mesh. The placement uses "
                                             "only the meshes above the first empty row. Set any "
                                             "row to close the gap.");
    }
}

// The mode fields, through the reflected rows: the enum control, display names and tooltips come
// from the component's field table.
constexpr std::array<std::string_view, 4> kModeFields = {"Fit", "ConformMode", "ConformTarget", "PlantMode"};

void BuildSplinePlacementInspector(const InspectorContext& context)
{
    using namespace Components;
    using namespace InspectorDrag;

    auto* comp = context.World ? context.World->GetComponent<SplinePlacement>(context.Entity) : nullptr;
    if (!comp)
        return;
    AddSplineRecipeBuildStateNotice(context);

    ECS::World* world = context.World;
    ECS::EntityHandle entity = context.Entity;
    Editor::EditorChangeNotifications* notifications = context.ChangeNotifications;
    Editor::UndoRedoService* undo = context.Undo;

    // Multi-selection: mirror edits to the other selected entities that
    // also carry the component (membership mirrors the generic fallback).
    std::vector<ECS::EntityHandle> extras;
    for (ECS::EntityHandle ex : GetAdditionalEntities(context))
    {
        if (world->GetComponent<SplinePlacement>(ex))
            extras.push_back(ex);
    }

    BuildPoolRows(context, "Straight", &SplinePlacement::StraightPool,
                  "Tile meshes placed along the spline. At each place the Seed picks one of "
                  "them, the same one on every rebuild. The meshes must have the same width "
                  "and the same pivot.",
                  extras);
    BuildPoolRows(context, "Curve", &SplinePlacement::CurvePool,
                  "Corner pieces for joints that bend past Seam Shear Max. The placement does "
                  "not place them yet: every joint shears.",
                  extras);
    BuildPoolRows(context, "Scatter", &SplinePlacement::ScatterPool,
                  "Accent meshes to scatter across the band. The placement does not place "
                  "them yet.",
                  extras);

    InspectorUI::AddAssetFieldRow(
        context.Parent, "Material Override", comp->OverrideMaterial.ToGuid(),
        {AssetType::Material}, &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
        [world, entity, extras, notifications, undo](const GUID& picked)
        {
            CommitToSelection(world, entity, extras, notifications, undo, "Change Material Override",
                              [picked](SplinePlacement& u)
                              { u.OverrideMaterial.Set(picked); });
        },
        context.Thumbnails,
        "Material applied to every placed tile. Empty keeps each mesh's own embedded "
        "model material, which on many kits is an untextured placeholder.");

    AddComponentFloatRowWithDrag<SplinePlacement>(
        context.Parent, "Spacing", comp->Spacing, world, entity, notifications, undo, "Change Spacing",
        [](SplinePlacement& u, float v)
        { u.Spacing = std::max(v, 0.05f); }, 2.0f,
        "Meters between tile centers along the draped centerline", extras, 0.05f);

    RenderReflectedFieldRows(context, ECS::GetComponentTypeId<SplinePlacement>(), kModeFields);

    AddComponentFloatRowWithDrag<SplinePlacement>(
        context.Parent, "Lateral Offset", comp->LateralOffset, world, entity, notifications, undo,
        "Change Lateral Offset",
        [](SplinePlacement& u, float v)
        { u.LateralOffset = v; }, 0.0f,
        "Meters to the right of travel; negative = left", extras);

    AddComponentFloatRowWithDrag<SplinePlacement>(
        context.Parent, "Slope Blend", comp->SlopeBlend, world, entity, notifications, undo, "Change Slope Blend",
        [](SplinePlacement& u, float v)
        { u.SlopeBlend = Math::Clamp01(v); },
        1.0f, "0 = tiles stay world-up, 1 = tiles align fully to the surface normal",
        extras, 0.0f, 1.0f);

    AddComponentFloatRowWithDrag<SplinePlacement>(
        context.Parent, "Max Tilt", comp->MaxTiltDegrees, world, entity, notifications, undo, "Change Max Tilt",
        [](SplinePlacement& u, float v)
        { u.MaxTiltDegrees = std::clamp(v, 0.0f, 90.0f); },
        Components::kDefaultSplineMaxTiltDegrees,
        "Degrees a tile may lean from upright. Ground steeper than this still sets the "
        "tile's height, but the tile holds this angle instead of standing on edge",
        extras, 0.0f, 90.0f);

    AddComponentIntRowWithDrag<SplinePlacement>(
        context.Parent, "Seed", static_cast<int>(comp->Seed), world, entity, notifications, undo, "Change Seed",
        [](SplinePlacement& u, int v)
        { u.Seed = static_cast<uint32>(std::max(v, 0)); },
        0,
        "Changes which Straight mesh each tile uses and every jitter below. The same "
        "seed gives the same layout on every rebuild",
        extras);

    AddComponentFloatRowWithDrag<SplinePlacement>(
        context.Parent, "Seam Shear Max", comp->SeamShearMaxDegrees, world, entity, notifications, undo,
        "Change Seam Shear Max",
        [](SplinePlacement& u, float v)
        { u.SeamShearMaxDegrees = std::max(v, 0.0f); },
        8.0f, "Cap in degrees on the per-tile skew that softens rigid seams; 0 disables",
        extras, 0.0f);

    // Per-station variation. Every one of these is deterministic in Seed
    // and defaults to a no-op, so an existing recipe lays out unchanged.
    AddComponentFloatRowWithDrag<SplinePlacement>(
        context.Parent, "Spacing Jitter", comp->SpacingJitterMetres, world, entity, notifications, undo,
        "Change Spacing Jitter",
        [](SplinePlacement& u, float v)
        { u.SpacingJitterMetres = std::max(v, 0.0f); },
        0.0f,
        "Meters a tile may slide along the path, either way. Breaks up the railway-sleeper "
        "look of an exact pitch. Capped at half the spacing, so tiles never reorder",
        extras, 0.0f);

    AddComponentFloatRowWithDrag<SplinePlacement>(
        context.Parent, "Yaw Jitter", comp->YawJitterDegrees, world, entity, notifications, undo,
        "Change Yaw Jitter",
        [](SplinePlacement& u, float v)
        { u.YawJitterDegrees = std::clamp(v, 0.0f, 180.0f); },
        0.0f, "Degrees a tile may turn on the spot, either way", extras, 0.0f, 180.0f);

    AddComponentFloatRowWithDrag<SplinePlacement>(
        context.Parent, "Lateral Jitter", comp->LateralJitterMetres, world, entity, notifications, undo,
        "Change Lateral Jitter",
        [](SplinePlacement& u, float v)
        { u.LateralJitterMetres = std::max(v, 0.0f); },
        0.0f, "Meters a tile may slide across the path, either way", extras, 0.0f);

    AddComponentFloatRowWithDrag<SplinePlacement>(
        context.Parent, "Dropout", comp->DropoutChance, world, entity, notifications, undo, "Change Dropout",
        [](SplinePlacement& u, float v)
        { u.DropoutChance = Math::Clamp01(v); },
        0.0f,
        "The fraction of tile places left empty. 0 places every tile, 1 places none. "
        "Gaps stop the path from looking like a manufactured strip",
        extras, 0.0f, 1.0f);

    AddComponentFloatRowWithDrag<SplinePlacement>(
        context.Parent, "End Taper", comp->EndTaperMetres, world, entity, notifications, undo, "Change End Taper",
        [](SplinePlacement& u, float v)
        { u.EndTaperMetres = std::max(v, 0.0f); }, 0.0f,
        "Meters at each end over which tiles thin out to nothing, so the path fades out "
        "instead of stopping square. 0 disables",
        extras, 0.0f);
}

} // namespace

void RegisterSplinePlacementInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<Components::SplinePlacement>(BuildSplinePlacementInspector);
}

} // namespace GameEngine
