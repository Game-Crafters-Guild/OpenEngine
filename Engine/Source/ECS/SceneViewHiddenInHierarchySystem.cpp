#include "ECS/SceneViewHiddenInHierarchySystem.h"

namespace GameEngine
{
namespace ECS
{

void SceneViewHiddenInHierarchySystem::Update(World& world, float32 /*deltaTime*/)
{
    m_Derivation.Update(world);
}

} // namespace ECS
} // namespace GameEngine
