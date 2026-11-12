// The reference surface of a flatten on a closed spline area (TerrainVolumeShape::SplineArea with
// UseVolumeHeight): the interior is the harmonic fill of the rim's heights, and where a road's
// band crosses the rim the rim takes the road's grade across the road's full width.
//
// The fixtures are the ground a paved area on Ground Authority Owns provisions: the area's volume
// at priority 10 in the "Roads" pool with a ground claim, and roads on Grades at priority 20 in the
// shared pool that respect claims. Left-handed, +Y up; the terrain is centred on the origin with
// one sample every 0.5 m, so the rim at x = +-20 m and z = +-20 m lies on sample rows.

#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "PhysicsECS/HeightFieldDataProvider.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

constexpr float32 kWorldSize = 128.0f;
constexpr uint32 kSampleDim = 257;
constexpr float32 kSpacing = kWorldSize / static_cast<float32>(kSampleDim - 1);
constexpr float32 kHeightScale = 64.0f;
constexpr float32 kRimHalf = 20.0f;
constexpr float32 kRoadHalfWidth = 3.0f;
constexpr float32 kFade = 3.0f;
constexpr float32 kAreaPriority = 10.0f;
constexpr float32 kRoadPriority = 20.0f;

struct ScopedServices
{
    ScopedServices()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
        SplineECS::SplineService::Initialize();
    }
    ~ScopedServices()
    {
        SplineECS::SplineService::Shutdown();
        TerrainService::Shutdown();
    }
};

Components::WorldTransform Identity()
{
    return Components::WorldTransform{};
}

float32 SampleWorldX(uint32 column)
{
    return static_cast<float32>(column) * kSpacing - kWorldSize * 0.5f;
}

uint32 ColumnAt(float32 world)
{
    return static_cast<uint32>(std::lround((world + kWorldSize * 0.5f) / kSpacing));
}

// One terrain, one world and one modifier system, with a plaza and its roads authored into it.
class AreaFlattenScene
{
public:
    AreaFlattenScene()
    {
        Terrain::TerrainConfig config{};
        config.HeightmapWidth = kSampleDim;
        config.HeightmapHeight = kSampleDim;
        config.WorldSizeX = kWorldSize;
        config.WorldSizeZ = kWorldSize;
        config.HeightScale = kHeightScale;
        config.LODLevels = 4;
        auto& terrains = TerrainService::Get();
        m_Handle = terrains.CreateTerrain(config);
        TerrainData* data = terrains.GetTerrainData(m_Handle);
        data->Heightfield.FillRegionWithNoise(4.0f, 1.0f, 0, 0, static_cast<int32>(kSampleDim) - 1, static_cast<int32>(kSampleDim) - 1, 5, 42);
        data->MarkFullDirty();
        terrains.RebuildQuadtree(m_Handle);
        data->ResetSplatmapAndCommitRange();

        const ECS::EntityHandle terrain = m_World.CreateEntity();
        Components::Terrain component{};
        component.SizeX = kWorldSize;
        component.SizeZ = kWorldSize;
        component.HeightScale = kHeightScale;
        component.TerrainDataHandle = m_Handle.Index;
        component.TerrainDataGeneration = m_Handle.Generation;
        component.BaseSource = Components::TerrainBaseSource::Flat;
        m_World.AddComponentImmediate<Components::Terrain>(terrain, component);
        m_World.AddComponentImmediate<Components::WorldTransform>(terrain, Identity());
    }

    // A square plaza with its corners at (+-20, y, +-20); `cornerY` runs counter-clockwise from
    // (-20, -20). The rim is a linear spline, so its height is linear along each side.
    ECS::EntityHandle AddPlaza(const float32 (&cornerY)[4])
    {
        return AddPlazaOnRim({
            {-kRimHalf, cornerY[0], -kRimHalf},
            {kRimHalf, cornerY[1], -kRimHalf},
            {kRimHalf, cornerY[2], kRimHalf},
            {-kRimHalf, cornerY[3], kRimHalf},
        });
    }

