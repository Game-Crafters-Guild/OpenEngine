#include "Terrain/TerrainEntityProvisioning.h"

#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/TemplateGroundPlaceholder.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Transform.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "Editor/Hierarchy/HierarchyEnableState.h"
#include "Editor/Hierarchy/HierarchyOrdering.h"
#include "SceneView/SceneViewNoticeOverlay.h"
#include "TerrainECS/TerrainEntityProvisioning.h"
#include "UndoRedo/DuplicateEntitiesCommand.h"
#include "UndoRedo/GenericEditUndo.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Logger/Logger.h"

#include <cstring>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kCreateTerrainLabel = "Create Terrain";
constexpr const char* kCreatePlanetLabel = "Create Planet";
constexpr const char* kAddReliefLabel = "Add Relief";
constexpr const char* kTurnOnLabel = "Turn on";

TerrainECS::TerrainProvisioningResult ProvisionTerrainAndPlaceRules(ECS::World& world, ECS::EntityHandle entity,
                                                                    const Components::Terrain& config,
                                                                    const Components::TerrainPlanetRelief& relief)
{
    TerrainECS::TerrainProvisioningResult provisioned = TerrainECS::ProvisionTerrainEntity(world, entity, config, relief);
    // Last, so the volume is complete before it takes a position in the hierarchy: below
    // everything already in the scene rather than wherever an unset order happens to sort.
    if (provisioned.SurfaceRulesVolume.IsValid())
        world.AddComponentImmediate(provisioned.SurfaceRulesVolume, NextHierarchyOrderAtBottom(&world));
    return provisioned;
}

// The entities the creation makes: the terrain, then the surface-rules volume when one was spawned.
// Empty when no entity was made. `outProvisioned` is false when the provisioning refused the preset.
std::vector<ECS::EntityHandle> AddTerrainEntities(ECS::World& world, const Components::Terrain& config,
                                                  const Components::TerrainPlanetRelief& relief, bool& outProvisioned)
{
    outProvisioned = false;
    const ECS::EntityHandle entity = world.CreateEntity();
    if (!entity.IsValid())
        return {};

    Components::Transform transform{};
    transform.SetIdentity();
    const std::string_view text = config.Domain == Components::TerrainDomain::Spherical ? "Planet" : "Terrain";
    Components::Name name{};
    std::memcpy(name.value, text.data(), text.size());
    world.AddComponentImmediate(entity, transform);
    world.AddComponentImmediate(entity, name);
    world.AddComponentImmediate(entity, NextHierarchyOrderAtBottom(&world));

    std::vector<ECS::EntityHandle> created{entity};
    const TerrainECS::TerrainProvisioningResult provisioned = ProvisionTerrainAndPlaceRules(world, entity, config, relief);
    if (!provisioned.Error.empty())
    {
        // The presets are inside every limit the provisioning checks.
        Logger::Log::Error("Create Terrain: the terrain preset was refused: {}", provisioned.Error);
        return created;
    }
    if (provisioned.SurfaceRulesVolume.IsValid())
        created.push_back(provisioned.SurfaceRulesVolume);
    outProvisioned = true;
    return created;
}

// Switches each template ground placeholder off through the entity switch, so the hierarchy shows it
// as off and one toggle switches it back on. Returns the placeholders that were on.
std::vector<ECS::EntityHandle> SwitchTemplateGroundOff(ECS::World& world, UndoRedoService* undo,
                                                       EditorChangeNotifications* notifications)
{
    std::vector<ECS::EntityHandle> wereOn;
    world.Query<ECS::Read<Components::TemplateGroundPlaceholder>>().IncludeDisabled().Each(
        [&](ECS::EntityHandle entity, const Components::TemplateGroundPlaceholder&) {
            if (ECS::Entity(&world, entity).IsEnabled())
                wereOn.push_back(entity);
        });
    CommitEntityEnabledToggle(world, undo, notifications, wereOn, false);
    return wereOn;
}

// The switched-off entities by name, as a list: "A", "A and B", "A, B and C".
std::string SwitchedOffNames(const ECS::World& world, const std::vector<ECS::EntityHandle>& switchedOff)
{
    std::string names;
    for (std::size_t i = 0; i < switchedOff.size(); ++i)
    {
        if (i > 0)
            names += i + 1 == switchedOff.size() ? " and " : ", ";
        names += EntityDisplayName(world, switchedOff[i]);
    }
    return names;
}

// The notice's text: the switched-off entities by name, and why they are off.
std::string GroundOffNotice(const std::string& names, std::size_t count)
{
    return names + (count == 1 ? " is switched off so it does not cover the terrain."
                               : " are switched off so they do not cover the terrain.");
}

