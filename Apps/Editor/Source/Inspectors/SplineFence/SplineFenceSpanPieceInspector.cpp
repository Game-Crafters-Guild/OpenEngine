#include "Inspectors/SplineFence/SplineFenceSpanPieceInspector.h"

#include "Editor/Entities/EditorComponentTraits.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/SplineFence/SplineFenceOverrideFields.h"
#include "SplineRecipes/FenceRecipeLayout.h"
#include "SplineRecipes/FenceSpanOverrides.h"
#include "UI/Controls/InspectorNotice.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Spline/SplineFence.h"
#include "Components/Spline/SplineFenceSpanPiece.h"
#include "UI/Controls/Dropdown.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

using Components::SplineFence;
using Components::SplineFenceSpanPiece;
using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;

const char* UndoNameFor(SplineSpanOverrideKind kind)
{
    switch (kind)
    {
    case SplineSpanOverrideKind::Gate:
        return "Make Span a Gate";
    case SplineSpanOverrideKind::ExplicitPart:
        return "Set Explicit Span Part";
    case SplineSpanOverrideKind::None:
    default:
        return "Clear Span Override";
    }
}

// What the span becomes when the author picks a kind: a gate keeps the gate
// piece it had, or takes the first; an explicit piece keeps the piece the span
// draws now, so choosing it changes nothing on screen until another piece is
// chosen.
uint8 SlotForKind(SplineSpanOverrideKind kind, const SplineSpanOverride* current,
                  const SplineFenceSpanPiece& piece)
{
    if (current && current->Kind == kind)
        return current->PoolSlot;
    if (kind == SplineSpanOverrideKind::ExplicitPart && !piece.IsGate)
        return static_cast<uint8>(std::min<uint32>(piece.PoolSlot, Components::kSplineFenceMaxParts - 1u));
    return 0u;
}

// Sets what the span at `address` draws, as one undo step. The part slot follows the kind.
void CommitSpanKind(ECS::World* world, ECS::EntityHandle fenceEntity, Editor::EditorChangeNotifications* notifications,
                    Editor::UndoRedoService* undo, const SplineFenceSpanPiece& address, SplineSpanOverrideKind kind)
{
    const auto* recipe = world->GetComponent<SplineFence>(fenceEntity);
    if (!recipe)
        return;
    const SplineSpanOverride* existing = SplineRecipes::FindSpanOverride(*recipe, address.Run, address.OrdinalInRun);
    const uint8 slot = SlotForKind(kind, existing, address);
    InspectorDrag::CommitComponentWithUndo<SplineFence>(
        world, fenceEntity, notifications, undo, UndoNameFor(kind),
        [address, kind, slot](SplineFence& updated)
        { SplineRecipes::SetSpanOverride(updated, address.Run, address.OrdinalInRun, kind, slot); });
}

// The fence rebuilds a picked piece in place: a wall made a gate keeps its
// entity and takes the gate's name. The header shows the name the panel was
// built with, so the section asks for one rebuild when the name moves on.
void RefreshWhenTheNameChanges(const InspectorContext& ctx)
{
    if (!ctx.FrameRefreshCallbacks || !ctx.RequestInspectorRefresh || !ctx.World)
        return;
    const auto* name = ctx.World->GetComponent<Components::Name>(ctx.Entity);
    auto shown = std::make_shared<std::string>(name ? name->View() : std::string_view());
    auto requested = std::make_shared<bool>(false);
    UIElement* host = ctx.Parent;
    ECS::World* world = ctx.World;
    const ECS::EntityHandle entity = ctx.Entity;
    auto refresh = ctx.RequestInspectorRefresh;
    ctx.FrameRefreshCallbacks->push_back(
        [world, entity, shown, requested, host, refresh]()
        {
            if (*requested || !host || !world->IsValid(entity))
                return;
            const auto* current = world->GetComponent<Components::Name>(entity);
            if (!current || *shown == current->View())
                return;
            *requested = true;
            host->PostAction(refresh);
        });
}

