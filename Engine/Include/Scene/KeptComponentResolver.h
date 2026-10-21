#pragma once

#include <cstddef>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Scene
{
/// Applies the scene components a load kept aside because no registered type had their name
/// (`World::GetUnresolvedComponents`), for every entry whose type is registered now: a C# or C++
/// module that loaded after the scene did. Each resolved entry gets the real component with its
/// authored values and leaves the store, so the next save writes it through the reflected path;
/// entries whose type is still unknown, or whose values do not apply, stay kept for a later load.
/// Entries of destroyed entities are dropped. Main thread only; idempotent.
/// @return The number of components applied.
std::size_t ResolveKeptComponents(ECS::World& world);
} // namespace GameEngine::Scene
