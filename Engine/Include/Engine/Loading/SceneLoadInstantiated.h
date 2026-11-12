#pragma once

#include <optional>
#include <string>

namespace GameEngine
{
namespace ECS
{
class World;
}
namespace Scene
{
struct SceneDegradation;
}

/// What a load hands to its request's OnInstantiated, on the main thread, once the world
/// holds the new scene and before any user system ticks against it.
class SceneLoadInstantiated
{
  public:
    /// The world that holds the new scene. What is spawned into it from here on counts
    /// toward this load.
    ECS::World& World() { return m_World; }
    /// The scene lines that did not apply.
    const Scene::SceneDegradation& Degradation() const { return m_Degradation; }
    /// Refuses the scene: the load raises Failed with `reason` and starts no user system.
    void Fail(std::string reason) { m_FailureReason = std::move(reason); }

  private:
    friend class SceneLoader;
    SceneLoadInstantiated(ECS::World& world, const Scene::SceneDegradation& degradation)
        : m_World(world), m_Degradation(degradation)
    {
    }

    ECS::World& m_World;
    const Scene::SceneDegradation& m_Degradation;
    std::optional<std::string> m_FailureReason;
};
} // namespace GameEngine
