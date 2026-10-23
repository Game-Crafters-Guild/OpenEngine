#pragma once
#include "Types/Types.h"
#include <type_traits>

namespace GameEngine::Components
{

// A vertical cylinder of Radius about the entity's position. A grid surface blocks the cells under it
// every update. With Carve, a mesh surface cuts it out of its tiles: the ground whose surface lies
// within Height above or below the position stops being walkable, rebaked under the dirty-tile cap
// when the obstacle is added, moved, removed or disabled. Without Carve, a mesh surface ignores it.
struct NavigationObstacle
{
    float32 Radius = 0.5f;
    float32 Height = 2.0f;
    bool Carve = false;
};

static_assert(std::is_trivially_copyable_v<NavigationObstacle>);
static_assert(std::is_standard_layout_v<NavigationObstacle>);

} // namespace GameEngine::Components
