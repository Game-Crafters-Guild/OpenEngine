#include "Inspectors/NavigationAgentInspector.h"

#include "Editor/Entities/EditorComponentTraits.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/InspectorNotice.h"

#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Components/Navigation/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "Components/Navigation/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationScriptApi.h"

#include <array>
#include <memory>
#include <string_view>

namespace GameEngine
{
namespace
{

using Components::NavigationAgent;
using InspectorDrag::CommitComponentWithUndo;

constexpr std::array<std::string_view, 9> kMovementFields = {
    "Speed",       "Acceleration",     "StoppingDistance", "Radius",        "Height",
    "SmoothPaths", "AvoidanceQuality", "SeparationWeight", "ReplanInterval",
};

// Failed orders read as errors, orders held back by a reason as warnings, the rest as information.
EditorUI::InspectorNotice::Kind NoticeKindOf(const Navigation::AgentStatus& status)
{
    if (status.State == Navigation::AgentState::Failed)
        return EditorUI::InspectorNotice::Kind::Error;
    if (status.Reason != Navigation::StatusReason::None)
        return EditorUI::InspectorNotice::Kind::Warning;
    return EditorUI::InspectorNotice::Kind::Information;
}

void AddSurfaceRow(const InspectorContext& ctx, const NavigationAgent& agent)
{
    const ECS::FieldInfo* field =
        ECS::ComponentFieldRegistry::FindField(ECS::GetComponentTypeId<NavigationAgent>(), "Surface");
    EntityField* surface = InspectorUI::AddEntityFieldRow(
        ctx.Parent, "Surface", agent.Surface, ctx.World,
        [w = ctx.World, e = ctx.Entity, n = ctx.ChangeNotifications, undo = ctx.Undo](ECS::EntityHandle picked)
        {
            CommitComponentWithUndo<NavigationAgent>(w, e, n, undo, "Change Navigation Agent Surface",
                                                     [picked](NavigationAgent& target) { target.Surface = picked; });
        },
        {ECS::GetComponentTypeId<Components::NavigationSurface>()},
        field && !field->Tooltip.empty() ? field->Tooltip.data() : nullptr);
    surface->SetEmptyText("The only surface in the scene");
}

// The order's status line. It follows the agent while the simulation runs; a change of its kind
// rebuilds the inspector, which draws the notice in the new kind.
void AddStatusNotice(const InspectorContext& ctx)
{
    const Navigation::AgentStatus status = Navigation::GetStatus(*ctx.World, ctx.Entity);
    auto notice = std::make_unique<EditorUI::InspectorNotice>(Navigation::FormatAgentStatus(status), NoticeKindOf(status));
    notice->SetId("navigation-agent-status");
    EditorUI::InspectorNotice* raw = notice.get();
    ctx.Parent->AddChild(std::move(notice));

    if (!ctx.SimulationRefreshCallbacks)
        return;
    ctx.SimulationRefreshCallbacks->push_back(
        [raw, world = ctx.World, entity = ctx.Entity, refresh = ctx.RequestInspectorRefresh,
         built = NoticeKindOf(status), requested = false]() mutable
        {
            if (!world || requested || !world->HasComponent<NavigationAgent>(entity))
                return;
            const Navigation::AgentStatus current = Navigation::GetStatus(*world, entity);
            if (NoticeKindOf(current) != built && refresh)
            {
                requested = true;
                refresh();
                return;
            }
            raw->SetText(Navigation::FormatAgentStatus(current));
        });
}

void BuildNavigationAgentInspector(const InspectorContext& ctx)
{
    if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        return;
    const auto* agent = ctx.World->GetComponent<NavigationAgent>(ctx.Entity);
    if (!agent)
        return;
    AddSurfaceRow(ctx, *agent);
    RenderReflectedFieldRows(ctx, ECS::GetComponentTypeId<NavigationAgent>(), kMovementFields);
    AddStatusNotice(ctx);
}

} // namespace

void RegisterNavigationAgentInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<NavigationAgent>(BuildNavigationAgentInspector);

    Editor::EditorComponentTraits traits;
    traits.DisplayName = "Navigation Agent";
    traits.InspectorCategory = "Navigation";
    traits.InspectorTooltip = "Walks a navigation surface to the destination a game or a script gives it. A physics "
                              "body on it must be kinematic.";
    Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<NavigationAgent>(),
                                                          std::move(traits));

    // The order's runtime state shows as the agent's status line, not as a section of its own.
    Editor::EditorComponentTraits state;
    state.HideInInspector = true;
    Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::NavigationAgentState>(),
                                                          std::move(state));
}

} // namespace GameEngine
