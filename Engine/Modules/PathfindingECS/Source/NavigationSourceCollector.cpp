#include "NavigationSourceCollector.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "Components/Navigation/NavigationAgent.h"
#include "Components/Navigation/NavigationSource.h"
#include "Components/Navigation/NavigationSurface.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PhysicsColliderOwner.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "Types/FlatMap.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace GameEngine::PathfindingECS
{

namespace
{

using Components::NavigationSourcePolicy;
using Navigation::BakeSource;
using Navigation::BakeSourceKind;

constexpr float32 kPi = 3.14159265358979323846f;
// Facets of the low-poly sphere and capsule a round collider is baked as: the voxelization is no
// finer than a cell, so more facets change nothing.
constexpr uint32 kRoundSegments = 12;
constexpr uint32 kRoundRings = 6;

void TransformPoint(const float32 m[16], const float32 in[3], float32 out[3])
{
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2] + m[12];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2] + m[13];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14];
}

// Collects one shape or mesh into the geometry as an item, transforming local vertices to world.
class ItemWriter
{
  public:
    ItemWriter(NavigationStaticGeometry& geometry, const float32 world[16], const BakeSource& source)
        : m_Geometry(geometry), m_World(world), m_Source(source), m_BaseVertex(geometry.Triangles.VertexCount()),
          m_FirstIndex(static_cast<uint32>(geometry.Triangles.Indices.size()))
    {
    }

    ~ItemWriter()
    {
        NavigationStaticGeometry::Item item;
        item.FirstIndex = m_FirstIndex;
        item.IndexCount = static_cast<uint32>(m_Geometry.Triangles.Indices.size()) - m_FirstIndex;
        item.Bounds = m_Bounds;
        item.Source = m_Source;
        if (item.IndexCount == 0)
            return;
        m_Geometry.Items.push_back(item);
        m_Geometry.Bounds.Include(m_Bounds);
    }

    // Returns the vertex's index relative to this item.
    uint32 AddVertex(float32 x, float32 y, float32 z)
    {
        const float32 local[3] = {x, y, z};
        float32 world[3];
        TransformPoint(m_World, local, world);
        m_Geometry.Triangles.Vertices.insert(m_Geometry.Triangles.Vertices.end(), world, world + 3);
        m_Bounds.Include(world);
        return m_VertexCount++;
    }

    void AddTriangle(uint32 a, uint32 b, uint32 c)
    {
        auto& indices = m_Geometry.Triangles.Indices;
        indices.push_back(m_BaseVertex + a);
        indices.push_back(m_BaseVertex + b);
        indices.push_back(m_BaseVertex + c);
    }

    void AddQuad(uint32 a, uint32 b, uint32 c, uint32 d)
    {
        AddTriangle(a, b, c);
        AddTriangle(a, c, d);
    }

  private:
    NavigationStaticGeometry& m_Geometry;
    const float32* m_World;
    BakeSource m_Source;
    uint32 m_BaseVertex;
    uint32 m_FirstIndex;
    uint32 m_VertexCount = 0;
    NavigationBounds m_Bounds;
};

// The navigation bake reads collider poses as float matrices relative to the world origin.
Mathematics::Matrix4x4 NarrowedWorldMatrix(const Components::WorldTransform& wt)
{
    return wt.ToMatrix4x4(wt.GetPosition() - Mathematics::WorldPosition{});
}

void AppendBox(NavigationStaticGeometry& geometry, const float32 world[16], const BakeSource& source,
               const Components::BoxColliderShape& box)
{
    ItemWriter writer(geometry, world, source);
    const float32 hx = box.halfExtentsX, hy = box.halfExtentsY, hz = box.halfExtentsZ;
    for (uint32 corner = 0; corner < 8; ++corner)
        writer.AddVertex((corner & 1) ? hx : -hx, (corner & 2) ? hy : -hy, (corner & 4) ? hz : -hz);
    // Faces wound outward by Recast's convention (rcMarkWalkableTriangles), so the top is walkable:
    // -X, +X, -Y, +Y, -Z, +Z.
    writer.AddQuad(0, 4, 6, 2);
    writer.AddQuad(1, 3, 7, 5);
    writer.AddQuad(0, 1, 5, 4);
    writer.AddQuad(2, 6, 7, 3);
    writer.AddQuad(0, 2, 3, 1);
    writer.AddQuad(4, 5, 7, 6);
}

