#pragma once

#include "ECS/ECS.h"
#include "Mathematics/Types.h"

#include <filesystem>
#include <span>
#include <string>

namespace GameEngine
{
namespace ECS
{
class World;
}
namespace Mathematics
{
struct Ray3D;
}
} // namespace GameEngine

namespace GameEngine::Editor
{

// Shown when a drop is refused because the point it aims at is on a mesh still being
// prepared for picking (SceneViewDropPoint::OnPendingMesh, SceneViewDropMesh::Pending).
inline constexpr const char* kDropOnPendingMeshNotice =
    "This mesh is still being prepared for picking. Try again in a moment.";

// Where a drop along the scene camera ray lands.
struct SceneViewDropPoint
{
    Mathematics::Vector3 Position{};
    // Position is on the bounds of a mesh whose picking BVH is still being built: a preview may
    // show it there, but a drop that would place something refuses it.
    bool OnPendingMesh = false;
};

// Resolves a world-space position along `cameraRay` (SceneViewController::MakeGizmoRay; the
// direction need not be normalized): physics bodies first, then renderable meshes, then the
// Y=0 plane, then a point at a fixed distance. `ignoreEntities` is typically the drag-drop
// preview model's own entities so the placement ray doesn't self-intersect and walk the
// preview toward the camera.
SceneViewDropPoint ResolveSceneViewDropPoint(const Mathematics::Ray3D& cameraRay,
                                             ECS::World& world,
                                             std::span<const ECS::EntityHandle> ignoreEntities = {});

// The mesh a drop along the scene camera ray would assign to.
struct SceneViewDropMesh
{
    ECS::EntityHandle Entity{}; // invalid: no enabled mesh under the point
    // Entity's picking BVH is still being built and the ray only met its bounds: a preview may
    // highlight it, but a drop that would assign to it refuses it.
    bool Pending = false;
};

// Picks the closest enabled MeshRenderer entity along `cameraRay`, any surface orientation,
// because a texture drop should land on whichever mesh is directly under the cursor.
SceneViewDropMesh PickSceneViewDropMesh(const Mathematics::Ray3D& cameraRay,
                                        ECS::World& world,
                                        std::span<const ECS::EntityHandle> ignoreEntities = {});

// True for a model file a drop places as an entity tree: glTF, GLB, OBJ or FBX.
bool IsPlacedModelFile(const std::filesystem::path& file);

// The notice for a dropped file that a Scene View or Hierarchy drop does not place: a file that is not
// a placed model, a prefab or a lens flare. It names the kind of file and, where one exists, the way
// to use it in a scene.
std::string DescribeUnplaceableFile(const std::filesystem::path& file);

// Why a drop of `files` on the Scene View or the Hierarchy places nothing, or empty when it places
// something: one file it places, or a texture or a material dropped alone, which the drop assigns.
std::string DescribeRefusedDrop(std::span<const std::filesystem::path> files);

} // namespace GameEngine::Editor
