#pragma once

#include "ECS/ECS.h"

#include <span>
#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// The Scene View eye: the Hierarchy's eye column. Every entity of `entities` takes `hidden` as its own
// eye state (Components::SceneViewHidden); an entity that already has it is left alone. Descendants
// keep their own state and follow through ECS::SceneViewHiddenInHierarchySystem. The eye is editor view
// state (design decision D12): it is not on the undo stack, announces no change and leaves the scene
// unmodified.
void SetHiddenInSceneView(ECS::World& world, std::span<const ECS::EntityHandle> entities, bool hidden);

// True when the Scene View leaves `entity` out: hidden itself, or under a hidden ancestor. Picking and
// the selection outline ask this, so a hidden entity is neither clicked nor outlined there.
bool IsHiddenInSceneView(ECS::World& world, ECS::EntityHandle entity);

// An entity's eye as the Hierarchy shows it.
struct SceneViewEyeState
{
    bool HasMesh = false;        // the entity itself has a mesh renderer
    bool HiddenSelf = false;     // its own eye is closed (SceneViewHidden)
    bool HiddenByParent = false; // its own eye is open and an ancestor's is closed
    // While HiddenByParent: the nearest ancestor whose own eye is closed.
    ECS::EntityHandle HidingAncestor{};

    bool operator==(const SceneViewEyeState&) const = default;
};

SceneViewEyeState DescribeSceneViewEye(ECS::World& world, ECS::EntityHandle entity);

// The eye's tooltip: what the eye hides (meshes, the entity's and its descendants') and what a click
// does; for an entity hidden by an ancestor, which one.
std::string SceneViewEyeTooltip(ECS::World& world, const SceneViewEyeState& state);

// Keeps the eye's components out of the Inspector: the Hierarchy's eye column shows them.
void RegisterSceneViewEyeTraits();

} // namespace GameEngine::Editor
