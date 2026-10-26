#include "ECS/DisabledInHierarchySystem.h"

#include "Components/Hierarchy.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <cstddef>

namespace GameEngine
{
namespace ECS
{

using GameEngine::Components::Parent;

void DisabledInHierarchySystem::Update(World& world, float32 /*deltaTime*/)
{
    if (m_Derivation.BindIfNeeded(world))
    {
        AutoComponentRegistrar<Parent>::EnsureRegistered();
        world.SetParentRelation(GetComponentTypeId<Parent>(), static_cast<uint32>(offsetof(Parent, Entity)));
    }
    m_Derivation.Update(world);
}

} // namespace ECS
} // namespace GameEngine
