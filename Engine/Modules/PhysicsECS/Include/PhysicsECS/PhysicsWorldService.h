#pragma once

#include "Physics/PhysicsWorld.h"

#include <cstdint>
#include <memory>

namespace GameEngine::PhysicsECS
{
// Simple singleton service for the physics world.
// This keeps PhysicsECS modular while still providing a globally accessible world instance.
class PhysicsWorldService
{
public:
    static void Initialize(const Physics::PhysicsWorldSettings& settings = {});
    // Ends every entity event subscription (PhysicsEntityEventCallbacks::ClearAll), then destroys
    // the world.
    static void Shutdown();

    static Physics::PhysicsWorld& Get();
    static Physics::PhysicsWorld* TryGet();
    static bool IsInitialized();

    // The settings used to initialize the current world (or defaults if not initialized).
    static const Physics::PhysicsWorldSettings& GetSettings();

    // Monotonic generation counter for the physics world instance.
    // Increments on Initialize() and Shutdown(). Systems use it to reset the
    // state they cache per physics world, such as body handles.
    static std::uint64_t GetWorldGeneration();

    // Wall-clock cost of the physics steps since the last take, in milliseconds; 0 when no step ran.
    // PhysicsStepSystem adds each step; the editor's Monitors take it once per frame ("Time/Physics").
    static float32 TakeStepMs();
    static void AddStepMs(float32 ms);

    // Entities the most recent writeback updates composed a pose for (WritePhysicsPose); the rest
    // were skipped as unchanged, so a scene at rest reads 0. Bodies are written by
    // PhysicsWritebackSystem, characters by CharacterControllerWritebackSystem; two values, because
    // the two systems may run in the same wave. Read by editor diagnostics (Monitors
    // "Physics/BodiesComposed", "Physics/CharactersComposed"); reset to 0 across world lifetimes.
    static uint32 GetLastBodiesComposed();
    static void SetLastBodiesComposed(uint32 count);
    static uint32 GetLastCharactersComposed();
    static void SetLastCharactersComposed(uint32 count);

    // Kinematic bodies the most recent frame that ran a step gave a velocity toward their Transform;
    // a scene whose kinematic bodies rest reads 0. Read by editor diagnostics (Monitors
    // "Physics/KinematicBodiesMoving"); reset to 0 across world lifetimes.
    static uint32 GetLastKinematicBodiesMoving();
    static void SetLastKinematicBodiesMoving(uint32 count);

private:
    static std::unique_ptr<Physics::PhysicsWorld> s_World;
    static Physics::PhysicsWorldSettings s_Settings;
    static std::uint64_t s_WorldGeneration;
    static float32 s_StepMs;
    static uint32 s_LastBodiesComposed;
    static uint32 s_LastCharactersComposed;
    static uint32 s_LastKinematicBodiesMoving;
};

} // namespace GameEngine::PhysicsECS

