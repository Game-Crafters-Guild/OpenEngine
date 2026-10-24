#pragma once

#include "Components/SceneViewHidden.h"
#include "ECS/HierarchyTagDerivation.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine
{
namespace ECS
{

// Derives Components::SceneViewHiddenInHierarchy, "hidden by the Scene View eye, itself or through an
// ancestor", from Components::SceneViewHidden and the Parent chain. Only the editor writes
// SceneViewHidden, so in the Player this pass exits on two archetype-level counts after a structural
// change and does nothing else.
class SceneViewHiddenInHierarchySystem : public ISystem
{
  public:
    const char* GetName() const override { return "SceneViewHiddenInHierarchy"; }
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(World& world, float32 deltaTime) override;

  private:
    HierarchyTagDerivation<Components::SceneViewHidden, Components::SceneViewHiddenInHierarchy> m_Derivation;
};

} // namespace ECS
} // namespace GameEngine