    // A plaza on the closed linear rim through `rim`.
    ECS::EntityHandle AddPlazaOnRim(const std::vector<Mathematics::Vector3>& rim)
    {
        const ECS::EntityHandle plaza = AddSpline(rim, 0.0f, true);

        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::SplineArea;
        volume.Falloff = kFade;
        volume.Priority = kAreaPriority;
        m_World.AddComponentImmediate<Components::TerrainModifierVolume>(plaza, volume);

        Components::TerrainFlattenEffect flatten{};
        flatten.UseVolumeHeight = true;
        flatten.Blend = Components::TerrainModifierBlend::Average;
        std::memcpy(flatten.PoolGroup, "Roads", 6);
        flatten.RespectClaims = false;
        m_World.AddComponentImmediate<Components::TerrainFlattenEffect>(plaza, flatten);
        m_World.AddComponentImmediate<Components::TerrainGroundClaimEffect>(plaza, Components::TerrainGroundClaimEffect{});
        return plaza;
    }

    // A road on Grades along `points`, graded to its own heights.
    ECS::EntityHandle AddRoad(const std::vector<Mathematics::Vector3>& points)
    {
        const ECS::EntityHandle road = AddSpline(points, kRoadHalfWidth, false);
        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::SplinePath;
        volume.Falloff = kFade;
        volume.Priority = kRoadPriority;
        m_World.AddComponentImmediate<Components::TerrainModifierVolume>(road, volume);

        Components::TerrainFlattenEffect flatten{};
        flatten.UseVolumeHeight = true;
        flatten.Blend = Components::TerrainModifierBlend::Average;
        flatten.RespectClaims = true;
        m_World.AddComponentImmediate<Components::TerrainFlattenEffect>(road, flatten);
        return road;
    }

    void Bake() { m_System.Update(m_World, 1.0f / 60.0f); }

    // The baked ground at a sample, world meters.
    float32 HeightAt(uint32 column, uint32 row) const
    {
        const TerrainData* data = TerrainService::Get().GetTerrainData(m_Handle);
        return data->Heightfield.GetRawSamples()[static_cast<std::size_t>(row) * kSampleDim + column] * kHeightScale;
    }

    std::vector<float32> CopyHeights() const
    {
        const TerrainData* data = TerrainService::Get().GetTerrainData(m_Handle);
        const float32* samples = data->Heightfield.GetRawSamples();
        return std::vector<float32>(samples, samples + data->Heightfield.GetSampleCount());
    }

    // Writes `value` into every sample under the plaza: a previous bake that is wrong everywhere.
    void OverwritePlazaSamples(float32 value)
    {
        TerrainData* data = TerrainService::Get().GetTerrainData(m_Handle);
        float32* samples = data->Heightfield.GetMutableSamples();
        for (uint32 row = ColumnAt(-kRimHalf); row <= ColumnAt(kRimHalf); ++row)
            for (uint32 column = ColumnAt(-kRimHalf); column <= ColumnAt(kRimHalf); ++column)
                samples[static_cast<std::size_t>(row) * kSampleDim + column] = value;
    }

    // A height offset in a small circle far from the plaza and its roads.
    void AddDistantVolume()
    {
        const ECS::EntityHandle entity = m_World.CreateEntity();
        Components::WorldTransform pose{};
        pose.Position.X = 50.0;
        pose.Position.Z = 50.0;
        m_World.AddComponentImmediate<Components::WorldTransform>(entity, pose);
        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::Circle;
        volume.Radius = 4.0f;
        m_World.AddComponentImmediate<Components::TerrainModifierVolume>(entity, volume);
        Components::TerrainHeightOffsetEffect offset{};
        offset.Offset = 1.0f;
        m_World.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(entity, offset);
    }

    TerrainHandle GetHandle() const { return m_Handle; }
    const TerrainModifierSystem& GetSystem() const { return m_System; }
    ECS::World& GetWorld() { return m_World; }

private:
    ECS::EntityHandle AddSpline(const std::vector<Mathematics::Vector3>& points, float32 radius, bool closed)
    {
        auto& splines = SplineECS::SplineService::Get();
        const SplineECS::SplineHandle handle = splines.CreateSpline(Spline::SplineType::Linear, closed);
        Spline::SplineData* data = splines.GetSplineData(handle);
        for (const Mathematics::Vector3& point : points)
            data->AddPoint(point, radius);
        splines.RebuildCache(handle);

        const ECS::EntityHandle entity = m_World.CreateEntity();
        Components::SplineComponent component{};
        component.SplineDataIndex = handle.Index();
        component.SplineDataGeneration = handle.Generation();
        m_World.AddComponentImmediate<Components::SplineComponent>(entity, component);
        m_World.AddComponentImmediate<Components::WorldTransform>(entity, Identity());
        return entity;
    }

    ScopedServices m_Services;
    TerrainHandle m_Handle{};
    ECS::World m_World;
    TerrainModifierSystem m_System;
};

