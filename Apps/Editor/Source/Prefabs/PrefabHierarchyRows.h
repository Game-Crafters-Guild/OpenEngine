#pragma once

#include "ECS/ECS.h"

#include <functional>
#include <string>

namespace GameEngine
{
class AssetRegistry;
class IThumbnailProvider;
class Label;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

class EditorChangeNotifications;
class SceneDocumentManager;
class UndoRedoService;
struct HierarchyRowParts;

/// What a Hierarchy row shows for prefabs: the root of an instance, an entity inside one, or
/// neither.
enum class PrefabRowRole
{
    None,
    InstanceRoot,
    Member,
};

struct PrefabRowState
{
    PrefabRowRole Role = PrefabRowRole::None;
    /// True for an instance root whose prefab file is not in the project.
    bool SourceMissing = false;
    /// True when an ancestor carries PrefabInstance: always for a Member, and for the root of a
    /// nested instance.
    bool InsideInstance = false;
};

/// The nearest entity at or above `entity` that carries PrefabInstance, or the invalid handle.
ECS::EntityHandle FindInstanceRoot(ECS::World& world, ECS::EntityHandle entity);

/// The outermost entity at or above `entity` that carries PrefabInstance, or the invalid handle.
ECS::EntityHandle FindOutermostInstanceRoot(ECS::World& world, ECS::EntityHandle entity);

/// The name of the prefab of the instance root `root` as the asset database shows it; empty when the
/// file is not in the project.
std::string PrefabNameOf(ECS::World& world, ECS::EntityHandle root);

/// The prefab role of `entity` in `world`: InstanceRoot when it carries PrefabInstance, Member when
/// only an ancestor does. `registry` answers whether a root's prefab file is in the project.
PrefabRowState ClassifyPrefabRow(ECS::World& world, ECS::EntityHandle entity, const AssetRegistry& registry);

/// Sets the prefab classes, the override dot or the added plus, and the missing-file tooltip on a
/// Hierarchy row as it binds (EditorComponentTraits::DecorateHierarchyRow for PrefabInstance).
/// The rows read the instance descriptions of PrefabInstanceDescriptions.
void DecoratePrefabRow(ECS::World& world, ECS::EntityHandle entity, HierarchyRowParts& parts);

/// Shows `text` on `label`; an empty `text` hides the label, so it takes no space.
void SetShownText(Label& label, const std::string& text);

/// Registers the prefab editor: the Scene View pick climb, the Hierarchy glyph, rows and menu, the Inspector's prefab
/// bar, override marks and override menus (empty while `isInPlayMode` returns true), the drop on an Assets folder and
/// the `.prefab` tile in `thumbnails` when there is one. The Overrides list reads the load report of the document that
/// `document` returns, which may be null. The records an Inspector commit writes join its step in `undo`, which may be
/// null. Call once at startup.
void RegisterPrefabEditor(EditorChangeNotifications& notifications, UndoRedoService* undo,
                          IThumbnailProvider* thumbnails, std::function<bool()> isInPlayMode,
                          std::function<SceneDocumentManager*()> document);

} // namespace GameEngine::Editor