// The "Turn on" button's tooltip: names the entities as the notice does.
std::string TurnOnTooltip(const std::string& names)
{
    return "Switch " + names + " back on. The terrain stays.";
}

// The notice's "Turn on": switches `switchedOff` back on as its own undo step. It does nothing when the
// world with id `worldId` is gone or has started a new handle run (a scene change), because an old
// handle can then name another entity.
void TurnGroundBackOn(ECS::World& world, uint64 worldId, uint64 handleEpoch, UndoRedoService* undo,
                      EditorChangeNotifications* notifications, const std::vector<ECS::EntityHandle>& switchedOff)
{
    if (ECS::World::FindLiveEntityHandleEpoch(worldId) != handleEpoch)
        return;
    CommitEntityEnabledToggle(world, undo, notifications, switchedOff, true);
}

// Makes the terrain and switches the template ground off, recording each part on `undo` when it is
// set. Returns the terrain entity, invalid if none was made.
ECS::EntityHandle AddTerrainAndSwitchGroundOff(ECS::World& world, UndoRedoService* undo,
                                               EditorChangeNotifications* notifications, const char* label,
                                               const Components::Terrain& config,
                                               const Components::TerrainPlanetRelief& relief,
                                               std::vector<ECS::EntityHandle>& outSwitchedOff)
{
    outSwitchedOff.clear();
    bool provisioned = false;
    const std::vector<ECS::EntityHandle> created = AddTerrainEntities(world, config, relief, provisioned);
    if (created.empty())
        return {};
    // Created before the command is built, so it is committed as already applied: undo removes the
    // entities, and the terrain's data with them; redo revives the same handles.
    if (undo)
        undo->CommitAlreadyApplied(
            std::make_unique<DuplicateEntitiesCommand>(label, &world, notifications, created, nullptr, nullptr));
    if (provisioned)
        outSwitchedOff = SwitchTemplateGroundOff(world, undo, notifications);
    return created.front();
}

} // namespace

std::string ProvisionTerrainEntity(ECS::World& world, ECS::EntityHandle entity,
                                   const Components::Terrain& config,
                                   const Components::TerrainPlanetRelief& relief)
{
    return std::move(ProvisionTerrainAndPlaceRules(world, entity, config, relief).Error);
}

ECS::EntityHandle CreateTerrainEntity(ECS::World& world, UndoRedoService* undo,
                                      EditorChangeNotifications* notifications, CBTTerrainECS::TerrainPreset preset)
{
    // The entity's name and the undo step's name both follow the preset's domain.
    const Components::Terrain config = CBTTerrainECS::MakeTerrainPreset(preset);
    const Components::TerrainPlanetRelief relief = CBTTerrainECS::MakePlanetReliefPreset(preset);
    const char* label = config.Domain == Components::TerrainDomain::Spherical ? kCreatePlanetLabel : kCreateTerrainLabel;
    std::vector<ECS::EntityHandle> switchedOff;
    if (!undo)
        return AddTerrainAndSwitchGroundOff(world, nullptr, notifications, label, config, relief, switchedOff);

    ECS::EntityHandle terrain{};
    CommitGenericEdit(world, *undo, label, [&]() {
        terrain = AddTerrainAndSwitchGroundOff(world, undo, notifications, label, config, relief, switchedOff);
    });
    if (switchedOff.empty())
        return terrain;
    const std::string names = SwitchedOffNames(world, switchedOff);
    ShowSceneViewNoticeWithAction(GroundOffNotice(names, switchedOff.size()), kTurnOnLabel, TurnOnTooltip(names),
                                  [&world, worldId = world.GetWorldId(), handleEpoch = world.GetEntityHandleEpoch(),
                                   undo, notifications, switchedOff]() {
                                      TurnGroundBackOn(world, worldId, handleEpoch, undo, notifications, switchedOff);
                                  });
    return terrain;
}

ECS::EntityHandle CreateGeneratedNoiseVolume(ECS::World& world, UndoRedoService* undo,
                                             EditorChangeNotifications* notifications)
{
    const ECS::EntityHandle volume = TerrainECS::SpawnGeneratedNoiseVolume(world);
    if (!volume.IsValid())
        return volume;
    world.AddComponentImmediate(volume, NextHierarchyOrderAtBottom(&world));
    // Created before the command is built, so it is committed as already applied.
    if (undo)
        undo->CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
            kAddReliefLabel, &world, notifications, std::vector<ECS::EntityHandle>{volume}, nullptr, nullptr));
    return volume;
}

} // namespace GameEngine::Editor
