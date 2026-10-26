#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine::SplineECS
{

// Re-derives the spline network's nodes after the splines' caches and the world transforms are
// current. Exclusive: it re-keys the records in SplineNetworkOverrides and calls the network's
// change listeners.
class SplineNetworkSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "SplineNetworkSystem"; }
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(ECS::World& world, float32 deltaTime) override;
};

} // namespace GameEngine::SplineECS