// A capsule of `halfHeight` cylinder half length along local `axis` (0 = X, 1 = Y, 2 = Z); a sphere
// is a capsule with no cylinder. Rings run from the bottom pole to the top pole.
void AppendRound(NavigationStaticGeometry& geometry, const float32 world[16], const BakeSource& source, float32 radius,
                 float32 halfHeight, uint8 axis)
{
    ItemWriter writer(geometry, world, source);
    const uint32 rings = kRoundRings * 2;
    for (uint32 ring = 0; ring <= rings; ++ring)
    {
        const float32 polar = kPi * static_cast<float32>(ring) / static_cast<float32>(rings);
        const float32 along = -std::cos(polar) * radius + (ring * 2 < rings ? -halfHeight : ring * 2 > rings ? halfHeight : 0.0f);
        const float32 ringRadius = std::sin(polar) * radius;
        for (uint32 segment = 0; segment < kRoundSegments; ++segment)
        {
            const float32 azimuth = 2.0f * kPi * static_cast<float32>(segment) / static_cast<float32>(kRoundSegments);
            const float32 u = std::cos(azimuth) * ringRadius;
            const float32 v = std::sin(azimuth) * ringRadius;
            switch (axis)
            {
            case 0: writer.AddVertex(along, u, v); break;
            case 2: writer.AddVertex(u, v, along); break;
            default: writer.AddVertex(u, along, v); break;
            }
        }
    }
    for (uint32 ring = 0; ring < rings; ++ring)
    {
        for (uint32 segment = 0; segment < kRoundSegments; ++segment)
        {
            const uint32 next = (segment + 1) % kRoundSegments;
            writer.AddQuad(ring * kRoundSegments + segment, ring * kRoundSegments + next,
                           (ring + 1) * kRoundSegments + next, (ring + 1) * kRoundSegments + segment);
        }
    }
}

// The plane's solid side lies below normal . x + d = 0; its walkable face is a square of
// halfExtent about the point on the plane nearest the body origin.
void AppendPlane(NavigationStaticGeometry& geometry, const float32 world[16], const BakeSource& source,
                 const Components::PlaneColliderShape& plane)
{
    const float32 length = std::sqrt(plane.normalX * plane.normalX + plane.normalY * plane.normalY +
                                     plane.normalZ * plane.normalZ);
    if (!(length > 0.0f))
        return;
    const float32 n[3] = {plane.normalX / length, plane.normalY / length, plane.normalZ / length};
    const float32 centre[3] = {-plane.d / length * n[0], -plane.d / length * n[1], -plane.d / length * n[2]};
    // Two tangents: the axis least aligned with the normal, made orthogonal, then their cross.
    float32 seed[3] = {1.0f, 0.0f, 0.0f};
    if (std::abs(n[0]) > 0.9f)
        seed[0] = 0.0f, seed[1] = 1.0f;
    const float32 dot = seed[0] * n[0] + seed[1] * n[1] + seed[2] * n[2];
    float32 t[3] = {seed[0] - dot * n[0], seed[1] - dot * n[1], seed[2] - dot * n[2]};
    const float32 tLength = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    t[0] /= tLength, t[1] /= tLength, t[2] /= tLength;
    const float32 b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};

    ItemWriter writer(geometry, world, source);
    const float32 h = plane.halfExtent;
    for (const auto& [su, sv] : {std::pair{-1.0f, -1.0f}, std::pair{1.0f, -1.0f}, std::pair{1.0f, 1.0f},
                                 std::pair{-1.0f, 1.0f}})
    {
        writer.AddVertex(centre[0] + (su * t[0] + sv * b[0]) * h, centre[1] + (su * t[1] + sv * b[1]) * h,
                         centre[2] + (su * t[2] + sv * b[2]) * h);
    }
    // Wound to face along the normal by Recast's convention (rcMarkWalkableTriangles), so a level
    // plane is walkable.
    writer.AddQuad(0, 1, 2, 3);
}

// The mesh's positions and whole triangles in its own space; empty when it has no triangle.
NavigationTriangles CopyMesh(const Mesh& mesh)
{
    NavigationTriangles copy;
    if (mesh.Vertices.empty() || mesh.Indices.size() < 3)
        return copy;
    copy.Vertices.reserve(mesh.Vertices.size() * 3);
    for (const auto& vertex : mesh.Vertices)
        copy.Vertices.insert(copy.Vertices.end(), vertex.Position, vertex.Position + 3);
    copy.Indices.assign(mesh.Indices.begin(), mesh.Indices.begin() + static_cast<std::ptrdiff_t>(mesh.Indices.size() / 3 * 3));
    return copy;
}

// Instances one chunk of the build transforms at least.
constexpr size_t kMinInstancesPerChunk = 64;

// The body a collider entity belongs to: its owner's when it has one, else itself.
ECS::EntityHandle BodyEntityOf(ECS::EntityHandle entity, const Components::PhysicsColliderOwner* owner)
{
    return owner && owner->body.IsValid() ? owner->body : entity;
}

