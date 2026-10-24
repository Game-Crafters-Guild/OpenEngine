#pragma once
#include "ECS/Entity.h"
#include "ECS/Systems.h"

#include <vector>

namespace GameEngine::Components
{
struct WorldTransform;
} // namespace GameEngine::Components

namespace GameEngine::PathfindingECS
{

class NavigationMovementSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "NavigationMovementSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    // Records the agent's new position when it differs from `current`.
    void QueueMove(ECS::EntityHandle entity, const Components::WorldTransform& current, float32 x, float32 y,
                   float32 z);
    // Writes each recorded position into the agent's local Transform and its WorldTransform.
    void WriteMoves(ECS::World& world) const;

    // An agent's new position this update, written after the agents are visited.
    struct AgentMove
    {
        ECS::EntityHandle Entity;
        float32 X = 0.0f;
        float32 Y = 0.0f;
        float32 Z = 0.0f;
    };

    float32 m_AccumulatedTime = 0.0f;
    std::vector<AgentMove> m_Moves;
};

} // namespace GameEngine::PathfindingECS