// The bilinear surface through the four corner heights: harmonic, so it is the exact fill of a rim
// whose sides are linear.
float32 BilinearRim(const float32 (&cornerY)[4], float32 x, float32 z)
{
    const float32 u = (x + kRimHalf) / (2.0f * kRimHalf);
    const float32 v = (z + kRimHalf) / (2.0f * kRimHalf);
    return cornerY[0] * (1.0f - u) * (1.0f - v) + cornerY[1] * u * (1.0f - v) + cornerY[2] * u * v
           + cornerY[3] * (1.0f - u) * v;
}

// The samples at least `inset` meters inside the rim.
template <typename Visit>
void ForEachInteriorSample(float32 inset, Visit&& visit)
{
    for (uint32 row = ColumnAt(-kRimHalf + inset); row <= ColumnAt(kRimHalf - inset); ++row)
        for (uint32 column = ColumnAt(-kRimHalf + inset); column <= ColumnAt(kRimHalf - inset); ++column)
            visit(column, row);
}

// A twisted rim: one corner raised 2 m, the sides linear. Its harmonic fill is the bilinear surface.
constexpr float32 kTwistedCorners[4] = {0.0f, 2.0f, 0.0f, 0.0f};

TEST(TerrainAreaFlattenSurface, ASlopedRimGivesTheHarmonicSurface)
{
    AreaFlattenScene scene;
    scene.AddPlaza(kTwistedCorners);
    scene.Bake();

    float32 worst = 0.0f;
    ForEachInteriorSample(1.0f, [&scene, &worst](uint32 column, uint32 row) {
        const float32 expected = BilinearRim(kTwistedCorners, SampleWorldX(column), SampleWorldX(row));
        worst = std::max(worst, std::fabs(scene.HeightAt(column, row) - expected));
    });
    EXPECT_LT(worst, 0.01f) << "the plaza does not follow the harmonic fill of its rim";
}

TEST(TerrainAreaFlattenSurface, ASlopedRimHasNoCreaseInside)
{
    AreaFlattenScene scene;
    scene.AddPlaza(kTwistedCorners);
    scene.Bake();

    // The second difference along both axes. The bilinear fill has none along an axis; a crease
    // shows as a slope change over one sample.
    float32 worst = 0.0f;
    ForEachInteriorSample(1.0f, [&scene, &worst](uint32 column, uint32 row) {
        const float32 h = scene.HeightAt(column, row);
        const float32 alongX = scene.HeightAt(column - 1, row) + scene.HeightAt(column + 1, row) - 2.0f * h;
        const float32 alongZ = scene.HeightAt(column, row - 1) + scene.HeightAt(column, row + 1) - 2.0f * h;
        worst = std::max({worst, std::fabs(alongX), std::fabs(alongZ)});
    });
    EXPECT_LT(worst, 0.002f) << "the plaza creases inside: a second difference of " << worst << " m";
}

// A saddle, y = k (x^2 - z^2). It is harmonic, so a rim on it has the saddle as its fill. A guess
// interpolated along rows and columns misses it by up to 0.36 m, so only the solve finds it.
float32 Saddle(float32 x, float32 z)
{
    constexpr float32 kCurvature = 0.002f;
    return kCurvature * (x * x - z * z);
}

TEST(TerrainAreaFlattenSurface, ASaddleRimGivesTheSaddle)
{
    // One rim point every meter, counter-clockwise from (-20, -20).
    std::vector<Mathematics::Vector3> rim;
    for (float32 along = -kRimHalf; along < kRimHalf; along += 1.0f)
        rim.push_back({along, Saddle(along, -kRimHalf), -kRimHalf});
    for (float32 along = -kRimHalf; along < kRimHalf; along += 1.0f)
        rim.push_back({kRimHalf, Saddle(kRimHalf, along), along});
    for (float32 along = kRimHalf; along > -kRimHalf; along -= 1.0f)
        rim.push_back({along, Saddle(along, kRimHalf), kRimHalf});
    for (float32 along = kRimHalf; along > -kRimHalf; along -= 1.0f)
        rim.push_back({-kRimHalf, Saddle(-kRimHalf, along), along});
    AreaFlattenScene scene;
    scene.AddPlazaOnRim(rim);
    scene.Bake();

    float32 worst = 0.0f;
    ForEachInteriorSample(1.0f, [&scene, &worst](uint32 column, uint32 row) {
        const float32 expected = Saddle(SampleWorldX(column), SampleWorldX(row));
        worst = std::max(worst, std::fabs(scene.HeightAt(column, row) - expected));
    });
    EXPECT_LT(worst, 0.01f) << "the plaza does not follow the harmonic fill of a saddle rim";
}