bool IsStaticBody(const ECS::World& world, ECS::EntityHandle bodyEntity)
{
    const auto* body = world.GetComponent<Components::PhysicsBody>(bodyEntity);
    return body && body->motionType == Physics::MotionType::Static;
}

// A collider the bake reads: solid, on a Static body. NavigationSource does not apply to colliders.
bool IsBakedCollider(const ECS::World& world, ECS::EntityHandle entity, const Components::PhysicsCollider& collider,
                     const Components::PhysicsColliderOwner* owner)
{
    return !collider.isTrigger && IsStaticBody(world, BodyEntityOf(entity, owner));
}

BakeSource ColliderSource(ECS::EntityHandle entity)
{
    return {entity, BakeSourceKind::Collider, NavigationSourcePolicy::Auto};
}

void SnapshotColliders(ECS::World& world, NavigationStaticGeometry& geometry)
{
    using Components::PhysicsCollider;
    using Components::PhysicsColliderOwner;
    using Components::WorldTransform;

    world.Query<ECS::Read<WorldTransform>, ECS::Read<PhysicsCollider>, ECS::Read<Components::BoxColliderShape>,
                ECS::Optional<PhysicsColliderOwner>>()
        .Each([&](ECS::EntityHandle e, const WorldTransform& wt, const PhysicsCollider& collider,
                  const Components::BoxColliderShape& box, const PhysicsColliderOwner* owner) {
            if (IsBakedCollider(world, e, collider, owner))
                AppendBox(geometry, NarrowedWorldMatrix(wt).Data(), ColliderSource(e), box);
        });
    world.Query<ECS::Read<WorldTransform>, ECS::Read<PhysicsCollider>, ECS::Read<Components::SphereColliderShape>,
                ECS::Optional<PhysicsColliderOwner>>()
        .Each([&](ECS::EntityHandle e, const WorldTransform& wt, const PhysicsCollider& collider,
                  const Components::SphereColliderShape& sphere, const PhysicsColliderOwner* owner) {
            if (IsBakedCollider(world, e, collider, owner))
                AppendRound(geometry, NarrowedWorldMatrix(wt).Data(), ColliderSource(e), sphere.radius, 0.0f, 1);
        });
    world.Query<ECS::Read<WorldTransform>, ECS::Read<PhysicsCollider>, ECS::Read<Components::CapsuleColliderShape>,
                ECS::Optional<PhysicsColliderOwner>>()
        .Each([&](ECS::EntityHandle e, const WorldTransform& wt, const PhysicsCollider& collider,
                  const Components::CapsuleColliderShape& capsule, const PhysicsColliderOwner* owner) {
            if (IsBakedCollider(world, e, collider, owner))
                AppendRound(geometry, NarrowedWorldMatrix(wt).Data(), ColliderSource(e), capsule.radius,
                            capsule.halfHeight, capsule.axis);
        });
    world.Query<ECS::Read<WorldTransform>, ECS::Read<PhysicsCollider>, ECS::Read<Components::PlaneColliderShape>,
                ECS::Optional<PhysicsColliderOwner>>()
        .Each([&](ECS::EntityHandle e, const WorldTransform& wt, const PhysicsCollider& collider,
                  const Components::PlaneColliderShape& plane, const PhysicsColliderOwner* owner) {
            if (IsBakedCollider(world, e, collider, owner))
                AppendPlane(geometry, NarrowedWorldMatrix(wt).Data(), ColliderSource(e), plane);
        });
}

// An entity that moves on its own: a navigation agent, or a physics body that is not Static.
bool MovesOnItsOwn(const ECS::World& world, ECS::EntityHandle entity)
{
    if (world.HasComponent<Components::NavigationAgent>(entity))
        return true;
    const auto* body = world.GetComponent<Components::PhysicsBody>(entity);
    return body && body->motionType != Physics::MotionType::Static;
}

// The policy that admits a mesh renderer into the bake (Auto or Include), or none when the bake
// leaves it out. The one rule the gathered geometry, its fitted bounds and the models a bake waits
// for all read. The policy is the NavigationSource on the mesh's entity, else on its nearest
// ancestor that has one, else Auto: prefab children and the submesh entities a model makes hang
// under the entity that authored it. Exclude leaves the mesh out. Include admits it unless an
// entity between the mesh and the Include's entity moves on its own. Auto admits it unless any
// entity from the mesh to the root moves on its own. A renderer with no model is baked only from
// the CPU copy that its generated mesh keeps.
std::optional<NavigationSourcePolicy> MeshRendererAdmission(ECS::World& world, ECS::EntityHandle e)
{
    bool authored = false;
    ECS::EntityHandle current = e;
    for (size_t step = 0; step < Components::kMaxHierarchyDepth && current.IsValid(); ++step)
    {
        const auto* source = authored ? nullptr : world.GetComponent<Components::NavigationSource>(current);
        if (source && source->Policy == NavigationSourcePolicy::Exclude)
            return std::nullopt;
        if (source && source->Policy == NavigationSourcePolicy::Include)
            return NavigationSourcePolicy::Include;
        authored = authored || source;
        // Below an Include, or anywhere under Auto, an entity that moves takes the mesh with it.
        if (MovesOnItsOwn(world, current))
            return std::nullopt;
        const auto* parent = world.GetComponent<Components::Parent>(current);
        if (!parent)
            break;
        current = parent->Entity;
    }
    return NavigationSourcePolicy::Auto;
}

