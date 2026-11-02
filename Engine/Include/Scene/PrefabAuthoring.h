#pragma once

// The editor's prefab authoring: Place, Create Prefab and the queries its panels read. A game spawns
// through Scene/PrefabSpawn.h and never calls these.

#include "AssetCore/GUID.h"
#include "Components/PrefabInstance.h"
#include "Components/SceneEntityTag.h"
#include "ECS/ECS.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class EventSubscription;
}

namespace GameEngine::Components
{
struct Transform;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Scene
{

/// An entity whose scene identity a prefab command changed, with the scene identity and the instance
/// marker it had before. RestorePrefabIdentities gives them back.
struct PrefabIdentityBefore
{
    ECS::EntityHandle Entity;
    std::optional<Components::SceneEntityTag> Tag;
    std::optional<Components::PrefabInstance> Marker;
};

/// The result of CreatePrefab. `Prefab` is null when nothing was created.
struct CreatedPrefab
{
    GUID Prefab;
    /// The members of the new instance, the root first, parents before children.
    std::vector<PrefabIdentityBefore> Members;
};

/// What UnpackPrefabInstance makes plain: the outer instance only, or every instance inside it too.
enum class PrefabUnpackDepth
{
    Outer,
    Completely,
};

/// The kind of one difference between a prefab entity and the prefab.
enum class PrefabOverrideKind
{
    Field,
    AddedComponent,
    RemovedComponent,
    RemovedChild,
};

/// One difference between a prefab entity and the prefab, as a scene save writes it.
struct PrefabOverride
{
    PrefabOverrideKind Kind = PrefabOverrideKind::Field;
    /// The component's token in the scene file; empty for a RemovedChild.
    std::string Component;
    /// The field's name in the scene file for a Field; the child's address for a RemovedChild.
    std::string Field;

    bool operator==(const PrefabOverride&) const = default;
};

/// One entity of a placed prefab instance, as DescribePrefabInstance reports it.
struct PrefabInstanceEntity
{
    ECS::EntityHandle Entity;
    /// True for an entity under the instance that is not part of the prefab: a scene save writes it
    /// as an entity of its own.
    bool AddedOnInstance = false;
    /// How a prefab entity differs from the prefab, in component order. The root's placement
    /// (transform, name, sibling order) is no override. Empty for an entity added on the instance.
    std::vector<PrefabOverride> Overrides;

