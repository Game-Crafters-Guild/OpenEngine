#pragma once

#include "ECS/Entity.h"
#include "Scene/PrefabAuthoring.h"
#include "Scene/SceneIO.h"
#include "Types/Color.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

/// What one row of the Overrides list shows: the kind of the override, or an override line the prefab does not
/// have a target for.
enum class PrefabOverrideRowKind
{
    ChangedField,
    AddedComponent,
    RemovedComponent,
    AddedEntity,
    RemovedEntity,
    Stale,
};

/// One override of an instance as the Overrides list shows it.
struct PrefabOverrideRow
{
    PrefabOverrideRowKind Kind = PrefabOverrideRowKind::ChangedField;
    /// The component and the Inspector's label for a field ("Transform: Local Position", "Reflection Probe:
    /// Intensity"), "Name" for the name, the component's name for a component, "Entity" for an added entity, the
    /// prefab's name of a removed child, the component and the field of the line for a stale row.
    std::string Label;
    /// The prefab's value of a changed field as the Inspector shows it (an asset's name, a rotation in degrees);
    /// empty for every other kind and for a value the list cannot show.
    std::string PrefabValue;
    /// The prefab's color of a changed color field, shown as a swatch beside PrefabValue.
    std::optional<ColorLinear> PrefabColor;
    /// The override as the instance's description reports it; empty for an added entity and a stale row.
    Scene::PrefabOverride Override;
    /// The index of the stale row's skip in the load report that the list was built from.
    std::size_t SkipIndex = 0;
    /// True when the instance shows the same value as the prefab: the field keeps a record.
    bool SameAsPrefab = false;
};

/// The overrides of one entity of the instance, or the stale rows of one address the prefab does not have.
struct PrefabOverrideGroup
{
    /// Invalid for a stale group.
    ECS::EntityHandle Entity;
    /// The entity's name; for a stale group, the name of the prefab's entity at the address, else the address.
    std::string Name;
    bool Stale = false;
    std::vector<PrefabOverrideRow> Rows;
};

/// The rows of an Overrides list: the overrides, and the stale rows.
struct PrefabOverrideCounts
{
    std::size_t Overrides = 0;
    std::size_t Stale = 0;
};

/// Every override of one outermost prefab instance, grouped by entity: the root first, then the entities under it,
/// parents first, then one stale group for each address the prefab does not have. Entities with no override have
/// no group.
struct PrefabOverridesListModel
{
    ECS::EntityHandle Root;
    std::string InstanceName;
    std::string PrefabName;
    std::vector<PrefabOverrideGroup> Groups;

    /// The overrides of the outermost instance that holds `entity`. `loadSkips` is the scene's load report: each
    /// orphaned override that the load recorded on the instance's root is a stale row.
    static PrefabOverridesListModel Describe(ECS::World& world, ECS::EntityHandle entity,
                                             std::span<const Scene::SceneDegradation::Skip> loadSkips);

    /// The row counts that Describe gives for the same arguments, without the labels and the prefab values.
    static PrefabOverrideCounts Count(ECS::World& world, ECS::EntityHandle entity,
                                      std::span<const Scene::SceneDegradation::Skip> loadSkips);
};

} // namespace GameEngine::Editor