// The kept CPU copies of the generated meshes that renderers with no model draw, by mesh handle.
using GeneratedMeshes = FlatMap<uint64, std::shared_ptr<const Mesh>>;

// A renderer with no model draws a mesh the engine generated. Only an entry that keeps its CPU copy
// is a source (a spline chunk, or a primitive that a script creates): an HLOD proxy keeps none, and
// it stands for member meshes that the bake reads already.
bool IsGeneratedNavigationMesh(const Rendering::MeshGPUEntry* entry)
{
    return entry && entry->cpuMesh;
}

// Another system of the wave can register or unregister a mesh during the gather. The TableScope
// holds those writers off only while the lookups copy the owners, never across the ECS walk; the
// copied owners keep each mesh alive after it. A retired chunk's handle finds no entry.
GeneratedMeshes SnapshotGeneratedMeshes(ECS::World& world, const Rendering::MeshGPURegistry* registry)
{
    GeneratedMeshes meshes;
    if (!registry)
        return meshes;
    std::vector<Components::MeshRenderer> generatedRenderers;
    world.Query<ECS::Read<Components::MeshRenderer>>().Each([&](const Components::MeshRenderer& renderer) {
        if (renderer.modelAssetGuid.IsNull() && renderer.meshGpuHandleId != 0u)
            generatedRenderers.push_back(renderer);
    });
    const Rendering::MeshGPURegistry::TableScope scope(*registry);
    for (const Components::MeshRenderer& renderer : generatedRenderers)
    {
        const Rendering::MeshGPUEntry* entry = Engine::Renderer::FindRendererMeshEntry(*registry, renderer);
        if (IsGeneratedNavigationMesh(entry))
            meshes.InsertOrAssign(renderer.meshGpuHandleId, entry->cpuMesh);
    }
    return meshes;
}

// Records each admitted mesh renderer as an instance of the mesh it shows, copying each mesh the first
// time it is seen.
class MeshInstanceWriter
{
  public:
    MeshInstanceWriter(NavigationGeometrySources& sources, AssetManager* assets, const GeneratedMeshes& generated)
        : m_Sources(sources), m_Assets(assets), m_Generated(generated)
    {
    }

    // Records the submesh the renderer on `e` shows, at its WorldTransform, which already places the
    // submesh's node; nothing when the renderer is not admitted or its model is not resident.
    void AddRenderer(ECS::World& world, ECS::EntityHandle e, const Components::MeshRenderer& renderer,
                     const Components::WorldTransform& wt)
    {
        const std::optional<NavigationSourcePolicy> admission = MeshRendererAdmission(world, e);
        if (!admission)
            return;
        const BakeSource source{e, BakeSourceKind::Mesh, *admission};
        const Mathematics::Matrix4x4 worldMatrix = NarrowedWorldMatrix(wt);

        if (renderer.modelAssetGuid.IsNull())
        {
            if (const auto* mesh = m_Generated.Find(renderer.meshGpuHandleId))
                Add(MeshIndexOf(**mesh), worldMatrix, source);
            return;
        }
        const GUID guid = renderer.modelAssetGuid.ToGuid();
        if (Engine::Renderer::PrimitiveGenerator::IsPrimitive(guid))
        {
            Add(PrimitiveIndexOf(guid), worldMatrix, source);
            return;
        }
        if (!m_Assets)
            return;
        const auto asset = m_Assets->GetAsset(guid);
        if (!asset || !asset->IsLoaded() || asset->GetType() != AssetType::Model)
            return;
        const auto& model = static_cast<const ModelAsset&>(*asset);
        if (model.GetMeshCount() == 0)
            return;
        const uint32 submesh = Engine::Renderer::ResolveSubmeshIndex(guid, model, renderer);
        if (submesh < model.GetMeshCount())
            Add(MeshIndexOf(model.GetMesh(submesh)), worldMatrix, source);
    }

  private:
    // The index of `mesh` in the snapshot's mesh list, copied the first time its address is seen. A
    // model's submesh and a generated mesh stay at one address while the snapshot runs.
    uint32 MeshIndexOf(const Mesh& mesh)
    {
        if (const uint32* known = m_MeshIndices.Find(&mesh))
            return *known;
        const uint32 meshIndex = AppendCopy(mesh);
        m_MeshIndices.InsertOrAssign(&mesh, meshIndex);
        return meshIndex;
    }

