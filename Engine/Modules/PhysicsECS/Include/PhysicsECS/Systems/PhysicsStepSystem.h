#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine::PhysicsECS
{
class PhysicsStepSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "PhysicsStepSystem"; }
    // Contact and trigger handlers run inside the step and call the engine's main-thread script API
    // (entity.Physics, an unsubscribe), so the step runs on the updating thread, outside any job,
    // whatever else its wave holds.
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(ECS::World& world, float32 deltaTime) override;
};
} // namespace GameEngine::PhysicsECS

