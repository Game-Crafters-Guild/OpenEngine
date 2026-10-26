#pragma once

#include "ECS/ECS.h"
#include "Mathematics/Ray.h"
#include "Mathematics/Vector3.h"
#include "Rendering/RenderLayerMask.h"
#include "Types/Types.h"

#include <limits>
#include <span>
#include <vector>

namespace GameEngine::Scene
{

/// What kind of object a scene ray hit. A caller branches on this when its follow-up action
/// depends on the surface (a terrain brush against a mesh selection).
enum class PickableKind : uint8
{
    None,
    Mesh,
    Terrain,
    Primitive,
    Bounds,
};

/// Options for RaycastScene and RaycastSceneAll. The defaults test every category, so a caller
/// sets only what it wants to change.
struct PickOptions
{
    bool                          IncludeMeshes        = true;
    bool                          IncludeTerrain       = true;
    bool                          IncludePrimitives    = true;
    /// A renderer with no CPU mesh, or a mesh still preparing its picking data, answers with the
    /// top of its world box. Off by default: a box is not a surface. A click that selects sets it.
    bool                          IncludeBoundsFallback = false;
    bool                          IncludeDisabled      = false;   ///< Include ECS::Disabled entities.
    Rendering::RenderLayerMask    LayerMask            = Rendering::RenderLayerMask::All();
    /// The farthest hit, in units of the ray direction's length: meters for a unit direction.
    float32                       MaxDistance          = std::numeric_limits<float32>::infinity();
    /// Entities the ray passes through, such as a drag-drop preview's own entities.
    std::span<const ECS::EntityHandle> IgnoreEntities{};
    /// When valid, the ray tests only this entity's mesh and nothing else. A spline stroke uses it
    /// to stay on one surface.
    ECS::EntityHandle RestrictToEntity{};

    /// Whether the ray first brings the scene TLAS current (SceneTlas::WaitCurrent), which blocks
    /// while it refits after something moved. False for a caller that already saw
    /// RequestCurrent or CheckRequest report Ready this frame.
    bool WaitForSceneTlas = true;
};

/// One hit. The world position and normal are filled for every kind; the triangle index and
/// barycentrics only for a mesh or primitive hit.
struct PickHit
{
    PickableKind                  Kind          = PickableKind::None;
    ECS::EntityHandle             Entity{};
    uint32                        SubmeshIndex  = 0u;
    uint32                        TriangleIndex = ~0u;
    Mathematics::Vector3          Barycentrics{};
    Mathematics::Vector3          WorldPosition{};
    Mathematics::Vector3          WorldNormal{};
    float32                       Distance      = 0.0f;
    /// A Bounds hit standing in for a mesh still preparing its picking data. A caller that would
    /// write the hit into the scene refuses it and asks again once the data is published.
    bool                          MeshBvhPending = false;
};

struct PickResult
{
    bool    Hit = false;
    PickHit Best{};
    /// The ray crossed the box of a mesh still preparing its picking data, nearer than the hit,
    /// and IncludeBoundsFallback was clear, so that mesh gave no hit this time. A caller that
    /// measures the ground holds its result and asks again: the hit may change once it is ready.
    bool    MeshBvhPending = false;
};

/// Casts a world-space ray against the scene and returns the closest hit: every renderer with a
/// WorldTransform and a MeshRenderer, through the scene TLAS, and the terrain. A mesh and the
/// terrain at the same distance (within 1e-5 of it) answer with the mesh: a mesh laid flush on the
/// ground is what the author put on top of it.
///
/// Main thread: it reads the live world and the CPU meshes of resident models, which a hot
/// reload mutates there. The BVH cache under it is thread-safe and builds on the job system.
/// The first call registers an asset-event callback that drops a model's cached BVH when the
/// model is modified, reloaded or destroyed.
PickResult RaycastScene(const Mathematics::Ray3D& ray,
                        ECS::World& world,
                        const PickOptions& options = {});

/// Casts a world-space ray and returns every hit along it, sorted front to back, for a caller
/// that steps through stacked entities, under the same tie rule as RaycastScene. The terrain
/// contributes at most its nearest hit. Returns true when a mesh on the ray is still preparing
/// its picking data and gave no hit (IncludeBoundsFallback off): ask again on a later frame.
bool RaycastSceneAll(const Mathematics::Ray3D& ray,
                     ECS::World& world,
                     std::vector<PickHit>& outHits,
                     const PickOptions& options = {});

} // namespace GameEngine::Scene
