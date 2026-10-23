#pragma once

#include "ECS/Systems.h"
#include "SmartShape2D/ShapeDestructionHistory.h"

namespace GameEngine::SmartShape2D
{
class ShapeDestructionDemoSystem : public ECS::ISystem
{
public:
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    ShapeDestructionHistory m_History;
};
}
