#include "NavigationSourceCollector.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
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
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>

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

void AppendMesh(NavigationStaticGeometry& geometry, const float32 world[16], const BakeSource& source, const Mesh& mesh)
{
    if (mesh.Vertices.empty() || mesh.Indices.size() < 3)
        return;
    ItemWriter writer(geometry, world, source);
    for (const auto& vertex : mesh.Vertices)
        writer.AddVertex(vertex.Position[0], vertex.Position[1], vertex.Position[2]);
    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
        writer.AddTriangle(mesh.Indices[i], mesh.Indices[i + 1], mesh.Indices[i + 2]);
}

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

void GatherColliders(ECS::World& world, NavigationStaticGeometry& geometry)
{
    using Components::PhysicsCollider;
    using Components::PhysicsColliderOwner;
    using Components::WorldTransform;

    world.Query<ECS::Read<WorldTransform>, ECS::Read<PhysicsCollider>, ECS::Read<Components::BoxColliderShape>,
                ECS::Optional<PhysicsColliderOwner>>()
        .Each([&](ECS::EntityHandle e, const WorldTransform& wt, const PhysicsCollider& collider,
                  const Components::BoxColliderShape& box, const PhysicsColliderOwner* owner) {
            if (IsBakedCollider(world, e, collider, owner))
                AppendBox(geometry, wt.matrix, ColliderSource(e), box);
        });
    world.Query<ECS::Read<WorldTransform>, ECS::Read<PhysicsCollider>, ECS::Read<Components::SphereColliderShape>,
                ECS::Optional<PhysicsColliderOwner>>()
        .Each([&](ECS::EntityHandle e, const WorldTransform& wt, const PhysicsCollider& collider,
                  const Components::SphereColliderShape& sphere, const PhysicsColliderOwner* owner) {
            if (IsBakedCollider(world, e, collider, owner))
                AppendRound(geometry, wt.matrix, ColliderSource(e), sphere.radius, 0.0f, 1);
        });
    world.Query<ECS::Read<WorldTransform>, ECS::Read<PhysicsCollider>, ECS::Read<Components::CapsuleColliderShape>,
                ECS::Optional<PhysicsColliderOwner>>()
        .Each([&](ECS::EntityHandle e, const WorldTransform& wt, const PhysicsCollider& collider,
                  const Components::CapsuleColliderShape& capsule, const PhysicsColliderOwner* owner) {
            if (IsBakedCollider(world, e, collider, owner))
                AppendRound(geometry, wt.matrix, ColliderSource(e), capsule.radius, capsule.halfHeight, capsule.axis);
        });
    world.Query<ECS::Read<WorldTransform>, ECS::Read<PhysicsCollider>, ECS::Read<Components::PlaneColliderShape>,
                ECS::Optional<PhysicsColliderOwner>>()
        .Each([&](ECS::EntityHandle e, const WorldTransform& wt, const PhysicsCollider& collider,
                  const Components::PlaneColliderShape& plane, const PhysicsColliderOwner* owner) {
            if (IsBakedCollider(world, e, collider, owner))
                AppendPlane(geometry, wt.matrix, ColliderSource(e), plane);
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
// ancestor that has one, else Auto: blueprint children and the submesh entities a model makes hang
// under the entity that authored it. Exclude leaves the mesh out. Include admits it unless an
// entity between the mesh and the Include's entity moves on its own. Auto admits it unless any
// entity from the mesh to the root moves on its own.
std::optional<NavigationSourcePolicy> MeshRendererAdmission(ECS::World& world, ECS::EntityHandle e,
                                                            const Components::MeshRenderer& renderer)
{
    if (renderer.modelAssetGuid.IsNull())
        return std::nullopt;
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

// Each MeshRenderer entity contributes its own submesh once, at its WorldTransform, which already
// places the submesh's node.
void GatherMeshRenderers(ECS::World& world, NavigationStaticGeometry& geometry)
{
    AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager();
    // Primitives are generated meshes with no asset behind them; each is generated once per bake.
    std::unordered_map<GUID, Mesh> primitives;

    world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle e, const Components::MeshRenderer& renderer, const Components::WorldTransform& wt) {
            const std::optional<NavigationSourcePolicy> admission = MeshRendererAdmission(world, e, renderer);
            if (!admission)
                return;
            const BakeSource source{e, BakeSourceKind::Mesh, *admission};

            const GUID guid = renderer.modelAssetGuid.ToGuid();
            if (Engine::Renderer::PrimitiveGenerator::IsPrimitive(guid))
            {
                auto [it, inserted] = primitives.try_emplace(guid);
                if (inserted)
                    it->second = Engine::Renderer::PrimitiveGenerator::GenerateByGuid(guid);
                AppendMesh(geometry, wt.matrix, source, it->second);
                return;
            }
            if (!assets)
                return;
            const auto asset = assets->GetAsset(guid);
            if (!asset || !asset->IsLoaded() || asset->GetType() != AssetType::Model)
                return;
            const auto& model = static_cast<const ModelAsset&>(*asset);
            if (model.GetMeshCount() == 0)
                return;
            const uint32 submesh = Engine::Renderer::ResolveSubmeshIndex(guid, model, renderer);
            if (submesh < model.GetMeshCount())
                AppendMesh(geometry, wt.matrix, source, model.GetMesh(submesh));
        });
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

NavigationStaticGeometry GatherNavigationStaticGeometry(ECS::World& world)
{
    NavigationStaticGeometry geometry;
    GatherColliders(world, geometry);
    GatherMeshRenderers(world, geometry);
    return geometry;
}

std::vector<GUID> FindUnloadedNavigationModels(ECS::World& world)
{
    std::vector<GUID> models;
    AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!assets)
        return models;
    world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle e, const Components::MeshRenderer& renderer, const Components::WorldTransform&) {
            if (!MeshRendererAdmission(world, e, renderer))
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

NavigationTerrainSource ResolveNavigationTerrainSource(ECS::World& world)
{
    NavigationTerrainSource source;
    source.Query = TerrainECS::ResolvePlanarHeightQuery(world);
    if (!source.Query.IsValid())
    {
        bool first = true;
        world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
            [&](const Components::Terrain& terrain, const Components::WorldTransform&) {
                if (first)
                {
                    source.Spherical = terrain.Domain == Components::TerrainDomain::Spherical;
                    source.Loading = !source.Spherical;
                }
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
        const float32 position[3] = {transform ? transform->matrix[12] : 0.0f, transform ? transform->matrix[13] : 0.0f,
                                     transform ? transform->matrix[14] : 0.0f};
        const float32 center[3] = {surface.Center.x, surface.Center.y, surface.Center.z};
        const float32 extent[3] = {std::abs(surface.Extent.x), std::abs(surface.Extent.y), std::abs(surface.Extent.z)};
        for (int axis = 0; axis < 3; ++axis)
        {
            bounds.Min[axis] = position[axis] + center[axis] - extent[axis];
            bounds.Max[axis] = position[axis] + center[axis] + extent[axis];
        }
        bounds.Empty = !(extent[0] > 0.0f && extent[2] > 0.0f);
        return bounds;
    }
    if (terrain.Query.IsValid())
    {
        const auto& query = terrain.Query;
        const float32 low[3] = {query.FootprintOriginX(), query.OriginY, query.FootprintOriginZ()};
        const float32 high[3] = {low[0] + query.FootprintSizeX(), query.OriginY + query.HeightScale,
                                 low[2] + query.FootprintSizeZ()};
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
                          float32 maxZ, uint32 stride, NavigationTriangles& out, int32& outTileX, int32& outTileZ)
{
    const auto& query = terrain.Query;
    const float32 spacingX = query.LatticeSpacingX() * static_cast<float32>(stride);
    const float32 spacingZ = query.LatticeSpacingZ() * static_cast<float32>(stride);
    if (!terrain.OverlapsXZ(minX, minZ, maxX, maxZ) || !(spacingX > 0.0f) || !(spacingZ > 0.0f))
        return true;

    const float32 originX = query.LatticeOriginX();
    const float32 originZ = query.LatticeOriginZ();
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

    // A tiled residency answers no sample on the footprint's far edge, which belongs to no tile, so
    // corners there are read a hair inside it.
    const float32 maxSampleX = originX + query.FootprintSizeX() - 1.0e-3f * spacingX;
    const float32 maxSampleZ = originZ + query.FootprintSizeZ() - 1.0e-3f * spacingZ;
    const size_t firstVertex = out.Vertices.size();
    const uint32 baseVertex = out.VertexCount();
    out.Vertices.reserve(firstVertex + static_cast<size_t>(width) * depth * 3);
    for (int32 z = z0; z <= z1; ++z)
    {
        for (int32 x = x0; x <= x1; ++x)
        {
            const float32 worldX = originX + static_cast<float32>(x) * spacingX;
            const float32 worldZ = originZ + static_cast<float32>(z) * spacingZ;
            float32 height = 0.0f;
            if (!query.SampleHeight(std::min(worldX, maxSampleX), std::min(worldZ, maxSampleZ), height))
            {
                out.Vertices.resize(firstVertex);
                if (!query.TileCoordinateAt(worldX, worldZ, outTileX, outTileZ))
                    outTileX = outTileZ = 0;
                return false;
            }
            out.Vertices.insert(out.Vertices.end(), {worldX, height, worldZ});
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
