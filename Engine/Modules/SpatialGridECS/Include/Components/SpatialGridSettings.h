#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// The world's spatial grid settings, a singleton. A world with none uses the defaults.
// A setting changes the speed of a query, never its result.
// @ge-no-add        a world singleton that the game sets from code (World::SetSingleton)
struct SpatialGridSettings
{
    // Meters, the edge of the smallest cell, rounded down to a power of two of 1 or more.
    // Use the diameter of the game's most common query. A value under 1 or not finite logs a warning.
    float32 CellSize = 16.0f;
};

static_assert(std::is_trivially_copyable_v<SpatialGridSettings>);

} // namespace GameEngine::Components