// A tilted plane, y = 1 + 0.04 x + 0.02 z. It is harmonic, so a rim on it has the plane as its fill.
float32 TiltedPlane(float32 x, float32 z)
{
    return 1.0f + 0.04f * x + 0.02f * z;
}

TEST(TerrainAreaFlattenSurface, ADiamondRimThroughSamplesGivesItsPlane)
{
    // The four corners sit on samples, and each side crosses a sample at every step along it, so
    // the stencil meets the rim exactly at a sample again and again.
    const std::vector<Mathematics::Vector3> rim = {{0.0f, TiltedPlane(0.0f, -kRimHalf), -kRimHalf},
                                                   {kRimHalf, TiltedPlane(kRimHalf, 0.0f), 0.0f},
                                                   {0.0f, TiltedPlane(0.0f, kRimHalf), kRimHalf},
                                                   {-kRimHalf, TiltedPlane(-kRimHalf, 0.0f), 0.0f}};
    AreaFlattenScene scene;
    scene.AddPlazaOnRim(rim);
    scene.Bake();

    float32 worst = 0.0f;
    ForEachInteriorSample(0.0f, [&scene, &worst](uint32 column, uint32 row) {
        const float32 x = SampleWorldX(column);
        const float32 z = SampleWorldX(row);
        if (std::fabs(x) + std::fabs(z) > kRimHalf - 1.0f)
            return;
        worst = std::max(worst, std::fabs(scene.HeightAt(column, row) - TiltedPlane(x, z)));
    });
    // The floor is the heightfield's quantization plus the solve's tolerance, about 3 mm. The bound
    // pins that floor; it does not separate a rim crossing that snaps from one that rounds.
    EXPECT_LT(worst, 0.004f) << "the plaza does not follow the plane of a diamond rim";
}