    // Primitives have no asset behind them: each is generated and copied once per snapshot.
    uint32 PrimitiveIndexOf(const GUID& guid)
    {
        if (const uint32* known = m_PrimitiveIndices.Find(guid))
            return *known;
        const uint32 meshIndex = AppendCopy(Engine::Renderer::PrimitiveGenerator::GenerateByGuid(guid));
        m_PrimitiveIndices.InsertOrAssign(guid, meshIndex);
        return meshIndex;
    }

    uint32 AppendCopy(const Mesh& mesh)
    {
        m_Sources.Meshes.push_back(CopyMesh(mesh));
        return static_cast<uint32>(m_Sources.Meshes.size() - 1);
    }

    void Add(uint32 meshIndex, const Mathematics::Matrix4x4& world, const BakeSource& source)
    {
        if (m_Sources.Meshes[meshIndex].Indices.empty())
            return;
        NavigationGeometrySources::MeshInstance instance;
        instance.Mesh = meshIndex;
        instance.World = world;
        instance.Source = source;
        m_Sources.Instances.push_back(instance);
    }

    NavigationGeometrySources& m_Sources;
    AssetManager* m_Assets;
    const GeneratedMeshes& m_Generated;
    // Each distinct model submesh or generated mesh, by address, to its index in the snapshot's mesh list.
    FlatMap<const void*, uint32> m_MeshIndices;
    // Each primitive, by GUID, to its index in the snapshot's mesh list.
    FlatMap<GUID, uint32> m_PrimitiveIndices;
};

// Each MeshRenderer entity contributes its own submesh once.
void SnapshotMeshRenderers(ECS::World& world, const Rendering::MeshGPURegistry* registry,
                           NavigationGeometrySources& sources)
{
    const GeneratedMeshes generated = SnapshotGeneratedMeshes(world, registry);
    MeshInstanceWriter writer(sources, EngineCore::GetInstance().TryGetAssetManager(), generated);
    world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::WorldTransform>>().Each(
        [&world, &writer](ECS::EntityHandle e, const Components::MeshRenderer& renderer,
                          const Components::WorldTransform& wt) { writer.AddRenderer(world, e, renderer, wt); });
}

// Where BuildNavigationStaticGeometry writes each mesh instance: its first vertex float and first index
// in the geometry's arrays, as appending the instances one by one would place them.
struct InstancePlacement
{
    std::vector<size_t> FirstVertex;
    std::vector<size_t> FirstIndex;
};

// Writes the instances [begin, end) of `sources` into `geometry` at `placement`, transformed to world
// space, and each one's world bounds into `bounds`.
void TransformInstances(const NavigationGeometrySources& sources, const InstancePlacement& placement, size_t begin,
                        size_t end, NavigationStaticGeometry& geometry, std::vector<NavigationBounds>& bounds)
{
    for (size_t i = begin; i < end; ++i)
    {
        const auto& instance = sources.Instances[i];
        const NavigationTriangles& mesh = sources.Meshes[instance.Mesh];
        float32* out = &geometry.Triangles.Vertices[placement.FirstVertex[i]];
        for (size_t v = 0; v < mesh.Vertices.size(); v += 3, out += 3)
        {
            const Mathematics::Vector3 point =
                instance.World.TransformPoint({mesh.Vertices[v], mesh.Vertices[v + 1], mesh.Vertices[v + 2]});
            out[0] = point.x;
            out[1] = point.y;
            out[2] = point.z;
            bounds[i].Include(out);
        }
        const uint32 baseVertex = static_cast<uint32>(placement.FirstVertex[i] / 3);
        uint32* index = &geometry.Triangles.Indices[placement.FirstIndex[i]];
        for (const uint32 local : mesh.Indices)
            *index++ = baseVertex + local;
    }
}

} // namespace

void NavigationBounds::Include(const float32 point[3])
{
    for (int axis = 0; axis < 3; ++axis)
    {
        Min[axis] = Empty ? point[axis] : std::min(Min[axis], point[axis]);
        Max[axis] = Empty ? point[axis] : std::max(Max[axis], point[axis]);
    }
    Empty = false;
}

void NavigationBounds::Include(const NavigationBounds& other)
{
    if (other.Empty)
        return;
    Include(other.Min);
    Include(other.Max);
}

bool NavigationBounds::OverlapsXZ(float32 minX, float32 minZ, float32 maxX, float32 maxZ) const
{
    return !Empty && Min[0] <= maxX && Max[0] >= minX && Min[2] <= maxZ && Max[2] >= minZ;
}

