#include "Scene/TerrainPicking.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "CBTTerrainECS/CBTRenderFeature.h"
#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/RenderServices.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/TerrainService.h"

namespace GameEngine::Scene
{

namespace
{

using Mathematics::Vector3;

// The nearest planar terrain hit so far.
struct PlanarPickBest
{
    float32 Distance = 0.0f;
    Vector3 Position{};
    Vector3 Normal{0.0f, 1.0f, 0.0f};
    ECS::EntityHandle Entity{};
    bool Hit = false;
};

// Marches a pick past its first hit at most this many more times: each march after the first reads
// the finest page fetched under the previous hit, which can move the hit onto a neighboring page.
constexpr uint32 kPickRefinements = 2;

// World height of `surface` at (worldX, worldZ) as a pick reads it (AcceptCoarse: the finest page on
// the CPU there, else a coarser one, so a pick lands wherever the terrain is drawn). False outside
// the footprint and where no page is on the CPU yet (a height store still cooking).
bool SamplePickHeight(const TerrainECS::SurfaceHeightQuery& surface, float32 worldX, float32 worldZ, float32& outY)
{
    TerrainECS::SurfaceHeightSample sample;
    if (!surface.SampleHeight(Vector3(worldX, 0.0f, worldZ), TerrainECS::SurfaceHeightPolicy::AcceptCoarse, sample))
        return false;
    outY = sample.Height;
    return true;
}

// Whether `surface`'s answer at `point` comes from a coarser level than the terrain's finest there.
bool ReadsCoarse(const TerrainECS::SurfaceHeightQuery& surface, const Vector3& point)
{
    TerrainECS::SurfaceHeightSample sample;
    return surface.SampleHeight(point, TerrainECS::SurfaceHeightPolicy::AcceptCoarse, sample) &&
           sample.Detail == TerrainECS::SurfaceHeightDetail::Coarse;
}

// Central-difference normal of `surface` at (worldX, worldZ), stepped at the terrain's own sample spacing
// (`samplesPerMeter`). +Y wherever a tap has no height.
Vector3 PickSurfaceNormal(const TerrainECS::SurfaceHeightQuery& surface, float32 samplesPerMeter, float32 worldX,
                          float32 worldZ)
{
    const float32 step = 1.0f / std::clamp(samplesPerMeter, 0.05f, 20.0f);
    float32 heightPlusX = 0.0f, heightMinusX = 0.0f, heightPlusZ = 0.0f, heightMinusZ = 0.0f;
    if (!SamplePickHeight(surface, worldX + step, worldZ, heightPlusX) ||
        !SamplePickHeight(surface, worldX - step, worldZ, heightMinusX) ||
        !SamplePickHeight(surface, worldX, worldZ + step, heightPlusZ) ||
        !SamplePickHeight(surface, worldX, worldZ - step, heightMinusZ))
        return Vector3(0.0f, 1.0f, 0.0f);
    // Heightfield y = h(x, z) has normal along (-dh/dx, 1, -dh/dz).
    const float32 inverseSpan = 1.0f / (2.0f * step);
    const Vector3 normal((heightMinusX - heightPlusX) * inverseSpan, 1.0f, (heightMinusZ - heightPlusZ) * inverseSpan);
    return normal * (1.0f / std::sqrt(Vector3::Dot(normal, normal)));
}

// Marches `ray` against one planar terrain and keeps the hit in `best` when it is nearer.
//
// A paged terrain is marched on its CPU pages. Where the hit read a coarse page (no CPU window
// reaches it), the finest page under the hit is fetched now (TerrainService::FetchFinestHeightPage:
// one page's read and decode for the click) and the ray marched again, so a pick lands on the finest
// height the terrain has, as far from the camera as it is.
void PickPlanarTerrain(TerrainECS::TerrainService& service, const Mathematics::Ray3D& ray, ECS::EntityHandle entity,
                       const Components::Terrain& terrain, const Components::WorldTransform& worldTransform,
                       PlanarPickBest& best)
{
    if (terrain.Domain != Components::TerrainDomain::Planar)
        return;
    // No query: the terrain has no height data yet (extraction has not run for it), so there is no
    // surface to hit and nothing drawn to click.
    TerrainECS::SurfaceHeightQuery surface = service.SurfaceQuery(terrain, worldTransform);
    if (!surface.IsValid())
        return;

    // Terrain world AABB: XZ is the footprint; Y spans the range the field holds (HeightRange: a
    // modifier volume drives samples outside the nominal band and the renderer draws them there). A
    // small Y margin keeps a ray grazing the exact peak or trough from being clipped out by rounding.
    // The marcher reach is this box regardless of size, so far-side hits on large terrains land.
    constexpr float32 kHeightMargin = 1.0f;
    float32 lowY = 0.0f;
    float32 highY = 0.0f;
    surface.HeightRange(lowY, highY);
    const Vector3 aabbMin(surface.FootprintOriginX(), lowY - kHeightMargin, surface.FootprintOriginZ());
    const Vector3 aabbMax(surface.FootprintOriginX() + surface.FootprintSizeX(), highY + kHeightMargin,
                          surface.FootprintOriginZ() + surface.FootprintSizeZ());

    float32 hitT = 0.0f;
    Vector3 hitPos{};
    for (uint32 march = 0; march <= kPickRefinements; ++march)
    {
        const auto sampleHeight = [&surface](float32 worldX, float32 worldZ, float32& outY) {
            return SamplePickHeight(surface, worldX, worldZ, outY);
        };
        if (!MarchTerrainSurface(ray, aabbMin, aabbMax, best.Distance, sampleHeight, hitT, hitPos))
            return;
        if (march == kPickRefinements || !ReadsCoarse(surface, hitPos) ||
            !service.FetchFinestHeightPage(terrain, hitPos))
            break;
        surface = service.SurfaceQuery(terrain, worldTransform);
    }
    if (hitT >= best.Distance)
        return;
    best.Distance = hitT;
    best.Position = hitPos;
    best.Normal = PickSurfaceNormal(surface, terrain.SamplesPerMeter, hitPos.x, hitPos.z);
    best.Entity = entity;
    best.Hit = true;
}

// Ray -> every planar terrain's heightfield footprint, nearest hit. The march walks geometry
// only the planar domain draws: a spherical terrain retains its heightfield but renders a
// displaced sphere instead, so marching it would offer a phantom flat target at the entity
// origin. Spherical terrains are handled by RaycastActivePlanet against what is drawn.
bool RaycastPlanarTerrains(const Mathematics::Ray3D& ray,
                           GameEngine::ECS::World& world,
                           float32 maxDistance,
                           TerrainPickHit& outHit)
{
    // The service owns the heightfields, so without it there is nothing planar to march.
    auto* terrainService = TerrainECS::TerrainService::TryGet();
    if (!terrainService)
        return false;

    PlanarPickBest best;
    best.Distance = maxDistance;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [terrainService, &ray, &best](ECS::EntityHandle entity, const Components::Terrain& terrain,
                                      const Components::WorldTransform& worldTransform) {
            PickPlanarTerrain(*terrainService, ray, entity, terrain, worldTransform, best);
        });
    if (!best.Hit)
        return false;

