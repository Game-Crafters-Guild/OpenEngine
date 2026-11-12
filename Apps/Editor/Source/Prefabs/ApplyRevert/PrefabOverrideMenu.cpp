#include "Prefabs/PrefabInstanceDescriptions.h"

#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Prefabs/ApplyRevert/ApplyPrefabOverrideCommand.h"
#include "Prefabs/PrefabHierarchyRows.h"
#include "Prefabs/PrefabOverrideRecordsCommand.h"
#include "SceneView/SceneViewNoticeOverlay.h"
#include "UI/EditorIcons.h"
#include "UndoRedo/EntityComponentsEdit.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Scene/PrefabAuthoring.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kRevertFieldLabel = "Revert to prefab value";
// Apply and Revert add no record.
constexpr std::span<const Scene::PrefabOverrideTarget> kNoTargets;

// The overrides of the target's entity that its row or header shows: the field's lines for a row, the
// component's addition or removal for a header.
std::vector<Scene::PrefabOverride> FindTargetOverrides(const OverrideMenuTarget& target)
{
    std::vector<Scene::PrefabOverride> found;
    const Scene::PrefabInstanceEntity* described =
        PrefabInstanceDescriptions::Get().FindEntity(*target.World, target.Entity);
    const std::string_view token = Scene::SceneSchemaRegistry::SchemaTokenForComponentType(target.Component);
    if (!described || token.empty())
        return found;
    for (const Scene::PrefabOverride& entry : described->Overrides)
    {
        if (entry.Component != token)
            continue;
        const bool headerEntry = entry.Kind == Scene::PrefabOverrideKind::AddedComponent ||
                                 entry.Kind == Scene::PrefabOverrideKind::RemovedComponent;
        const bool fieldEntry = entry.Kind == Scene::PrefabOverrideKind::Field &&
                                InspectorFieldForSceneKey(target.Component, entry.Field) == target.Field;
        if (target.Field.empty() ? headerEntry : fieldEntry)
            found.push_back(entry);
    }
    return found;
}

// What the undo step names: the row's label, else the field's words, else the component's for a header.
std::string DescribeTarget(const OverrideMenuTarget& target)
{
    if (!target.Label.empty())
        return target.Label;
    if (!target.Field.empty())
        return IdentifierToWords(target.Field);
    return IdentifierToWords(Scene::SceneSchemaRegistry::SchemaTokenForComponentType(target.Component));
}

// The component of the first override of `entity` among `targets`; 0 when no loaded module registers it.
ECS::ComponentTypeId ComponentTypeOfTargets(std::span<const Scene::PrefabOverrideTarget> targets,
                                            ECS::EntityHandle entity)
{
    for (const Scene::PrefabOverrideTarget& target : targets)
    {
        if (target.Entity != entity)
            continue;
        if (const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find(target.Override.Component))
            return Scene::SceneSchemaRegistry::ComponentTypeForSchema(*schema);
    }
    return 0;
}

std::vector<Scene::PrefabOverrideTarget> TargetsOf(const OverrideMenuTarget& target,
                                                  const std::vector<Scene::PrefabOverride>& overrides)
{
    std::vector<Scene::PrefabOverrideTarget> targets;
    for (const Scene::PrefabOverride& prefabOverride : overrides)
        targets.push_back({target.Entity, prefabOverride});
    return targets;
}

void ApplyOverrides(const OverrideMenuTarget& target, const std::vector<Scene::PrefabOverride>& overrides,
                    const std::string& prefabName)
{
    if (!target.World || !target.World->IsValid(target.Entity))
        return;
    ApplyPrefabOverridesAsOneStep(*target.World, TargetsOf(target, overrides), target.Undo, target.Notifications,
                                  std::format("Apply {} to prefab '{}'", DescribeTarget(target), prefabName));
}

void RevertOverrides(const OverrideMenuTarget& target, const std::vector<Scene::PrefabOverride>& overrides)
{
    if (!target.World || !target.World->IsValid(target.Entity))
        return;
    RevertPrefabOverridesAsOneStep(*target.World, TargetsOf(target, overrides), target.Undo, target.Notifications,
                                   "Revert " + DescribeTarget(target));
}

} // namespace

