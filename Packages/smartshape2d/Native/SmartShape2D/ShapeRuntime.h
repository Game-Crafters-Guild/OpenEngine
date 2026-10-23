#pragma once

#include "SmartShape2D/SmartShape.h"
#include "SmartShape2D/SmartShapeEdge.h"
#include "SmartShape2D/SmartShapeTexture.h"
#include "SmartShape2D/GeneratedShapeResources.h"
#include "ECS/ECS.h"
#include "GenerationalVector/GenerationalHandle.h"
#include "Types/FlatMap.h"
#include "Mathematics/Matrix4x4.h"

#include <array>
#include <vector>

namespace GameEngine::ECS { class World; }
namespace GameEngine::Engine::Renderer { class RenderServices; }
namespace GameEngine::Spline { class SplineData; }

namespace GameEngine::SmartShape2D
{

/// Owns generated children and mesh registrations. Accessed only by the exclusive system.
class ShapeRuntime
{
public:
    static ShapeRuntime& Get();
    void Initialize(Engine::Renderer::RenderServices* services);
    void Update(ECS::World& world);
    void Shutdown();

private:
    static void ReleaseGeneratedResources(Components::GeneratedShapeResources& resources);
    struct ShapeState
    {
        Components::SmartShape Settings{};
        uint64 SplineVersion = 0;
        uint32 SplineIndex = 0;
        uint32 SplineGeneration = 0;
        GUID MeshKey;
        std::vector<ECS::EntityHandle> Children;
        std::vector<Components::SmartShapeEdge> Overrides;
        std::vector<Components::SmartShapeTexture> Textures;
        ECS::EntityHandle CollisionEntity;
        GenerationalVector::Handle CollisionData;
        Mathematics::Matrix4x4 WorldMatrix;
        bool Built = false;
        bool Visited = false;
    };
    struct WorldState
    {
        bool HooksRegistered = false;
        uint64 ResetGeneration = 0;
        uint64 DeviceGeneration = 0;
        FlatMap<uint32, ShapeState> Shapes;
    };

    void Release(ECS::World* world, ShapeState& state);
    void Rebuild(ECS::World& world, ECS::EntityHandle owner, const Spline::SplineData& spline,
                  const Components::SmartShape& settings, ShapeState& state);

    Engine::Renderer::RenderServices* m_Services = nullptr;
    FlatMap<uint64, WorldState> m_Worlds;
    std::vector<ECS::EntityHandle> m_Candidates;
    std::vector<uint32> m_Retired;
    std::vector<std::pair<ECS::EntityHandle, Components::SmartShapeEdge>> m_Edges;
    std::vector<Components::SmartShapeEdge> m_Overrides;
    std::vector<std::pair<ECS::EntityHandle, Components::SmartShapeTexture>> m_Textures;
    std::vector<Components::SmartShapeTexture> m_TextureSet;
};
} // namespace GameEngine::SmartShape2D