void NavigationTriangles::Clear()
{
    Vertices.clear();
    Indices.clear();
}

void NavigationStaticGeometry::AppendOverlapping(float32 minX, float32 minZ, float32 maxX, float32 maxZ,
                                                 NavigationTriangles& out) const
{
    // Items index into the shared vertex array; their vertices are copied once each, renumbered.
    std::unordered_map<uint32, uint32> remap;
    for (const Item& item : Items)
    {
        if (!item.Bounds.OverlapsXZ(minX, minZ, maxX, maxZ))
            continue;
        for (uint32 i = 0; i < item.IndexCount; ++i)
        {
            const uint32 source = Triangles.Indices[item.FirstIndex + i];
            auto [it, inserted] = remap.try_emplace(source, out.VertexCount());
            if (inserted)
            {
                const float32* v = &Triangles.Vertices[static_cast<size_t>(source) * 3];
                out.Vertices.insert(out.Vertices.end(), v, v + 3);
            }
            out.Indices.push_back(it->second);
        }
    }
}

uint64 NavigationStaticGeometry::HashOverlapping(float32 minX, float32 minZ, float32 maxX, float32 maxZ) const
{
    std::vector<uint64> itemHashes;
    for (const Item& item : Items)
    {
        if (!item.Bounds.OverlapsXZ(minX, minZ, maxX, maxZ))
            continue;
        uint64 hash = Hashing::kFnv1a64OffsetBasis;
        for (uint32 i = 0; i < item.IndexCount; ++i)
        {
            const uint32 source = Triangles.Indices[item.FirstIndex + i];
            hash = Hashing::Fnv1a64(&Triangles.Vertices[static_cast<size_t>(source) * 3], 3 * sizeof(float32), hash);
        }
        itemHashes.push_back(hash);
    }
    std::sort(itemHashes.begin(), itemHashes.end());
    return Hashing::Fnv1a64(itemHashes.data(), itemHashes.size() * sizeof(uint64));
}

NavigationGeometrySources SnapshotNavigationGeometrySources(ECS::World& world, const Rendering::MeshGPURegistry* meshes)
{
    NavigationGeometrySources sources;
    SnapshotColliders(world, sources.Shapes);
    SnapshotMeshRenderers(world, meshes, sources);
    return sources;
}

NavigationStaticGeometry BuildNavigationStaticGeometry(NavigationGeometrySources sources,
                                                       JobSystem::WorkStealingThreadPool* pool,
                                                       const std::atomic<bool>* cancelled)
{
    NavigationStaticGeometry geometry = std::move(sources.Shapes);
    const size_t instanceCount = sources.Instances.size();
    InstancePlacement placement{std::vector<size_t>(instanceCount), std::vector<size_t>(instanceCount)};
    size_t vertexFloats = geometry.Triangles.Vertices.size(), indices = geometry.Triangles.Indices.size();
    for (size_t i = 0; i < instanceCount; ++i)
    {
        const NavigationTriangles& mesh = sources.Meshes[sources.Instances[i].Mesh];
        placement.FirstVertex[i] = vertexFloats;
        placement.FirstIndex[i] = indices;
        vertexFloats += mesh.Vertices.size();
        indices += mesh.Indices.size();
    }
    geometry.Triangles.Vertices.resize(vertexFloats);
    geometry.Triangles.Indices.resize(indices);

    std::vector<NavigationBounds> bounds(instanceCount);
    JobSystem::ParallelFor(
        pool, instanceCount,
        [&sources, &placement, &geometry, &bounds, cancelled](size_t begin, size_t end) {
            if (cancelled && cancelled->load(std::memory_order_relaxed))
                return false;
            TransformInstances(sources, placement, begin, end, geometry, bounds);
            return true;
        },
        kMinInstancesPerChunk);

    for (size_t i = 0; i < instanceCount; ++i)
    {
        NavigationStaticGeometry::Item item;
        item.FirstIndex = static_cast<uint32>(placement.FirstIndex[i]);
        item.IndexCount = static_cast<uint32>(sources.Meshes[sources.Instances[i].Mesh].Indices.size());
        item.Bounds = bounds[i];
        item.Source = sources.Instances[i].Source;
        geometry.Items.push_back(item);
        geometry.Bounds.Include(bounds[i]);
    }
    return geometry;
}

NavigationStaticGeometry GatherNavigationStaticGeometry(ECS::World& world, const Rendering::MeshGPURegistry* meshes)
{
    return BuildNavigationStaticGeometry(SnapshotNavigationGeometrySources(world, meshes), nullptr, nullptr);
}

const Rendering::MeshGPURegistry* FindEngineMeshRegistry()
{
    const auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    return renderServices ? &renderServices->GetMeshGPURegistry() : nullptr;
}