    bool HasOverrides() const { return !Overrides.empty(); }
};

/// A prefab file's text before and after an apply: an apply's undo writes Before, its redo After.
struct PrefabFileEdit
{
    std::filesystem::path File;
    std::string Before;
    std::string After;
};

/// The editor's Place: SpawnPrefab after it loads `prefab` and the prefabs its PrefabRef fields name, and keeps
/// them resident while `world` exists, as a scene load does. Each entity gets a new scene identity, so a scene
/// save writes a [prefab] section. Returns the root, or an invalid handle with the cause logged. Main thread only.
ECS::EntityHandle PlacePrefab(ECS::World& world, const GUID& prefab, const Components::Transform& at,
                              ECS::EntityHandle parent);

/// The editor's Create Prefab: writes the subtree under `root` to the new .prefab file `prefabFile` and makes it an
/// instance in place; every handle stays valid, and the prefabs its PrefabRef fields name stay resident for `world`.
/// On failure returns the null GUID, with the cause and the fix in `outError`. Main thread only.
CreatedPrefab CreatePrefab(ECS::World& world, ECS::EntityHandle root, const std::filesystem::path& prefabFile,
                           std::string& outError);

/// The editor's Unpack: makes the instance at `root` plain entities with the values it shows. The root keeps
/// its identity, every member gets a new one; an instance inside stays one unless `depth` is Completely.
/// Returns the changed entities with what they had, parents first; empty with `outError` when refused.
std::vector<PrefabIdentityBefore> UnpackPrefabInstance(ECS::World& world, ECS::EntityHandle root,
                                                       PrefabUnpackDepth depth, std::string& outError);

/// The editor's Duplicate: gives the copies at `copyRoots`, which still hold the identities they were
/// copied from, identities of their own. An instance in a copy stays an instance with its overrides and a
/// new identity; any other copy has none until a save gives it one. Main thread only.
void GiveCopySceneIdentities(ECS::World& world, std::span<const ECS::EntityHandle> copyRoots);

/// Gives each entity of `before` that is alive the scene identity and the instance marker it had: the
/// undo of CreatePrefab and UnpackPrefabInstance. Main thread only.
void RestorePrefabIdentities(ECS::World& world, std::span<const PrefabIdentityBefore> before);

/// Rebuilds, as a new generation, every resident template built from the file of `prefab`, after a
/// write the file watcher has not reported yet, and gives the instances of every world that holds it the
/// new generation. A template that does not build is logged and keeps its previous generation. Main
/// thread only.
void RebuildPrefab(const GUID& prefab);

/// While `deferred` is true, a prefab file change does not change the instances in `world`, also after
/// `world` is cleared. Setting it back to false gives them every change made in the meantime. The editor
/// defers its scene during Play. Main thread only.
void SetPrefabPropagationDeferred(ECS::World& world, bool deferred);

/// Calls `onChanged` on the main thread after a prefab file change changed the instances in a world:
/// their fields, components or entities. The call stops when the returned subscription is destroyed.
[[nodiscard]] EventSubscription SubscribePrefabInstancesChanged(std::function<void(ECS::World&)> onChanged);

/// Gives the prefab entity `entity` the prefab's value for `prefabOverride`: the field's value, the component
/// removed when the instance added it, or the component with the prefab's values when the instance
/// removed it. False, with the cause in `outError`, when the prefab does not have the target. Main thread only.
bool RevertPrefabOverride(ECS::World& world, ECS::EntityHandle entity, const PrefabOverride& prefabOverride,
                          std::string& outError);

/// The entity of the prefab template that the prefab entity `entity` instantiates, in the prefab library's world:
/// it holds the prefab's values. A null world when `entity` is in no resident prefab instance. Main thread only.
struct PrefabTemplateEntity
{
    const ECS::World* World = nullptr;
    ECS::EntityHandle Entity;
};
PrefabTemplateEntity FindPrefabTemplateEntity(ECS::World& world, ECS::EntityHandle entity);

/// The entity of the prefab template at `address` (entity ids joined by '.', the root's is empty), in the prefab of
/// the outermost instance that holds `entity`. A null world when the prefab is not resident or has no entity at
/// `address`. Main thread only.
PrefabTemplateEntity FindPrefabTemplateEntityAt(ECS::World& world, ECS::EntityHandle entity, std::string_view address);

/// One override of one prefab entity, for ApplyPrefabOverrides.
struct PrefabOverrideTarget
{
    ECS::EntityHandle Entity;
    PrefabOverride Override;
};

/// Writes each of `targets` into the innermost prefab file that authors its entity, with one write per file, then
/// rebuilds the written files once and updates the instances in every open world as WritePrefabFileText does. `outEdits` holds
/// the text before and after of each written file. False, with the cause in `outError`, when the prefab does not have
/// a target (then no file is written) or a write fails. Main thread only.
bool ApplyPrefabOverrides(ECS::World& world, std::span<const PrefabOverrideTarget> targets,
                          std::vector<PrefabFileEdit>& outEdits, std::string& outError);

/// Writes `text` to the prefab file `file`, rebuilds every resident prefab that reads it, and gives each
/// instance of those prefabs in every open world the new prefab values of every field, component and entity
/// it does not override. False, with the cause in `outError`, when the file cannot be written. Main thread only.
bool WritePrefabFileText(const std::filesystem::path& file, const std::string& text, std::string& outError);

/// An entity field under a prefab's root that names an entity outside the prefab.
struct OutgoingReference
{
    /// The entity that holds the field.
    ECS::EntityHandle Entity;
    /// The component of `Entity` that holds the field.
    ECS::ComponentTypeId Component = 0;
    /// The reflected name of the field in `Component`.
    std::string Field;
    /// The entity outside the prefab that the field names.
    ECS::EntityHandle Target;
};

/// The entity fields under `root` that name an entity outside the subtree, in component order. CreatePrefab
/// writes each as empty in the file and keeps it as an override on the new instance. Empty when `root` is
/// not alive.
std::vector<OutgoingReference> CollectOutgoingReferences(ECS::World& world, ECS::EntityHandle root);

/// The number of entities CreatePrefab writes for `root`: the root and every entity under it,
/// RuntimeOnlyEntity branches left out. Zero when `root` is not alive.
std::size_t CountPrefabEntities(ECS::World& world, ECS::EntityHandle root);

/// Describes the instance at `root` with the rules a scene save uses and changes nothing: the root,
/// then the entities under it, parents first; not the children of an added entity. False, with `out`
/// empty, when `root` is no instance root or its prefab is not resident. Main thread only.
bool DescribePrefabInstance(ECS::World& world, ECS::EntityHandle root, std::vector<PrefabInstanceEntity>& out);

} // namespace GameEngine::Scene
