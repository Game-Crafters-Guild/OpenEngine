#pragma once
#include "ECS/Systems.h"

#include <unordered_map>

namespace GameEngine::PathfindingECS
{

class NavigationDebugSystem : public ECS::ISystem
{
public:
    // The debug drawing animates while paused.
    static constexpr ECS::SystemTime Time = ECS::SystemTime::Real;

    const char* GetName() const override { return "NavigationDebugSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    std::unordered_map<uint64, uint32> m_GridVersionCache;
    bool m_LastShowCosts = false;
    float32 m_Time = 0.0f;
};

} // namespace GameEngine::PathfindingECS