std::vector<GUID> FindUnloadedNavigationModels(ECS::World& world)
{
    std::vector<GUID> models;
    AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!assets)
        return models;
    world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle e, const Components::MeshRenderer& renderer, const Components::WorldTransform&) {
            // A generated mesh has no asset to wait for.
            if (renderer.modelAssetGuid.IsNull() || !MeshRendererAdmission(world, e))
                return;
            const GUID guid = renderer.modelAssetGuid.ToGuid();
            if (Engine::Renderer::PrimitiveGenerator::IsPrimitive(guid) || assets->IsAssetLoaded(guid))
                return;
            if (std::find(models.begin(), models.end(), guid) == models.end())
                models.push_back(guid);
        });
    return models;
}

bool NavigationTerrainSource::OverlapsXZ(float32 minX, float32 minZ, float32 maxX, float32 maxZ) const
{
    if (!Query.IsValid())
        return false;
    const float32 originX = Query.FootprintOriginX();
    const float32 originZ = Query.FootprintOriginZ();
    return originX <= maxX && originX + Query.FootprintSizeX() >= minX && originZ <= maxZ &&
           originZ + Query.FootprintSizeZ() >= minZ;
}

bool NavigationTerrainSource::ReachesPlanetRelief(const NavigationBounds& bounds) const
{
    if (!Spherical || bounds.Empty)
        return false;
    // The box holds a point of the relief when its nearest point to the center is inside the outer
    // radius and its farthest point is outside the inner radius.
    float64 nearestSquared = 0.0;
    float64 farthestSquared = 0.0;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float64 low = bounds.Min[axis];
        const float64 high = bounds.Max[axis];
        const float64 nearest = std::clamp(0.0, low, high);
        const float64 farthest = std::max(std::abs(low), std::abs(high));
        nearestSquared += nearest * nearest;
        farthestSquared += farthest * farthest;
    }
    const float64 inner = PlanetRelief.Inner;
    const float64 outer = PlanetRelief.Outer;
    return nearestSquared <= outer * outer && farthestSquared >= inner * inner;
}

NavigationTerrainSource ResolveNavigationTerrainSource(ECS::World& world,
                                                       const Components::WorldTransform* surfaceTransform)
{
    NavigationTerrainSource source;
    const Mathematics::Vector3 origin =
        surfaceTransform ? surfaceTransform->GetPosition() - Mathematics::WorldPosition{} : Mathematics::Vector3{};
    source.Query = TerrainECS::ResolveSurfaceHeightQuery(world, origin);
    const TerrainECS::SurfaceHeightTerrain terrain = TerrainECS::FindSurfaceHeightTerrain(world, origin);
    source.Spherical = terrain.Terrain && terrain.Terrain->Domain == Components::TerrainDomain::Spherical;
    if (source.Spherical)
    {
        source.PlanetRelief = TerrainECS::PlanetReliefShellOf(
            terrain.Terrain->PlanetRadius, terrain.Relief ? *terrain.Relief : Components::TerrainPlanetRelief{});
    }
    if (!source.Query.IsValid() && !source.Spherical)
    {
        bool first = true;
        world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
            [&](const Components::Terrain& other, const Components::WorldTransform&) {
                if (first)
                    source.Loading = other.Domain != Components::TerrainDomain::Spherical;
                first = false;
            });
    }
    return source;
}

NavigationBounds NavigationSurfaceBounds(const Components::NavigationSurface& surface, const Components::WorldTransform* transform,
                                         const NavigationTerrainSource& terrain,
                                         const NavigationStaticGeometry& geometry)
{
    NavigationBounds bounds;
    if (!surface.FitBounds)
    {
        const Mathematics::Vector3 origin =
            transform ? transform->GetPosition() - Mathematics::WorldPosition{} : Mathematics::Vector3{};
        const Mathematics::Vector3 center = origin + surface.Center;
        const Mathematics::Vector3 extent(std::abs(surface.Extent.x), std::abs(surface.Extent.y), std::abs(surface.Extent.z));
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            bounds.Min[axis] = center[axis] - extent[axis];
            bounds.Max[axis] = center[axis] + extent[axis];
        }
        bounds.Empty = !(extent[0] > 0.0f && extent[2] > 0.0f);
        return bounds;
    }
    if (terrain.Query.IsValid())
    {
        const auto& query = terrain.Query;
        // The heights the terrain holds, which reach below its transform where it is lowered.
        float32 lowY = 0.0f;
        float32 highY = 0.0f;
        query.HeightRange(lowY, highY);
        const float32 low[3] = {query.FootprintOriginX(), lowY, query.FootprintOriginZ()};
        const float32 high[3] = {low[0] + query.FootprintSizeX(), highY, low[2] + query.FootprintSizeZ()};
        bounds.Include(low);
        bounds.Include(high);
        return bounds;
    }
    return geometry.Bounds;
}