    outHit.Entity        = best.Entity;
    outHit.WorldPosition = best.Position;
    outHit.WorldNormal   = best.Normal;
    outHit.Distance      = best.Distance;
    return true;
}

// The CBT render feature when it is currently drawing a spherical planet. It is the
// authority for both halves of the planet's geometry: GetDomainConfig().PlanetRadius is the
// radius the GPU displaces FROM, and SampleSphereSurfaceHeight is the CPU mirror of the
// height it displaces BY (procedural relief from the active planet tuning + TerrainService's
// editable sculpt layer). Reading the renderer rather than the Terrain component is what
// makes a pick land on the ground the user can see, including mid-edit frames where the
// component has already moved and the drawn geometry has not.
//
// No feature, or a feature not on the spherical domain, means no planet is on screen: there
// is then nothing to pick, and inventing a radius would place hits on a sphere the renderer
// never drew.
const CBTTerrainECS::CBTRenderFeature* ActivePlanetFeature()
{
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    if (!renderServices)
        return nullptr;
    const auto* feature = renderServices->GetFeature<CBTTerrainECS::CBTRenderFeature>();
    if (!feature)
        return nullptr;
    const auto& domain = feature->GetDomainConfig();
    if (domain.DomainMode != CBTTerrain::kDomainSpherical || !(domain.PlanetRadius > 0.0f))
        return nullptr;
    return feature;
}

// Ray -> the one spherical terrain, resolved through the SAME active-terrain resolver the
// renderer tunes from and the sculpt brush routes by, so pick, sculpt and draw can never
// disagree about which terrain is the planet. CBT is single-active-terrain by design, so
// this runs once per cast rather than once per entity.
bool RaycastActivePlanet(const Mathematics::Ray3D& ray,
                         GameEngine::ECS::World& world,
                         float32 maxDistance,
                         TerrainPickHit& outHit)
{
    const CBTTerrainECS::CBTRenderFeature* feature = ActivePlanetFeature();
    if (!feature)
        return false;

    const ECS::EntityHandle entity = CBTTerrainECS::FindActiveTerrainEntity(world);
    if (!entity.IsValid())
        return false;
    const auto* terrain = world.GetComponent<Components::Terrain>(entity);
    if (!terrain || terrain->Domain != Components::TerrainDomain::Spherical)
        return false;

    float32 t = 0.0f;
    Vector3 pos{};
    Vector3 normal{};
    if (!SolvePlanetSurfaceHit(ray, feature->GetDomainConfig().PlanetRadius, maxDistance,
                               [feature](float32 dx, float32 dy, float32 dz) {
                                   return feature->SampleSphereSurfaceHeight(dx, dy, dz);
                               },
                               t, pos, normal))
        return false;

    outHit.Entity        = entity;
    outHit.WorldPosition = pos;
    outHit.WorldNormal   = normal;
    outHit.Distance      = t;
    return true;
}

} // namespace

bool RaycastTerrain(const Mathematics::Ray3D& ray,
                    GameEngine::ECS::World& world,
                    float32 maxDistance,
                    TerrainPickHit& outHit)
{
    bool    anyHit = false;
    float32 bestT  = std::min(maxDistance, std::numeric_limits<float32>::max());
    TerrainPickHit best{};
    TerrainPickHit hit{};

    if (RaycastPlanarTerrains(ray, world, bestT, hit))
    {
        best   = hit;
        bestT  = hit.Distance;
        anyHit = true;
    }
    // Capped at bestT, so a planar terrain (or, through RaycastScene, a mesh) in front of the
    // planet still wins.
    if (RaycastActivePlanet(ray, world, bestT, hit))
    {
        best   = hit;
        bestT  = hit.Distance;
        anyHit = true;
    }

    if (!anyHit)
        return false;

    outHit = best;
    return true;
}

}
