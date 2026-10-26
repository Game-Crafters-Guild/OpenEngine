#pragma once

#include "AssetCore/GUID.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Scene/SceneRaycast.h"

namespace GameEngine::Scene
{

/// A runtime mesh that has no MeshRenderer, handed to the raycast as data: the GPU mesh to test
/// and the model GUID its BVH is cached under. The editor's pick providers describe a plugin's
/// generated meshes this way.
struct RaycastMesh
{
    ECS::EntityHandle Entity{};
    Rendering::MeshGPUHandle Mesh{};   ///< An invalid handle is a mesh not built yet, never hit.
    GUID ModelAsset{};
    Rendering::RenderLayerMask RenderLayers;
};

/// What a host application adds to a scene raycast beyond the options a game sets.
struct HostRaycastInputs
{
    /// Meshes with no MeshRenderer that the ray also tests, read when IncludeMeshes is set or
    /// when RestrictToEntity names their entity.
    std::span<const RaycastMesh> ExtraMeshes{};
    /// Renderers the ray passes through when this returns true, such as the meshes the editor's
    /// Scene View hides. Null tests every renderer.
    bool (*SkipEntity)(ECS::World& world, ECS::EntityHandle entity) = nullptr;
};

/// RaycastScene with the host's inputs. RaycastScene is this call with empty inputs.
PickResult RaycastSceneForHost(const Mathematics::Ray3D& ray,
                               ECS::World& world,
                               const PickOptions& options,
                               const HostRaycastInputs& host);

/// RaycastSceneAll with the host's inputs. RaycastSceneAll is this call with empty inputs.
bool RaycastSceneAllForHost(const Mathematics::Ray3D& ray,
                            ECS::World& world,
                            std::vector<PickHit>& outHits,
                            const PickOptions& options,
                            const HostRaycastInputs& host);

} // namespace GameEngine::Scene
