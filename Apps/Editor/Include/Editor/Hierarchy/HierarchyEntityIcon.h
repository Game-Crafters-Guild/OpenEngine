#pragma once

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "Scene/FunctionRef.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace GameEngine {

class AssetManager;
class TreeView;
class UIElement;
class UIManager;
struct EditorContext;

namespace Editor {

struct HierarchyRowParts;

/// The children of an entity, for a subtree walk; an empty span when it has none.
/// The lists must form a forest: each entity in at most one list, no list reaching
/// an ancestor. The walk has no cycle guard. The span stays valid until the next call.
using EntityChildrenOf = Scene::FunctionRef<std::span<const ECS::EntityHandle>(ECS::EntityHandle)>;

void ClearHierarchyEntityIconClasses(UIElement* element);
/// \p childrenOf, when given, answers the model-GUID subtree walk (see
/// FindModelGuidForEntityOrChildren).
void ApplyHierarchyEntityIconClasses(ECS::World* world, UIElement* element, ECS::EntityHandle handle,
                                     EntityChildrenOf childrenOf = {});
/// Same, for the element that actually draws the glyph. Adds the shared
/// `entity-icon` marker on top of the kind class, so one CSS selector
/// (`.entity-icon.hierarchy-entity-*`) styles the Hierarchy tree's preview
/// icon, the Inspector header icon and the Bookmarks row icon alike. Rows use
/// the plain overload above: a row carrying the marker would paint the glyph
/// as its own background.
void ApplyHierarchyEntityIconClassesToIcon(ECS::World* world, UIElement* icon,
                                           ECS::EntityHandle handle,
                                           EntityChildrenOf childrenOf = {});

/// True when \p element carries a hierarchy-entity CSS class that renders as a pure CSS
/// glyph (lights, cameras, primitives, empties, terrain, ocean, physics, etc.) rather than
/// a model/sprite thumbnail. Callers use this to skip the model-GUID subtree scan for rows
/// that will never show a thumbnail. This is the single source of truth — both the Hierarchy
/// row bind and the Inspector header icon consult it.
bool EntityIconIsCssOnly(const UIElement& element);

/// Runs every registered EditorComponentTraits::DecorateHierarchyRow on a row as it binds.
void ApplyHierarchyRowDecorations(ECS::World& world, ECS::EntityHandle handle, HierarchyRowParts& parts);

/// Runs the decorations again, in place, on the row `tree` holds for `treeId` (the row showing
/// `entity`), and does nothing when the tree holds none. A committed component edit changes what
/// a decoration shows without a rebind.
void ApplyHeldHierarchyRowDecorations(ECS::World& world, ECS::EntityHandle entity, const TreeView& tree,
                                      std::uint64_t treeId);

/// The first non-primitive model that the entity or a descendant draws, breadth first.
/// With \p childrenOf the walk follows those lists. Without it, the walk reads every
/// Parent in the world: use that for one lookup, never for each row of a list.
GUID FindModelGuidForEntityOrChildren(ECS::World* world, ECS::EntityHandle handle,
                                      EntityChildrenOf childrenOf = {});

/// If \p handle is a sprite (plane primitive + file-backed MaterialAsset),
/// returns the absolute filesystem path of its albedoMap texture. Returns an
/// empty path when the entity isn't a sprite or the texture can't be resolved.
std::filesystem::path TryResolveSpriteAlbedoTexturePath(ECS::World* world,
                                                        ECS::EntityHandle handle,
                                                        AssetManager* assets);

/// Returns the source-relative asset path (metadata path) corresponding to the
/// asset displayed by the inspector header icon: the sprite's albedo texture if
/// the entity is a sprite, otherwise the model asset referenced (directly or by
/// a child) by its MeshRenderer. Returns an empty path for CSS-only icons
/// (lights, primitives, cameras, etc.) that have no underlying file asset.
std::filesystem::path TryResolveInspectorIconAssetPath(ECS::World* world,
                                                       ECS::EntityHandle handle,
                                                       AssetManager* assets);

void ApplyTitleModelThumbnail(UIElement& titleEl, const std::string& relOrEngine, float iconSizePx);

/// Matches Hierarchy panel visuals: CSS entity classes and/or model / Polyhaven thumbnails on \p iconEl.
void ApplyInspectorEntityIcon(ECS::World* world,
                              const EditorContext* ctx,
                              UIElement& iconEl,
                              ECS::EntityHandle handle,
                              float iconSizePx,
                              UIManager* uiManager,
                              bool uiReplayActive,
                              UIElement* postActionHost);

} // namespace Editor
} // namespace GameEngine
