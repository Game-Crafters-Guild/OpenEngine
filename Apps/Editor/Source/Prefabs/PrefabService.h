#pragma once

#include "ECS/ECS.h"

#include <filesystem>

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::Components
{
struct Transform;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

/// The editor's owner of prefab operations over the engine's prefab library. The panels and the
/// prefab commands call it; it never records an undo step. Main thread only.
class PrefabService
{
  public:
    explicit PrefabService(AssetManager& assets);

    /// Places an instance of `prefabFile` in `world` with its whole tree: the root at `at` under `parent`
    /// (none when invalid), last in the Hierarchy when it has no parent. A file that cannot be placed
    /// shows a Scene View notice and returns an invalid handle; the log names the cause.
    ECS::EntityHandle Place(ECS::World& world, const std::filesystem::path& prefabFile,
                            const Components::Transform& at, ECS::EntityHandle parent);

  private:
    AssetManager& m_Assets;
};

} // namespace GameEngine::Editor
