#pragma once

// A large grid model registered with the engine's asset manager and one entity rendering it: the
// fixture of the pending-BVH tests. Its BVH takes long enough to build that a ray cast right after
// the setup finds it pending.

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "MeshPicking/MeshBvh.h"
#include "Scene/MeshBvhCache.h"
#include "Scene/SceneRaycast.h"
#include "Scene/SceneTlas.h"
#include "Types/Types.h"

#include <chrono>
#include <cmath>

#include <gtest/gtest.h>

namespace PendingBvhGrid
{
using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;
using GameEngine::GUID;
using GameEngine::uint32;
using GameEngine::Mesh;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Ray3D;
using GameEngine::Mathematics::Vector3;
using GameEngine::Scene::MeshBvhCache;
using GameEngine::Scene::PickOptions;
namespace Components = GameEngine::Components;
namespace MeshPicking = GameEngine::MeshPicking;

inline constexpr float kGridSize = 20.0f;
// 300 x 300 vertices, about 179k triangles: a build long enough to tell from a copy.
inline constexpr uint32 kGridVertices = 300;

inline float SurfaceHeight(float x, float z)
{
    return 0.5f * std::sin(x * 1.3f) * std::cos(z * 1.1f);
}

// A bumpy square grid centred on the origin, heights within [-0.5, 0.5].
inline Mesh MakeGrid()
{
    Mesh mesh;
    mesh.Vertices.resize(kGridVertices * kGridVertices);
    for (uint32 j = 0; j < kGridVertices; ++j)
    {
        for (uint32 i = 0; i < kGridVertices; ++i)
        {
            const float x = (static_cast<float>(i) / (kGridVertices - 1) - 0.5f) * kGridSize;
            const float z = (static_cast<float>(j) / (kGridVertices - 1) - 0.5f) * kGridSize;
            auto& position = mesh.Vertices[j * kGridVertices + i].Position;
            position[0] = x;
            position[1] = SurfaceHeight(x, z);
            position[2] = z;
        }
    }
    for (uint32 j = 0; j + 1 < kGridVertices; ++j)
    {
        for (uint32 i = 0; i + 1 < kGridVertices; ++i)
        {
            const uint32 a = j * kGridVertices + i;
            const uint32 c = a + kGridVertices;
            mesh.Indices.insert(mesh.Indices.end(), {a, c, a + 1, a + 1, c, c + 1});
        }
    }
    return mesh;
}

inline MeshPicking::MeshView ViewOf(const Mesh& mesh)
{
    MeshPicking::MeshView view;
    view.Positions = reinterpret_cast<const Vector3*>(&mesh.Vertices[0].Position[0]);
    view.VertexStride = sizeof(GameEngine::Vertex);
    view.VertexCount = static_cast<uint32>(mesh.Vertices.size());
    view.Indices = mesh.Indices.data();
    view.IndexCount = static_cast<uint32>(mesh.Indices.size());
    return view;
}

inline double MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Straight down onto the grid.
inline Ray3D DownAt(float x, float z)
{
    Ray3D ray;
    ray.origin = Vector3(x, 50.0f, z);
    ray.direction = Vector3(0.0f, -1.0f, 0.0f);
    return ray;
}

// One grid model registered with the engine's asset manager and one entity rendering it. The
// engine is initialized because the picks reach the asset manager and the job system.
class PendingBvhGridTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void SetUp() override
    {
        m_Model = GameEngine::MakeShared<GameEngine::ModelAsset>(m_Guid, "pending_bvh_grid.glb");
        m_Model->SetMeshesForTest({MakeGrid()});
        EngineCore::GetInstance().GetAssetManager().RegisterLoadedAsset(m_Guid, m_Model);

        m_Entity = m_World.CreateEntity();
        Components::MeshRenderer renderer{};
        renderer.modelAssetGuid.Set(m_Guid);
        m_World.AddComponentImmediate(m_Entity, renderer);
        m_World.AddComponentImmediate(m_Entity, Components::WorldTransform{});
        Components::LocalBounds bounds{};
        bounds.Box.center = Vector3(0.0f, 0.0f, 0.0f);
        bounds.Box.halfExtents = Vector3(kGridSize * 0.5f, 0.5f, kGridSize * 0.5f);
        m_World.AddComponentImmediate(m_Entity, bounds);
    }

    void TearDown() override
    {
        // Waits for a job still building the key, so none outlives the test.
        (void)MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid());
        MeshBvhCache::Instance().InvalidateAsset(m_Guid);
        GameEngine::Scene::ReleaseSceneTlas(m_World);
    }

    const Mesh& Grid() const { return m_Model->GetMesh(0); }

    PickOptions RestrictedToGrid() const
    {
        PickOptions options;
        options.RestrictToEntity = m_Entity;
        return options;
    }

    const GUID m_Guid = GUID::Generate();
    GameEngine::SharedPtr<GameEngine::ModelAsset> m_Model;
    World m_World;
    GameEngine::ECS::EntityHandle m_Entity{};
};
} // namespace PendingBvhGrid
