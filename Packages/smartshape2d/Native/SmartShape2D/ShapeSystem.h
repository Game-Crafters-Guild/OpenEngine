#pragma once

#include "SmartShape2D/ShapeRuntime.h"
#include "ECS/Systems.h"

namespace GameEngine::SmartShape2D
{
class ShapeSystem : public ECS::ISystem
{
public:
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(ECS::World& world, float32) override { ShapeRuntime::Get().Update(world); }
};
} // namespace GameEngine::SmartShape2D
