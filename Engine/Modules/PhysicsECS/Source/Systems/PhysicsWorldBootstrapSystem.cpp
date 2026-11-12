#include "PhysicsECS/Systems/PhysicsWorldBootstrapSystem.h"

#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/PhysicsEntityEventCallbacks.h"

#include "PhysicsECS/Components/PhysicsWorldSettingsComponent.h"

#include "ECS/Query.h"
#include "Logger/Logger.h"

namespace GameEngine::PhysicsECS
{
void PhysicsWorldBootstrapSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (PhysicsWorldService::IsInitialized())
        return;

    Physics::PhysicsWorldSettings settings{};
    bool foundSettings = false;
    int settingsCount = 0;

    world.Query<ECS::Read<GameEngine::Components::PhysicsWorldSettingsComponent>>().Each(
        [&](ECS::EntityHandle /*e*/, const GameEngine::Components::PhysicsWorldSettingsComponent& c)
        {
            ++settingsCount;
            if (!foundSettings)
            {
                settings = c.settings;
                foundSettings = true;
            }
        });

    if (settingsCount > 1)
    {
        Logger::Log::Warning("[PhysicsECS] Multiple PhysicsWorldSettingsComponent found ({}); using the first one encountered", settingsCount);
    }

    PhysicsWorldService::Initialize(settings);
    // The scene's physics rate sets the rate of the world's clock; the clock refuses a step of 0 or less.
    ECS::SimulationClock& clock = world.GetSimulationClock();
    clock.SetFixedDeltaSeconds(settings.fixedTimeStep);
    if (settings.maxSubSteps > 0)
        clock.SetMaxStepsPerFrame(static_cast<uint32>(settings.maxSubSteps));
    // A new physics world starts a play session, and a play session starts at tick 0.
    clock.Reset();
    PhysicsEntityEventCallbacks::NotifyWorldStateChanged();
}
} // namespace GameEngine::PhysicsECS
