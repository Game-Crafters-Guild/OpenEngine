#include "SceneView/SceneViewDropPlacement.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Ray.h"
#include "Physics/PhysicsQuery.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "Picking/ScenePick.h"
#include "AssetCore/AssetTypes.h"
#include "Types/StringUtils.h"

#include <cmath>
#include <format>

namespace GameEngine::Editor
{

namespace
{

constexpr float kFallbackAlongRayDistance = 10.0f;

// The result becomes a pick ray, so a non-finite direction has to take the
// +Z fallback rather than pass through: NaN compares false against everything
// and would slip a bare `len2 < eps` test, and an infinite len2 slips it too,
// yielding d * (1/inf).
Mathematics::Vector3 NormalizeDir(const Mathematics::Vector3& d)
{
    const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    if (!std::isfinite(len2) || len2 < 1e-12f)
        return Mathematics::Vector3(0.0f, 0.0f, 1.0f);
    const float inv = 1.0f / std::sqrt(len2);
    return Mathematics::Vector3(d.x * inv, d.y * inv, d.z * inv);
}

bool HasDisabledInAncestryOrSelf(ECS::World& world, ECS::EntityHandle entity)
{
    using GameEngine::Components::Parent;
    if (!entity.IsValid())
        return false;
    if (world.GetComponent<ECS::Disabled>(entity) != nullptr)
        return true;
    int safety = 64;
    const Parent* p = world.GetComponent<Parent>(entity);
    while (p && p->Entity.IsValid() && safety-- > 0)
    {
        if (world.GetComponent<ECS::Disabled>(p->Entity) != nullptr)
            return true;
        p = world.GetComponent<Parent>(p->Entity);
    }
    return false;
}

} // namespace

SceneViewDropPoint ResolveSceneViewDropPoint(const Mathematics::Ray3D& cameraRay,
                                             ECS::World& world,
                                             std::span<const ECS::EntityHandle> ignoreEntities)
{
    using namespace GameEngine::Mathematics;

    Ray3D ray{};
    ray.origin = cameraRay.origin;
    ray.direction = NormalizeDir(cameraRay.direction);
    SceneViewDropPoint point;

    // Physics first: takes priority over the visible mesh because a collision
    // shape often models the "intended drop surface" (e.g. a flat platform
    // with a decorative mesh on top). Drop wherever the physics ray lands.
    if (auto* phys = PhysicsECS::PhysicsWorldService::TryGet())
    {
        Physics::RayCastQuery q{};
        q.ray.origin = ray.origin;
        q.ray.direction = ray.direction;
        q.maxDistance = 1.0e6f;
        Physics::RayCastResult hit{};
        if (phys->RayCast(q, hit) && hit.HasHit())
        {
            point.Position = Vector3(hit.hitPoint.x, hit.hitPoint.y, hit.hitPoint.z);
            return point;
        }
    }

    // Per-triangle mesh fallback. Drop wherever the cursor hits - no
    // orientation gating. Matches Godot/Unreal behavior: the user is
    // intentionally aiming at the surface they want to drop on. Future
    // follow-up: optional modifier to align the placed object's up-axis
    // to the hit normal (Unreal's "Surface Snap" feature).
    Scene::PickOptions pickOpts;
    pickOpts.IncludeTerrain  = true;
    pickOpts.IncludeBoundsFallback = true; // a renderer with no CPU mesh still takes the drop
    pickOpts.IgnoreEntities  = ignoreEntities;
    const auto pick = Picking::PickScene(ray, world, pickOpts);
    if (pick.Hit)
    {
        point.Position = pick.Best.WorldPosition;
        point.OnPendingMesh = pick.Best.MeshBvhPending;
        return point;
    }

    float tPlane = 0.0f;
    Vector3 planeHit{};
    const Vector3 planeN(0.0f, 1.0f, 0.0f);
    const Vector3 planeO(0.0f, 0.0f, 0.0f);
    if (IntersectRayPlane(ray, planeO, planeN, tPlane, planeHit) && tPlane >= 0.0f)
    {
        point.Position = planeHit;
        return point;
    }

    point.Position = RayPointAt(ray, kFallbackAlongRayDistance);
    return point;
}

SceneViewDropMesh PickSceneViewDropMesh(const Mathematics::Ray3D& cameraRay,
                                        ECS::World& world,
                                        std::span<const ECS::EntityHandle> ignoreEntities)
{
    Mathematics::Ray3D ray{};
    ray.origin = cameraRay.origin;
    ray.direction = NormalizeDir(cameraRay.direction);

    Scene::PickOptions pickOpts;
    pickOpts.IncludeTerrain = false;
    pickOpts.IncludeBoundsFallback = true;
    pickOpts.IgnoreEntities = ignoreEntities;
    const auto pick = Picking::PickScene(ray, world, pickOpts);
    SceneViewDropMesh mesh;
    if (!pick.Hit || HasDisabledInAncestryOrSelf(world, pick.Best.Entity))
        return mesh;
    mesh.Entity = pick.Best.Entity;
    mesh.Pending = pick.Best.MeshBvhPending;
    return mesh;
}

bool IsPlacedModelFile(const std::filesystem::path& file)
{
    const std::string extension = ToLowerAscii(file.extension().string());
    return extension == ".glb" || extension == ".gltf" || extension == ".obj" || extension == ".fbx";
}

std::string DescribeUnplaceableFile(const std::filesystem::path& file)
{
    const AssetType type = GetAssetTypeFromExtension(file.extension().string());
    if (type == AssetType::NavigationGrid)
        return "Navigation Grid files cannot be placed in a scene. Add a Navigation Surface component and "
               "assign the file to its Authored Cells.";
    // A model format the drop does not import, or a file of no known type, is named by its extension.
    if (type == AssetType::Model || type == AssetType::Unknown)
        return std::format("{} files cannot be placed in a scene.", file.extension().string());
    return std::format("{} files cannot be placed in a scene.", IdentifierToWords(AssetTypeToString(type)));
}

std::string DescribeRefusedDrop(std::span<const std::filesystem::path> files)
{
    const std::filesystem::path* firstFile = nullptr;
    for (const std::filesystem::path& file : files)
    {
        if (file.empty())
            continue;
        const AssetType type = GetAssetTypeFromExtension(file.extension().string());
        if (IsPlacedModelFile(file) || type == AssetType::Prefab || type == AssetType::LensFlareDefinition)
            return {};
        if (!firstFile)
            firstFile = &file;
    }
    if (!firstFile)
        return "Nothing to drop.";
    if (files.size() == 1)
    {
        const AssetType type = GetAssetTypeFromExtension(firstFile->extension().string());
        if (type == AssetType::Texture || type == AssetType::Material)
            return {};
    }
    return DescribeUnplaceableFile(*firstFile);
}

} // namespace GameEngine::Editor