uint32 NavigationCellsAlong(float32 length, float32 cellSize)
{
    return std::max(1u, static_cast<uint32>(std::ceil(length / cellSize)));
}

NavigationBakeExtent NavigationBakeExtentOf(const Components::NavigationSurface& surface,
                                            const NavigationBounds& bounds)
{
    if (!(surface.CellSize > 0.0f))
        return {};
    const float32 unit = surface.Mode == Components::NavigationSurfaceMode::Grid
                             ? surface.CellSize
                             : static_cast<float32>(Pathfinding::kNavMeshTileCells) * surface.CellSize;
    return {NavigationCellsAlong(bounds.Max[0] - bounds.Min[0], unit),
            NavigationCellsAlong(bounds.Max[2] - bounds.Min[2], unit)};
}

uint32 TerrainLatticeStride(const NavigationTerrainSource& terrain, float32 cellSize)
{
    const float32 spacing = std::min(terrain.Query.LatticeSpacingX(), terrain.Query.LatticeSpacingZ());
    if (!(spacing > 0.0f) || !(cellSize > spacing))
        return 1;
    return static_cast<uint32>(std::floor(cellSize / spacing));
}

bool AppendTerrainLattice(const NavigationTerrainSource& terrain, float32 minX, float32 minZ, float32 maxX,
                          float32 maxZ, uint32 stride, NavigationTriangles& out, Mathematics::Vector3& outWaitPoint)
{
    const auto& query = terrain.Query;
    const float32 spacingX = query.LatticeSpacingX() * static_cast<float32>(stride);
    const float32 spacingZ = query.LatticeSpacingZ() * static_cast<float32>(stride);
    if (!terrain.OverlapsXZ(minX, minZ, maxX, maxZ) || !(spacingX > 0.0f) || !(spacingZ > 0.0f))
        return true;

    const float32 originX = query.FootprintOriginX();
    const float32 originZ = query.FootprintOriginZ();
    const int32 lastX = static_cast<int32>(std::floor(query.FootprintSizeX() / spacingX + 1.0e-3f));
    const int32 lastZ = static_cast<int32>(std::floor(query.FootprintSizeZ() / spacingZ + 1.0e-3f));
    const int32 x0 = std::clamp(static_cast<int32>(std::floor((minX - originX) / spacingX)), 0, lastX);
    const int32 z0 = std::clamp(static_cast<int32>(std::floor((minZ - originZ) / spacingZ)), 0, lastZ);
    const int32 x1 = std::clamp(static_cast<int32>(std::ceil((maxX - originX) / spacingX)), 0, lastX);
    const int32 z1 = std::clamp(static_cast<int32>(std::ceil((maxZ - originZ) / spacingZ)), 0, lastZ);
    const int32 width = x1 - x0 + 1;
    const int32 depth = z1 - z0 + 1;
    if (width < 2 || depth < 2)
        return true;

    const size_t firstVertex = out.Vertices.size();
    const uint32 baseVertex = out.VertexCount();
    out.Vertices.reserve(firstVertex + static_cast<size_t>(width) * depth * 3);
    for (int32 z = z0; z <= z1; ++z)
    {
        for (int32 x = x0; x <= x1; ++x)
        {
            const float32 worldX = originX + static_cast<float32>(x) * spacingX;
            const float32 worldZ = originZ + static_cast<float32>(z) * spacingZ;
            TerrainECS::SurfaceHeightSample sample;
            if (!query.SampleHeight(Mathematics::Vector3(worldX, 0.0f, worldZ),
                                    TerrainECS::SurfaceHeightPolicy::RequireFinest, sample))
            {
                out.Vertices.resize(firstVertex);
                outWaitPoint = Mathematics::Vector3(worldX, 0.0f, worldZ);
                return false;
            }
            out.Vertices.insert(out.Vertices.end(), {worldX, sample.Height, worldZ});
        }
    }
    out.Indices.reserve(out.Indices.size() + static_cast<size_t>(width - 1) * (depth - 1) * 6);
    for (int32 z = 0; z + 1 < depth; ++z)
    {
        for (int32 x = 0; x + 1 < width; ++x)
        {
            const uint32 a = baseVertex + static_cast<uint32>(z * width + x);
            const uint32 b = a + 1;
            const uint32 c = a + static_cast<uint32>(width);
            const uint32 d = c + 1;
            // Wound to face up by Recast's convention (rcMarkWalkableTriangles).
            out.Indices.insert(out.Indices.end(), {a, c, b, b, c, d});
        }
    }
    return true;
}

} // namespace GameEngine::PathfindingECS
