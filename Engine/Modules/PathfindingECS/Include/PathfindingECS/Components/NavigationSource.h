#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Whether an entity's rendered mesh is part of the navigation bake.
enum class NavigationSourcePolicy : uint8
{
    // The mesh is baked when neither its entity nor any ancestor moves on its own: no navigation
    // agent, and no physics body or a Static one.
    Auto = 0,
    // The mesh is baked where it stands when the bake runs, even when the entity that carries the
    // Include moves. A descendant that inherits it and moves on its own is left out unless it sets
    // Include itself.
    Include = 1,
    // The mesh is never baked. Colliders are separate sources and keep their own rule.
    Exclude = 2,
};

// The navigation bake's policy for the meshes of this entity and of every descendant that has no
// NavigationSource of its own: blueprint children and the submesh entities a model loads under it.
// An entity with no NavigationSource on itself or an ancestor bakes as Auto.
struct NavigationSource
{
    // A child that authors its own policy, Auto included, overrides its ancestors; removing the
    // component is the off state.
    static constexpr bool NotToggleable = true;

    NavigationSourcePolicy Policy = NavigationSourcePolicy::Auto;
};

static_assert(std::is_trivially_copyable_v<NavigationSource>);
static_assert(std::is_standard_layout_v<NavigationSource>);

} // namespace GameEngine::Components
