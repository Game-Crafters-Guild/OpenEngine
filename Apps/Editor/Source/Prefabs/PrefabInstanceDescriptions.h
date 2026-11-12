#pragma once

#include "EditorChangeNotifications.h"

#include "ECS/ECS.h"
#include "Scene/PrefabAuthoring.h"
#include "Types/FlatMap.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
struct OverrideMenuEntry;
struct OverrideMenuTarget;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class UndoRedoService;

/// One outermost prefab instance as Scene::DescribePrefabInstance reports it.
struct DescribedPrefabInstance
{
    ECS::EntityHandle Root;
    /// The root, then the entities under it, parents first.
    std::vector<Scene::PrefabInstanceEntity> Entities;

    /// The entry of `entity`; null for an entity that is not part of the instance.
    const Scene::PrefabInstanceEntity* Find(ECS::EntityHandle entity) const;

  private:
    friend class PrefabInstanceDescriptions;
    FlatMap<uint32, uint32> m_IndexOf; // entity id -> its index in Entities
};

/// The descriptions of the prefab instances of one world, which the Hierarchy rows, the Inspector's
/// prefab bar and its override marks read. An instance keeps its description until the editor reports
/// a committed change to one of its entities or a change to the world's structure. A committed edit of a
/// component outside Play gives a record to each field of it that was an override before the edit, in the edit's
/// undo step. Main thread only.
class PrefabInstanceDescriptions final
{
  public:
    static PrefabInstanceDescriptions& Get();

    /// Drops the descriptions that the changes `notifications` reports make stale. A channel is subscribed once;
    /// the channel owns the subscriptions, so a destroyed channel takes them with it. The records of a commit join
    /// the newest step of `undo`, which may be null; no commit writes a record while `isInPlayMode` returns true.
    /// The newest call sets `undo` and `isInPlayMode` for every channel.
    void Track(EditorChangeNotifications& notifications, UndoRedoService* undo, std::function<bool()> isInPlayMode);

    /// The description of the outermost instance that holds `entity`. Null when `entity` is in no
    /// instance or the instance's prefab is not resident. Valid until the next call on this object.
    const DescribedPrefabInstance* FindInstance(ECS::World& world, ECS::EntityHandle entity);

    /// The description of `entity` in its outermost instance; null outside every described instance.
    /// Valid until the next call on this object.
    const Scene::PrefabInstanceEntity* FindEntity(ECS::World& world, ECS::EntityHandle entity);

    /// Drops the description of the instance that holds `entity`, so the next read describes it again.
    void Forget(ECS::World& world, ECS::EntityHandle entity);

  private:
    void ResetForWorld(const ECS::World& world);
    void OnComponentChanged(EditorChangeNotifications& notifications,
                            const EditorChangeNotifications::ComponentChangedEvent& change);
    // Gives a record to each field of the edited component that was an override before the commit `change`, so
    // a field set back to the prefab's value stays an override. The new records join the commit's undo step.
    void RecordOverridesBeforeEdit(EditorChangeNotifications& notifications,
                                   const EditorChangeNotifications::ComponentChangedEvent& change, ECS::EntityHandle root);
    void OnWorldStructureChanged(const EditorChangeNotifications::WorldStructureChangedEvent& change);

    uint64 m_WorldId = 0;
    uint64 m_Epoch = 0;
    struct Entry
    {
        ECS::EntityHandle Root;
        // Null for an instance whose prefab is not resident.
        std::unique_ptr<DescribedPrefabInstance> Description;
    };

    FlatMap<uint32, Entry> m_Instances; // root id -> its entry
    std::vector<Scene::PrefabInstanceEntity> m_Scratch;
    // The GetChannelId of each tracked channel. An id is never given to a second channel.
    std::vector<uint64> m_TrackedChannels;
    UndoRedoService* m_Undo = nullptr;
    std::function<bool()> m_IsInPlayMode;
};

/// The Inspector field of `typeId` that the scene key `key` of an override writes: the reflected field of that
/// name, or the array field whose name the element's index follows, or else `key` itself (a scene key with no
/// reflected field, such as the Transform's "rotation"). The result views `key` or the field registry.
std::string_view InspectorFieldForSceneKey(ECS::ComponentTypeId typeId, std::string_view key);

/// The Apply and Revert entries of the Inspector's override menu for a prefab entity's row or header; empty
/// when the target has no override. Each entry is one undo step; a refusal shows a Scene View notice.
/// This function and the two below are defined in ApplyRevert/PrefabOverrideMenu.cpp.
std::vector<OverrideMenuEntry> QueryPrefabOverrideMenu(const OverrideMenuTarget& target);

/// Writes `targets` into their prefab files as one undo step named `undoName`. A refusal shows a notice.
void ApplyPrefabOverridesAsOneStep(ECS::World& world, std::span<const Scene::PrefabOverrideTarget> targets,
                                   UndoRedoService* undo, EditorChangeNotifications* notifications,
                                   const std::string& undoName);

/// Gives each entity of `targets` the prefab's value of its override, as one undo step named `undoName`. A refusal
/// shows a notice.
void RevertPrefabOverridesAsOneStep(ECS::World& world, std::span<const Scene::PrefabOverrideTarget> targets,
                                    UndoRedoService* undo, EditorChangeNotifications* notifications,
                                    const std::string& undoName);

} // namespace GameEngine::Editor