void BuildSpanPieceSection(const InspectorContext& ctx)
{
    ECS::World* world = ctx.World;
    const ECS::EntityHandle pieceEntity = ctx.Entity;
    const auto* piece = world ? world->GetComponent<SplineFenceSpanPiece>(pieceEntity) : nullptr;
    if (!piece)
        return;
    const auto* parent = world->GetComponent<Components::Parent>(pieceEntity);
    const ECS::EntityHandle fenceEntity = parent ? parent->Entity : ECS::EntityHandle{};
    const SplineFence* fence =
        fenceEntity.IsValid() ? world->GetComponent<SplineFence>(fenceEntity) : nullptr;
    if (!fence)
    {
        InspectorUI::AddInfoCard(ctx.Parent, "This span is no longer under a fence.");
        return;
    }
    RefreshWhenTheNameChanges(ctx);

    InspectorUI::AddLine(ctx.Parent, "Span " + std::to_string(piece->OrdinalInRun) +
                                         " of the segment from point " + std::to_string(piece->Run) +
                                         ". What it uses is saved on the fence and survives every "
                                         "rebuild; the rest of this span is rebuilt from the fence.");

    const SplineSpanOverride* current =
        SplineRecipes::FindSpanOverride(*fence, piece->Run, piece->OrdinalInRun);
    const SplineSpanOverrideKind kind = current ? current->Kind : SplineSpanOverrideKind::None;

    Editor::EditorChangeNotifications* notifications = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    const SplineFenceSpanPiece address = *piece;
    const bool tableFull = !current && SplineRecipes::CountSpanOverrides(*fence) >=
                                           Components::kSplineFenceMaxSpanOverrides;

    std::vector<Dropdown::Option> kinds = Editor::SpanOverrideKindOptions("Any Span Part");
    const int kindIndex = Editor::SpanOverrideKindIndex(kinds, kind);
    Dropdown* kindField = InspectorUI::AddDropdownRow(
        ctx.Parent, "Use", kinds, kindIndex,
        "What this span draws. Any Span Part: the fence picks a Span part by Seed. Gate draws a Gate part "
        "and clears the crest parts over it. Explicit Part draws one Span part here instead of the part "
        "the Seed picks.");
    kindField->SetOnValueChanged(
        [world, fenceEntity, notifications, undo, address, kinds = std::move(kinds)](const std::string& value)
        {
            if (const auto nextKind = Editor::SpanOverrideKindOfOption(kinds, value))
                CommitSpanKind(world, fenceEntity, notifications, undo, address, *nextKind);
        });
    if (tableFull)
    {
        kindField->SetDisabled(true);
        InspectorUI::AddInfoCard(
            ctx.Parent, "The fence already holds " +
                            std::to_string(Components::kSplineFenceMaxSpanOverrides) +
                            " span overrides; remove one in the fence's Span Overrides to set "
                            "another here.");
    }

    if (kind == SplineSpanOverrideKind::None)
    {
        if (SplineRecipes::FencePoolOf(*fence, Components::SplineFencePartKind::Gate).Count == 0u)
        {
            InspectorUI::AddInfoCard(ctx.Parent,
                                     "The fence has no Gate part with a mesh: a Gate here would leave an "
                                     "opening, which cannot be selected in the scene view again.");
        }
        return;
    }

    const bool gate = kind == SplineSpanOverrideKind::Gate;
    Editor::AddOverridePartRow(
        ctx.Parent, gate ? "Gate Part" : "Span Part", *fence,
        gate ? Components::SplineFencePartKind::Gate : Components::SplineFencePartKind::Span, current->PoolSlot,
        gate ? "The Gate part that stands in this span, from the fence's Parts list"
             : "The Span part that this span keeps, from the fence's Parts list",
        [world, fenceEntity, notifications, undo, address, kind](uint8 slot)
        {
            InspectorDrag::CommitComponentWithUndo<SplineFence>(
                world, fenceEntity, notifications, undo,
                kind == SplineSpanOverrideKind::Gate ? "Change Gate Part" : "Change Explicit Span Part",
                [address, kind, slot](SplineFence& updated)
                { SplineRecipes::SetSpanOverride(updated, address.Run, address.OrdinalInRun, kind, slot); });
        });
    // The selected span exists, so the problem needs no data from the last build.
    const std::string problem =
        Editor::SpanOverrideProblem(*fence, static_cast<uint32>(current - fence->Overrides), Editor::FenceBuildState{});
    if (!problem.empty())
        InspectorUI::AddNotice(ctx.Parent, problem, EditorUI::InspectorNotice::Kind::Warning);
}

} // namespace

void RegisterSplineFenceSpanPieceInspector()
{
    // The section is the one thing on a generated piece an author edits, so it
    // leads the inspector rather than sitting under the long Material section.
    Editor::EditorComponentTraits traits;
    traits.LeadsInspector = true;
    Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<SplineFenceSpanPiece>(),
                                                          std::move(traits));
    InspectorRegistry::Get().RegisterComponentInspector<SplineFenceSpanPiece>(
        [](const InspectorContext& ctx) { BuildSpanPieceSection(ctx); });
}

} // namespace GameEngine