std::vector<OverrideMenuEntry> QueryPrefabOverrideMenu(const OverrideMenuTarget& target)
{
    std::vector<OverrideMenuEntry> entries;
    if (!target.World || !target.World->IsValid(target.Entity))
        return entries;
    std::vector<Scene::PrefabOverride> overrides = FindTargetOverrides(target);
    if (overrides.empty())
        return entries;
    const std::string name = PrefabNameOf(*target.World, FindInstanceRoot(*target.World, target.Entity));
    const std::string prefabName = name.empty() ? std::string("Prefab") : name;
    const Scene::PrefabOverrideKind kind = overrides.front().Kind;
    OverrideMenuEntry apply{std::format("Apply to prefab '{}'", prefabName), EditorIcons::kCube,
                            [target, overrides, prefabName]() { ApplyOverrides(target, overrides, prefabName); }};
    const std::string revertLabel =
        kind == Scene::PrefabOverrideKind::AddedComponent     ? std::format("Remove {} from instance", DescribeTarget(target))
        : kind == Scene::PrefabOverrideKind::RemovedComponent ? std::format("Restore {}", DescribeTarget(target))
                                                              : std::string(kRevertFieldLabel);
    OverrideMenuEntry revert{revertLabel, EditorIcons::kRevert,
                             [target, overrides = std::move(overrides)]() { RevertOverrides(target, overrides); }};
    // A removed component's header offers its restore first: the ghost reads as something to bring back.
    if (kind == Scene::PrefabOverrideKind::RemovedComponent)
    {
        entries.push_back(std::move(revert));
        entries.push_back(std::move(apply));
        return entries;
    }
    entries.push_back(std::move(apply));
    entries.push_back(std::move(revert));
    return entries;
}

void ApplyPrefabOverridesAsOneStep(ECS::World& world, std::span<const Scene::PrefabOverrideTarget> targets,
                                   UndoRedoService* undo, EditorChangeNotifications* notifications,
                                   const std::string& undoName)
{
    if (targets.empty())
        return;
    std::string error;
    // An applied field keeps no record; the undo gives back the records the fields held.
    const std::vector<Scene::PrefabOverrideTarget> dropped = Scene::ForgetPrefabOverrides(world, targets);
    std::unique_ptr<ApplyPrefabOverrideCommand> command =
        ApplyPrefabOverrideCommand::Create(world, notifications, targets, undoName, error);
    if (!command)
        Scene::RecordPrefabOverrides(world, dropped);
    else if (undo)
    {
        undo->BeginCompound(undoName);
        undo->CommitAlreadyApplied(std::move(command));
        if (!dropped.empty())
            undo->CommitAlreadyApplied(
                std::make_unique<PrefabOverrideRecordsCommand>(world, notifications, kNoTargets, dropped, undoName));
        undo->EndCompound();
    }
    if (!error.empty())
        ShowSceneViewNotice(std::move(error));
}

void RevertPrefabOverridesAsOneStep(ECS::World& world, std::span<const Scene::PrefabOverrideTarget> targets,
                                    UndoRedoService* undo, EditorChangeNotifications* notifications,
                                    const std::string& undoName)
{
    if (targets.empty())
        return;
    std::string error;
    // One edit for each entity and the records the revert drops, in one compound step.
    std::vector<ECS::EntityHandle> entities;
    for (const Scene::PrefabOverrideTarget& target : targets)
        if (std::find(entities.begin(), entities.end(), target.Entity) == entities.end())
            entities.push_back(target.Entity);
    if (undo)
        undo->BeginCompound(undoName);
    std::vector<Scene::PrefabOverrideTarget> reverted;
    for (const ECS::EntityHandle entity : entities)
    {
        if (!world.IsValid(entity))
            continue;
        const auto revert = [&world, entity, targets, &reverted, &error]()
        {
            for (const Scene::PrefabOverrideTarget& target : targets)
            {
                if (target.Entity != entity)
                    continue;
                if (!Scene::RevertPrefabOverride(world, entity, target.Override, error))
                    return false;
                reverted.push_back(target);
            }
            return true;
        };
        CommitEntityComponentsEdit(world, entity, undo, notifications, undoName, revert);
        // The description from before the revert would give the reverted fields their records back.
        PrefabInstanceDescriptions::Get().Forget(world, entity);
        if (notifications)
            notifications->NotifyComponentChanged({&world, entity, ComponentTypeOfTargets(targets, entity),
                                                   EditorChangeNotifications::ChangeKind::Commit});
    }
    if (undo)
    {
        if (!reverted.empty())
            undo->CommitAlreadyApplied(
                std::make_unique<PrefabOverrideRecordsCommand>(world, notifications, kNoTargets, reverted, undoName));
        undo->EndCompound();
    }
    if (!error.empty())
        ShowSceneViewNotice(std::move(error));
}

} // namespace GameEngine::Editor