// A plaza sloped 5 percent along +X, and two roads with their own grades entering at its west and
// east sides at heights that differ from the rim's.
constexpr float32 kSlopedCorners[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
constexpr float32 kWestRoadZ = 0.0f;
constexpr float32 kEastRoadZ = 5.0f;

float32 WestRoadY(float32 x, float32 stepAtRim)
{
    return -1.0f + stepAtRim + 0.08f * (x + kRimHalf);
}

float32 EastRoadY(float32 x, float32 stepAtRim)
{
    return 1.0f + stepAtRim - 0.06f * (x - kRimHalf);
}

void AddRoads(AreaFlattenScene& scene, float32 westStep, float32 eastStep)
{
    scene.AddRoad({{-60.0f, WestRoadY(-60.0f, westStep), kWestRoadZ},
                   {-kRimHalf, WestRoadY(-kRimHalf, westStep), kWestRoadZ}});
    scene.AddRoad({{kRimHalf, EastRoadY(kRimHalf, eastStep), kEastRoadZ},
                   {60.0f, EastRoadY(60.0f, eastStep), kEastRoadZ}});
}

// The largest step at an opening, across the road's width: between the ground on the rim and the
// road's own grade there, and between the rim and the first sample inside it. `inward` is +1 when
// the plaza lies toward +X from the rim, -1 when toward -X. The fade past the rim is not measured:
// there the road defers to the plaza's claim and the plaza's own weight fades.
float32 WorstOpeningStep(const AreaFlattenScene& scene, float32 rimX, int32 inward, float32 roadZ,
                         const std::function<float32(float32)>& roadY)
{
    float32 worst = 0.0f;
    const uint32 rimColumn = ColumnAt(rimX);
    const uint32 insideColumn = static_cast<uint32>(static_cast<int32>(rimColumn) + inward);
    for (uint32 row = ColumnAt(roadZ - kRoadHalfWidth + kSpacing); row <= ColumnAt(roadZ + kRoadHalfWidth - kSpacing);
         ++row)
    {
        const float32 onRim = scene.HeightAt(rimColumn, row);
        worst = std::max(worst, std::fabs(onRim - roadY(rimX)));
        worst = std::max(worst, std::fabs(scene.HeightAt(insideColumn, row) - onRim));
    }
    return worst;
}

TEST(TerrainAreaFlattenSurface, SlopedRoadsMeetTheSlopedPlazaAcrossTheArmsFullWidth)
{
    AreaFlattenScene scene;
    scene.AddPlaza(kSlopedCorners);
    constexpr float32 kWestStep = 0.2f;
    constexpr float32 kEastStep = -0.15f;
    AddRoads(scene, kWestStep, kEastStep);
    scene.Bake();

    // A 25 percent grade over one sample is the tolerance: the plaza bends to the road there, the
    // road does not step.
    const float32 tolerance = 0.25f * kSpacing;
    EXPECT_LT(WorstOpeningStep(scene, -kRimHalf, 1, kWestRoadZ, [](float32 x) { return WestRoadY(x, kWestStep); }),
              tolerance)
        << "the west road and the plaza do not meet across the road's width";
    EXPECT_LT(WorstOpeningStep(scene, kRimHalf, -1, kEastRoadZ, [](float32 x) { return EastRoadY(x, kEastStep); }),
              tolerance)
        << "the east road and the plaza do not meet across the road's width";
}

// A road that arrives 1 m above the rim, more than the network's split-level tolerance (0.25 m): the
// plaza grades up to the road at the opening and keeps the rim's heights away from it.
TEST(TerrainAreaFlattenSurface, ARoadAboveTheRimGradesThePlazaUpAtItsOpening)
{
    AreaFlattenScene scene;
    scene.AddPlaza(kSlopedCorners);
    constexpr float32 kStep = 1.0f;
    AddRoads(scene, kStep, 0.0f);
    scene.Bake();

    EXPECT_LT(WorstOpeningStep(scene, -kRimHalf, 1, kWestRoadZ, [](float32 x) { return WestRoadY(x, kStep); }),
              0.25f * kSpacing)
        << "the plaza does not grade up to the raised road";

    // The west side far from the opening keeps the rim's height.
    const uint32 rimColumn = ColumnAt(-kRimHalf);
    for (const float32 z : {-15.0f, 15.0f})
        EXPECT_NEAR(scene.HeightAt(rimColumn, ColumnAt(z)), -1.0f, 0.01f) << "the rim moved at z = " << z;
}

TEST(TerrainAreaFlattenSurface, TheWalkingSlopeInsideStaysUnderTheRoads)
{
    AreaFlattenScene scene;
    scene.AddPlaza(kSlopedCorners);
    AddRoads(scene, 0.0f, 0.0f);
    scene.Bake();

    // The steepest grade the plaza meets: the rim's 5 percent and the roads' 8 and 6 percent.
    constexpr float32 kSteepestInput = 0.08f;
    float32 steepest = 0.0f;
    ForEachInteriorSample(1.0f, [&scene, &steepest](uint32 column, uint32 row) {
        const float32 dx = (scene.HeightAt(column + 1, row) - scene.HeightAt(column - 1, row)) / (2.0f * kSpacing);
        const float32 dz = (scene.HeightAt(column, row + 1) - scene.HeightAt(column, row - 1)) / (2.0f * kSpacing);
        steepest = std::max(steepest, std::sqrt(dx * dx + dz * dz));
    });
    EXPECT_LE(steepest, kSteepestInput) << "the plaza is steeper inside than the grades it meets";
}

TEST(TerrainAreaFlattenSurface, ARebuildFromTheSameInputsChangesNoTexel)
{
    AreaFlattenScene scene;
    scene.AddPlaza(kTwistedCorners);
    AddRoads(scene, 0.2f, -0.15f);
    scene.Bake();
    const std::vector<float32> first = scene.CopyHeights();

    // A previous bake that is wrong everywhere under the plaza, then a full rebuild by a second
    // system: the surface reads the authored rim and roads, never the ground it wrote.
    scene.OverwritePlazaSamples(0.75f);
    TerrainModifierSystem rebuild;
    rebuild.Update(scene.GetWorld(), 1.0f / 60.0f);
    const std::vector<float32> second = scene.CopyHeights();

    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(std::memcmp(first.data(), second.data(), first.size() * sizeof(float32)), 0)
        << "a rebuild from the same inputs changed the ground";
}

// The baked ground over the plaza and its fade, row by row.
std::vector<float32> CopyPlazaHeights(const AreaFlattenScene& scene)
{
    std::vector<float32> heights;
    for (uint32 row = ColumnAt(-kRimHalf - kFade); row <= ColumnAt(kRimHalf + kFade); ++row)
        for (uint32 column = ColumnAt(-kRimHalf - kFade); column <= ColumnAt(kRimHalf + kFade); ++column)
            heights.push_back(scene.HeightAt(column, row));
    return heights;
}

// A road from the west opening that swings south, far past the plaza's bounds and its own reach,
// and comes back in at the east opening. Each part near the plaza is its own run, so the plaza is
// graded as by two roads; one run would bridge the gap across the plaza.
TEST(TerrainAreaFlattenSurface, ARoadThatLeavesAndComesBackGradesThePlazaAsTwoRoads)
{
    constexpr float32 kWestY = -0.8f;
    constexpr float32 kEastY = 0.85f;
    const Mathematics::Vector3 westOpening{-kRimHalf, kWestY, kWestRoadZ};
    const Mathematics::Vector3 westTurn{-35.0f, kWestY, kWestRoadZ};
    const Mathematics::Vector3 eastTurn{35.0f, kEastY, kEastRoadZ};
    const Mathematics::Vector3 eastOpening{kRimHalf, kEastY, kEastRoadZ};

    std::vector<float32> oneRoad;
    {
        AreaFlattenScene scene;
        scene.AddPlaza(kSlopedCorners);
        scene.AddRoad({westOpening, westTurn, {-35.0f, kWestY, -40.0f}, {35.0f, kEastY, -40.0f}, eastTurn, eastOpening});
        scene.Bake();
        oneRoad = CopyPlazaHeights(scene);
    }
    std::vector<float32> twoRoads;
    {
        AreaFlattenScene scene;
        scene.AddPlaza(kSlopedCorners);
        scene.AddRoad({westOpening, westTurn});
        scene.AddRoad({eastTurn, eastOpening});
        scene.Bake();
        twoRoads = CopyPlazaHeights(scene);
    }

    ASSERT_EQ(oneRoad.size(), twoRoads.size());
    EXPECT_EQ(std::memcmp(oneRoad.data(), twoRoads.data(), oneRoad.size() * sizeof(float32)), 0)
        << "the road that leaves and comes back grades the plaza unlike its two parts";
}

TEST(TerrainAreaFlattenSurface, OnlyAnEditToTheRimOrAnOpeningRebuildsTheSurface)
{
    AreaFlattenScene scene;
    scene.AddPlaza(kSlopedCorners);
    AddRoads(scene, 0.2f, -0.15f);
    const ECS::EntityHandle westRoad = scene.AddRoad({{-60.0f, -0.5f, -12.0f}, {-kRimHalf, -0.5f, -12.0f}});
    scene.Bake();
    ASSERT_EQ(scene.GetSystem().GetAreaSurfaceBuildCountForTests(), 1u);

    // A modifier far from the plaza re-gathers the stack and leaves the plaza's inputs as they were.
    scene.AddDistantVolume();
    scene.Bake();
    EXPECT_EQ(scene.GetSystem().GetAreaSurfaceBuildCountForTests(), 1u) << "an unrelated edit rebuilt the surface";

    // A new grade on a road at an opening is a new boundary.
    scene.GetWorld().GetComponentForWrite<Components::TerrainFlattenEffect>(westRoad)->TargetHeight = 0.3f;
    scene.Bake();
    EXPECT_EQ(scene.GetSystem().GetAreaSurfaceBuildCountForTests(), 2u) << "a road edit at an opening kept the old surface";
}

TEST(TerrainAreaFlattenSurface, TheColliderUnderThePlazaEqualsTheSurface)
{
    AreaFlattenScene scene;
    scene.AddPlaza(kTwistedCorners);
    scene.Bake();

    // The collider reads its samples through the provider the terrain service registers.
    const PhysicsECS::HeightFieldDataProvider provider = PhysicsECS::GetHeightFieldDataProvider();
    ASSERT_NE(provider, nullptr);
    const PhysicsECS::HeightFieldData collider = provider(scene.GetHandle().Index, scene.GetHandle().Generation, 0);
    ASSERT_NE(collider.samples, nullptr);
    ASSERT_EQ(collider.sampleCount, kSampleDim);

    float32 worst = 0.0f;
    ForEachInteriorSample(1.0f, [&collider, &worst](uint32 column, uint32 row) {
        const float32 colliderY = collider.samples[static_cast<std::size_t>(row) * kSampleDim + column] * kHeightScale;
        const float32 expected = BilinearRim(kTwistedCorners, SampleWorldX(column), SampleWorldX(row));
        worst = std::max(worst, std::fabs(colliderY - expected));
    });
    EXPECT_LT(worst, 0.01f) << "the collider under the plaza does not follow the surface";
}

} // namespace
