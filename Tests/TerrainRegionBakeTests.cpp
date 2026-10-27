#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "CBTTerrain/CBTPlanetShading.h"
#include "Components/SceneEntityTag.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "Core/CpuProfiler.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/HalfFloat.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/PageResidency.h"
#include "PageStreaming/PageStore/PageStoreAccess.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "PageStreaming/PageStore/PageStorePatcher.h"
#include "PageStreaming/PageStore/PageStoreReader.h"
#include "PageStreaming/PageStore/PageStoreWriter.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainSurfaceBakeInputs.h"
#include "TerrainECS/TerrainSurfacePageBaker.h"
#include "TerrainECS/TerrainSurfacePageDriver.h"
#include "TerrainECS/TerrainSurfaceStore.h"
#include "TerrainECS/HeightPageStoreLoader.h"
#include "TerrainECS/TerrainColorMapBaker.h"
#include "Types/Fnv1a.h"
#include "SplineECS/SplineService.h"
#include "TerrainECS/TerrainAtlas.h" // BuildCoarseHeightField (the atlas coarse field)
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainReprovisionDebounce.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainSplatComposite.h" // CompositeSplatTexel (zero-base rationale)
#include "TerrainECS/TileStreamingManager.h"
#include "TerrainRowBands.h" // TerrainECS/Source: RunRowBands, kBakeBandRows

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

struct ScopedTerrainService
{
    ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
    }
    ~ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }
};

constexpr float32 kWorldSize = 256.0f;
constexpr float32 kHeightScale = 64.0f;
constexpr uint32 kHeightmapDim = 129;

Terrain::TerrainConfig MakeTestConfig(uint32 dim = kHeightmapDim)
{
    Terrain::TerrainConfig cfg{};
    cfg.HeightmapWidth = dim;
    cfg.HeightmapHeight = dim;
    cfg.WorldSizeX = kWorldSize;
    cfg.WorldSizeZ = kWorldSize;
    cfg.HeightScale = kHeightScale;
    cfg.LODLevels = 4;
    return cfg;
}

// Mimic TerrainExtractionSystem's terrain-creation path: base fill, quadtree,
// procedural splatmap (which caches the splat bake height range).
TerrainHandle CreateBakedTerrainWithBase(TerrainService& svc,
                                         Components::TerrainBaseSource baseSource,
                                         const GUID& heightmapGuid,
                                         uint32 dim = kHeightmapDim)
{
    const TerrainHandle handle = svc.CreateTerrain(MakeTestConfig(dim));
    auto* data = svc.GetTerrainData(handle);
    std::shared_ptr<const Terrain::HeightfieldData> heightmap;
    if (baseSource == Components::TerrainBaseSource::HeightmapAsset && !heightmapGuid.IsNull())
        heightmap = svc.ResolveHeightmapAsset(heightmapGuid);
    FillHeightfieldBaseRegion(data->Heightfield, baseSource, heightmap.get(),
                              0, 0,
                              static_cast<int32>(dim) - 1,
                              static_cast<int32>(dim) - 1);
    data->MarkFullDirty();
    svc.RebuildQuadtree(handle);
    data->ResetSplatmapAndCommitRange();
    return handle;
}

TerrainHandle CreateBakedTerrain(TerrainService& svc)
{
    return CreateBakedTerrainWithBase(svc, Components::TerrainBaseSource::ProceduralNoise, GUID{});
}

// Mirror the engine bootstrap: the primary world subscribes lifecycle events
// for the five modifier types (TerrainModifierSystem's change gating consumes
// Added/Removed — a destroyed modifier is invisible to Changed<> scans).
// Tests stand in for the engine tick's once-per-frame swap by calling
// world.SwapLifecycleEvents() after structural changes.
void RegisterModifierLifecycleEvents(ECS::World& world)
{
    // Folds the canonical root/effect lists rather than restating them, so a
    // component added to the gather cannot go silently unwatched here.
    TerrainECS::EnableTerrainModifierLifecycleEvents(world);
}

ECS::EntityHandle CreateTerrainEntity(
    ECS::World& world, TerrainHandle handle,
    Components::TerrainBaseSource baseSource = Components::TerrainBaseSource::ProceduralNoise,
    const GUID& heightmapGuid = GUID{})
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = kWorldSize;
    terrain.SizeZ = kWorldSize;
    terrain.HeightScale = kHeightScale;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    terrain.BaseSource = baseSource;
    if (!heightmapGuid.IsNull())
        terrain.TerrainAssetGuid.Set(heightmapGuid);
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    return e;
}

// A circle volume carrying one flatten to an absolute world height.
ECS::EntityHandle CreateFlattenModifier(ECS::World& world, float32 x, float32 z, float32 targetHeight,
                                        float32 radius = 25.0f)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Circle;
    vol.Radius = radius;
    vol.Falloff = 8.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);

    Components::TerrainFlattenEffect fx{};
    fx.UseVolumeHeight = false;
    fx.TargetHeight = targetHeight;
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);

    Components::WorldTransform xf{};
    xf.matrix[12] = x;
    xf.matrix[14] = z;
    world.AddComponentImmediate<Components::WorldTransform>(e, xf);
    return e;
}

// A Shape::Global volume carrying one Surface Rules effect — the shape the
// Inspector's rules authoring takes, and the one whose dirty rect is every
// resident tile, which is what makes dragging one of its bands the expensive
// edit the preview cadence exists for.
ECS::EntityHandle CreateGlobalRulesModifier(ECS::World& world)
{
    auto e = world.CreateEntity();

    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Global;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);

    Components::TerrainSurfaceRulesEffect rules{};
    rules.RuleCount = 1;
    rules.Rules[0].MaterialSlot = 1;
    rules.Rules[0].Strength = 1.0f;
    rules.Rules[0].ConditionCount = 1;
    Components::TerrainRuleCondition& condition = rules.Rules[0].Conditions[0];
    condition.Kind = Components::TerrainRuleConditionKind::HeightNormalized;
    condition.Min = 0.2f;
    condition.Max = 0.8f;
    condition.Feather = 0.05f;
    world.AddComponentImmediate<Components::TerrainSurfaceRulesEffect>(e, rules);

    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    return e;
}

// Texels carrying any material weight. The unbaked base is all zero, so every splat
// comparison needs this as a POSITIVE CONTROL: two all-zero splatmaps are byte-equal
// for the trivial reason, and a seam test that lost its rules would pass while
// measuring nothing.
std::size_t WeightedTexelCount(const std::vector<uint8>& splatmap)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i + 3 < splatmap.size(); i += 4)
        if (splatmap[i] | splatmap[i + 1] | splatmap[i + 2] | splatmap[i + 3])
            ++count;
    return count;
}

// A spherical (planet) terrain entity. The heightfield handle is unused on the sphere
// path (relief is procedural), but a live handle keeps it consistent with the render
// resolver's liveness.
ECS::EntityHandle CreatePlanetEntity(ECS::World& world, TerrainHandle handle)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.Domain = Components::TerrainDomain::Spherical;
    terrain.PlanetRadius = 2000.0f;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    return e;
}

// A noise effect — one of the sphere-supported effect kinds (with stamp, flatten
// and SculptZone).
ECS::EntityHandle CreateNoiseModifier(ECS::World& world, float32 x, float32 z)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Circle;
    vol.Radius = 25.0f;
    vol.Falloff = 8.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);

    Components::TerrainNoiseEffect fx{};
    fx.Amplitude = 5.0f;
    fx.Frequency = 8.0f;
    fx.Octaves = 3u;
    world.AddComponentImmediate<Components::TerrainNoiseEffect>(e, fx);

    Components::WorldTransform xf{};
    xf.matrix[12] = x;
    xf.matrix[14] = z;
    world.AddComponentImmediate<Components::WorldTransform>(e, xf);
    return e;
}

ECS::EntityHandle CreateStampModifier(ECS::World& world, float32 x, float32 z,
                                      const GUID& maskGuid)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Circle;
    vol.Radius = 25.0f;
    vol.Falloff = 8.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);

    Components::TerrainStampEffect fx{};
    fx.HeightScale = 20.0f;
    fx.Rotation = 30.0f;
    fx.Blend = Components::TerrainModifierBlend::Add;
    if (!maskGuid.IsNull())
        fx.StampAssetGuid.Set(maskGuid);
    world.AddComponentImmediate<Components::TerrainStampEffect>(e, fx);

    Components::WorldTransform xf{};
    xf.matrix[12] = x;
    xf.matrix[14] = z;
    world.AddComponentImmediate<Components::WorldTransform>(e, xf);
    return e;
}

// Asymmetric procedural mask so rotation/orientation mistakes change the bake.
std::vector<float32> MakeGradientMask(uint32 width, uint32 height)
{
    std::vector<float32> texels(static_cast<size_t>(width) * height);
    for (uint32 z = 0; z < height; ++z)
        for (uint32 x = 0; x < width; ++x)
        {
            const float32 u = static_cast<float32>(x) / static_cast<float32>(width - 1);
            const float32 v = static_cast<float32>(z) / static_cast<float32>(height - 1);
            texels[static_cast<size_t>(z) * width + x] = 0.15f + 0.7f * u * (0.3f + 0.7f * v);
        }
    return texels;
}

// Extraction's consumption is a cursor advance now (nobody clears shared
// state) — returns the cursor value the consumer would hold afterwards.
uint64 SimulateExtractionConsumed(TerrainData& data)
{
    data.SplatmapDirty = false;
    return data.HeightfieldVersion;
}

// ---- Sphere (planet) modifier helpers -------------------------------------------------------
// The planet is centred at the world origin (C7), so a modifier's world position IS its direction
// (once normalized) and its distance from centre. These helpers place a modifier at a full 3D
// world point (unlike the planar helpers, which only set XZ) so the sphere bake sees a real
// direction + radius.
struct Dir3 { float32 x, y, z; };

Dir3 Normalize3(Dir3 d)
{
    const float32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const float32 inv = len > 0.0f ? 1.0f / len : 0.0f;
    return {d.x * inv, d.y * inv, d.z * inv};
}

ECS::EntityHandle CreateSphereFlattenModifier(ECS::World& world, Dir3 worldPos, float32 radius,
                                              float32 falloff, bool useEntityHeight,
                                              float32 targetHeight)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Circle;
    vol.Radius = radius;
    vol.Falloff = falloff;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);

    Components::TerrainFlattenEffect fx{};
    fx.UseVolumeHeight = useEntityHeight;
    fx.TargetHeight = targetHeight;
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);

    Components::WorldTransform xf{};
    xf.matrix[12] = worldPos.x;
    xf.matrix[13] = worldPos.y;
    xf.matrix[14] = worldPos.z;
    world.AddComponentImmediate<Components::WorldTransform>(e, xf);
    return e;
}

ECS::EntityHandle CreateSphereStampModifier(ECS::World& world, Dir3 worldPos, float32 radius,
                                            float32 falloff, float32 heightScale,
                                            Components::TerrainModifierBlend blend)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Circle;
    vol.Radius = radius;
    vol.Falloff = falloff;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);

    Components::TerrainStampEffect fx{};
    fx.HeightScale = heightScale;
    fx.Blend = blend;
    // No StampAssetGuid → the documented flat-white mask (a uniform disc), so the test needs no
    // texture asset / AssetManager.
    world.AddComponentImmediate<Components::TerrainStampEffect>(e, fx);

    Components::WorldTransform xf{};
    xf.matrix[12] = worldPos.x;
    xf.matrix[13] = worldPos.y;
    xf.matrix[14] = worldPos.z;
    world.AddComponentImmediate<Components::WorldTransform>(e, xf);
    return e;
}
} // namespace

// The load-bearing parity test: after a modifier edit, the region re-bake must
// produce heightfield samples and splatmap bytes IDENTICAL to a fresh full
// bake of the final modifier state. Bit-equality against the full bake proves
// both halves at once: in-region samples equal the full bake, and out-region
// samples were left exactly as a full bake would have produced them.
TEST(TerrainRegionBake, RegionBakeMatchesFullBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    // Terrain A: initial modifier bake, then a modifier move → region re-bake
    // covering where the modifier was AND where it is.
    const TerrainHandle handleA = CreateBakedTerrain(svc);
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTerrainEntity(worldA, handleA);
    auto modA = CreateFlattenModifier(worldA, 30.0f, 40.0f, 20.0f);

    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // initial bake (full: no baseline yet)
    DirtyRegionLog::Region hfRegion;
    ASSERT_TRUE(dataA->HeightfieldDirtyLog.CollectSince(0, hfRegion));
    const uint64 hfCursor = SimulateExtractionConsumed(*dataA);

    auto* xf = worldA.GetComponentForWrite<Components::WorldTransform>(modA);
    ASSERT_NE(xf, nullptr);
    xf->matrix[12] = -20.0f;
    xf->matrix[14] = -35.0f;

    systemA.Update(worldA, 1.0f / 60.0f);

    // The second bake must be region-scoped: the dirty rect recorded after
    // the consumer's cursor is a proper sub-rect (both modifier positions
    // are well inside the terrain).
    ASSERT_TRUE(dataA->HeightfieldDirtyLog.CollectSince(hfCursor, hfRegion));
    EXPECT_GT(hfRegion.MinX, 0);
    EXPECT_GT(hfRegion.MinZ, 0);
    EXPECT_LT(hfRegion.MaxX, static_cast<int32>(kHeightmapDim));
    EXPECT_LT(hfRegion.MaxZ, static_cast<int32>(kHeightmapDim));

    // Terrain B: fresh full bake of the FINAL modifier state.
    const TerrainHandle handleB = CreateBakedTerrain(svc);
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);

    ECS::World worldB;
    CreateTerrainEntity(worldB, handleB);
    CreateFlattenModifier(worldB, -20.0f, -35.0f, 20.0f);

    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    // Heightfields bit-identical everywhere.
    ASSERT_EQ(dataA->Heightfield.GetWidth(), dataB->Heightfield.GetWidth());
    ASSERT_EQ(dataA->Heightfield.GetHeight(), dataB->Heightfield.GetHeight());
    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)));

    // Splatmaps bit-identical everywhere.
    ASSERT_EQ(dataA->Splatmap.size(), dataB->Splatmap.size());
    ASSERT_FALSE(dataA->Splatmap.empty());
    EXPECT_EQ(0, std::memcmp(dataA->Splatmap.data(), dataB->Splatmap.data(),
                             dataA->Splatmap.size()));
}

// Interactive geometry-drag coalescing: while the editor signals a transform
// drag (SetInteractiveModifierEdit), the expensive per-frame full-footprint
// re-bake is deferred — the heightfield must NOT change across the deferred
// frames — and the settle bake on release must land the final position
// byte-identically to a fresh full bake. This is the core symptom-2 fix: a
// gizmo drag of a modifier re-baked old∪new every frame (single-digit fps).
TEST(TerrainRegionBake, InteractiveGeometryDragDefersThenSettleMatchesFullBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handleA = CreateBakedTerrain(svc);
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTerrainEntity(worldA, handleA);
    auto modA = CreateFlattenModifier(worldA, 30.0f, 40.0f, 20.0f);

    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // initial bake at the start position

    // Snapshot the baked heightfield; the deferred drag frames must not touch it.
    std::vector<float32> heightsAtDragStart(
        dataA->Heightfield.GetRawSamples(),
        dataA->Heightfield.GetRawSamples() + dataA->Heightfield.GetSampleCount());
    const uint64 versionAtDragStart = dataA->HeightfieldVersion;

    // Drag: several intermediate positions while the interactive flag is set.
    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::TransformGizmo, true);
    const std::array<std::pair<float32, float32>, 3> dragPath = {{
        {10.0f, 10.0f}, {-10.0f, 5.0f}, {-20.0f, -35.0f}}};
    for (const auto& [px, pz] : dragPath)
    {
        auto* xf = worldA.GetComponentForWrite<Components::WorldTransform>(modA);
        ASSERT_NE(xf, nullptr);
        xf->matrix[12] = px;
        xf->matrix[14] = pz;
        systemA.Update(worldA, 1.0f / 60.0f);

        // Deferred: the terrain still reflects the pre-drag bake — no re-bake ran.
        EXPECT_EQ(dataA->HeightfieldVersion, versionAtDragStart)
            << "an interactive drag frame re-baked instead of deferring";
        EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(), heightsAtDragStart.data(),
                                 heightsAtDragStart.size() * sizeof(float32)))
            << "an interactive drag frame mutated the heightfield instead of deferring";
    }

    // Release: the settle bake runs and lands the final position.
    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::TransformGizmo, false);
    systemA.Update(worldA, 1.0f / 60.0f);
    EXPECT_GT(dataA->HeightfieldVersion, versionAtDragStart) << "settle bake did not run on release";

    // Reference: a fresh full bake of the final position.
    const TerrainHandle handleB = CreateBakedTerrain(svc);
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);

    ECS::World worldB;
    CreateTerrainEntity(worldB, handleB);
    CreateFlattenModifier(worldB, -20.0f, -35.0f, 20.0f);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    // The settled terrain is byte-identical to per-frame baking of the final state.
    ASSERT_EQ(dataA->Heightfield.GetSampleCount(), dataB->Heightfield.GetSampleCount());
    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)));
    ASSERT_EQ(dataA->Splatmap.size(), dataB->Splatmap.size());
    ASSERT_FALSE(dataA->Splatmap.empty());
    EXPECT_EQ(0, std::memcmp(dataA->Splatmap.data(), dataB->Splatmap.data(),
                             dataA->Splatmap.size()));
}

// Edge case: an interactive drag that returns the modifier to its pre-drag
// position leaves nothing to bake on release — the settle path must recognise
// the nil net change (hash unchanged), skip the bake, and clear its pending
// state so the terrain stays byte-identical and the gate goes quiet.
TEST(TerrainRegionBake, InteractiveDragReturnedToStartSettleIsNoOp)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handle = CreateBakedTerrain(svc);
    auto* data = svc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTerrainEntity(world, handle);
    auto mod = CreateFlattenModifier(world, 30.0f, 40.0f, 20.0f);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // initial bake at (30,40)
    const uint64 versionAfterBake = data->HeightfieldVersion;
    std::vector<float32> heights(
        data->Heightfield.GetRawSamples(),
        data->Heightfield.GetRawSamples() + data->Heightfield.GetSampleCount());

    // Drag away and back to the start.
    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::TransformGizmo, true);
    for (const auto& [px, pz] : std::array<std::pair<float32, float32>, 3>{{
             {-20.0f, -35.0f}, {5.0f, 5.0f}, {30.0f, 40.0f}}})
    {
        auto* xf = world.GetComponentForWrite<Components::WorldTransform>(mod);
        ASSERT_NE(xf, nullptr);
        xf->matrix[12] = px;
        xf->matrix[14] = pz;
        system.Update(world, 1.0f / 60.0f);
    }
    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::TransformGizmo, false);
    system.Update(world, 1.0f / 60.0f); // settle: net change is nil → no bake

    EXPECT_EQ(data->HeightfieldVersion, versionAfterBake) << "a nil-net drag re-baked on release";
    EXPECT_EQ(0, std::memcmp(data->Heightfield.GetRawSamples(), heights.data(),
                             heights.size() * sizeof(float32)));

    // A following idle tick must stay quiet (pending state cleared, gate quiet).
    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(data->HeightfieldVersion, versionAfterBake);
}

// Live-preview throttle: while the editor signals a transform drag, the modifier
// system re-bakes the affected region at a bounded wall-clock cadence so the
// modifier's effect updates live as it moves (the fix for "delayed until movement
// finished — no preview"). Drive a deterministic injected clock past the pinned
// interval mid-drag and assert (b) at least one preview re-baked BEFORE release,
// and that the settled terrain after N throttled previews is byte-identical to a
// fresh full bake of the final position (previews must not perturb the release).
TEST(TerrainRegionBake, InteractiveDragThrottledPreviewLandsBeforeReleaseAndSettleMatches)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handleA = CreateBakedTerrain(svc);
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTerrainEntity(worldA, handleA);
    auto modA = CreateFlattenModifier(worldA, 30.0f, 40.0f, 20.0f);

    TerrainModifierSystem systemA;
    // Deterministic throttle: injected clock + a pinned 100 ms interval so the
    // adaptive backoff can't move it (no sleeping, no wall-clock flakiness).
    systemA.SetPreviewClockForTests(std::chrono::steady_clock::time_point{});
    systemA.SetPreviewIntervalForTests(100.0f);
    systemA.Update(worldA, 1.0f / 60.0f); // initial bake at the start position

    const uint64 versionAtDragStart = dataA->HeightfieldVersion;

    // Drag across several positions, advancing the clock 60 ms/frame so a preview
    // lands roughly every other frame.
    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::TransformGizmo, true);
    const std::array<std::pair<float32, float32>, 6> dragPath = {{
        {20.0f, 30.0f}, {10.0f, 20.0f}, {0.0f, 10.0f},
        {-10.0f, 0.0f}, {-20.0f, -20.0f}, {-30.0f, -40.0f}}};
    bool sawMidDragBake = false;
    for (const auto& [px, pz] : dragPath)
    {
        auto* xf = worldA.GetComponentForWrite<Components::WorldTransform>(modA);
        ASSERT_NE(xf, nullptr);
        xf->matrix[12] = px;
        xf->matrix[14] = pz;
        systemA.AdvancePreviewClockForTests(60.0f);
        systemA.Update(worldA, 1.0f / 60.0f);
        if (dataA->HeightfieldVersion != versionAtDragStart)
            sawMidDragBake = true;
    }

    // Oracle (b): a live preview re-baked the terrain BEFORE release.
    EXPECT_TRUE(sawMidDragBake) << "no live preview landed during the drag";
    EXPECT_GE(systemA.GetDragPreviewBakeCountForTests(), 1u);
    EXPECT_GT(dataA->HeightfieldVersion, versionAtDragStart);

    // Release: the settle bake lands the final position.
    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::TransformGizmo, false);
    systemA.Update(worldA, 1.0f / 60.0f);

    // Reference: a fresh full bake of the final position.
    const TerrainHandle handleB = CreateBakedTerrain(svc);
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);
    ECS::World worldB;
    CreateTerrainEntity(worldB, handleB);
    CreateFlattenModifier(worldB, -30.0f, -40.0f, 20.0f);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    // The settled terrain (after N throttled previews) is byte-identical to
    // per-frame baking of the final state — previews preserve the settle oracle.
    ASSERT_EQ(dataA->Heightfield.GetSampleCount(), dataB->Heightfield.GetSampleCount());
    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)));
    ASSERT_EQ(dataA->Splatmap.size(), dataB->Splatmap.size());
    ASSERT_FALSE(dataA->Splatmap.empty());
    EXPECT_EQ(0, std::memcmp(dataA->Splatmap.data(), dataB->Splatmap.data(),
                             dataA->Splatmap.size()));
}

// Oracle (c): the throttle bounds preview bakes to elapsed/interval + 1. Drive a
// continuous ~60 fps drag (16 ms/frame) for a known span against a pinned 100 ms
// interval; the number of preview re-bakes must not exceed the throttle budget
// (i.e. the drag never re-bakes every frame — the #530 collapse guard), yet at
// least one preview must land.
TEST(TerrainRegionBake, InteractiveDragThrottleBoundsPreviewCount)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handle = CreateBakedTerrain(svc);
    auto* data = svc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTerrainEntity(world, handle);
    auto mod = CreateFlattenModifier(world, 0.0f, 0.0f, 20.0f);

    TerrainModifierSystem system;
    constexpr float32 kIntervalMs = 100.0f;
    system.SetPreviewClockForTests(std::chrono::steady_clock::time_point{});
    system.SetPreviewIntervalForTests(kIntervalMs);
    system.Update(world, 1.0f / 60.0f); // initial bake

    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::TransformGizmo, true);
    constexpr int32 kFrames = 30;
    constexpr float32 kFrameMs = 16.0f; // ~60 fps drag frame budget
    float32 x = 0.0f;
    for (int32 i = 0; i < kFrames; ++i)
    {
        x += 3.0f;
        auto* xf = world.GetComponentForWrite<Components::WorldTransform>(mod);
        ASSERT_NE(xf, nullptr);
        xf->matrix[12] = x;
        system.AdvancePreviewClockForTests(kFrameMs);
        system.Update(world, 1.0f / 60.0f);
    }

    const float32 totalElapsedMs = static_cast<float32>(kFrames) * kFrameMs;
    const uint32 maxBakes = static_cast<uint32>(totalElapsedMs / kIntervalMs) + 1u;
    EXPECT_LE(system.GetDragPreviewBakeCountForTests(), maxBakes)
        << "throttle exceeded elapsed/interval + 1 (would re-collapse the drag fps)";
    EXPECT_GE(system.GetDragPreviewBakeCountForTests(), 1u)
        << "throttle starved the drag of any live preview";
}

// The same oracle for a SURFACE RULES condition band, which is what the
// Inspector's two-handle band widget drags.
//
// It is a separate test from the gizmo one above because nothing about the
// coalescing branch was obviously true for it: the branch is gated on
// `anyGeometryChange && !anyPayloadChange && !fullBake`, and a rules edit moves
// no transform, writes no payload, and sits on a GLOBAL volume whose bounds are
// infinite. It reaches the branch by a different route at every one of those
// three — a re-parameterized modifier (its geometry hash folds every authored
// rule field), no payload version bump, and an infinite dirty rect that is still
// a REGION rect rather than a full bake. If any of the three stopped holding,
// dragging a band would re-bake the world once per mouse-move and this is the
// only thing that would say so.
//
// Red arm: with the throttle absent the count is one per frame — 30 against a
// budget of 5 — so a regression fails on the bound, not on a tolerance.
TEST(TerrainRegionBake, InteractiveRulesBandDragThrottleBoundsPreviewCount)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handle = CreateBakedTerrain(svc);
    auto* data = svc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTerrainEntity(world, handle);
    auto mod = CreateGlobalRulesModifier(world);

    TerrainModifierSystem system;
    constexpr float32 kIntervalMs = 100.0f;
    system.SetPreviewClockForTests(std::chrono::steady_clock::time_point{});
    system.SetPreviewIntervalForTests(kIntervalMs);
    system.Update(world, 1.0f / 60.0f); // initial bake establishes the baseline snapshot

    // The Inspector's source, not the gizmo's — this is the arm the band widget
    // holds for the duration of its drag.
    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::InspectorDrag, true);

    constexpr int32 kFrames = 30;
    constexpr float32 kFrameMs = 16.0f; // ~60 fps drag frame budget
    float32 low = 0.2f;
    for (int32 i = 0; i < kFrames; ++i)
    {
        // One mouse-move's worth of handle travel, written the way the widget's
        // preview writes it: straight onto the component column.
        low += 0.005f;
        auto* rules = world.GetComponentForWrite<Components::TerrainSurfaceRulesEffect>(mod);
        ASSERT_NE(rules, nullptr);
        rules->Rules[0].Conditions[0].Min = low;
        system.AdvancePreviewClockForTests(kFrameMs);
        system.Update(world, 1.0f / 60.0f);
    }

    const float32 totalElapsedMs = static_cast<float32>(kFrames) * kFrameMs;
    const uint32 maxBakes = static_cast<uint32>(totalElapsedMs / kIntervalMs) + 1u;
    EXPECT_LE(system.GetDragPreviewBakeCountForTests(), maxBakes)
        << "a rules band drag re-baked more often than the preview cadence allows - it is "
           "missing the coalescing branch, so every mouse-move re-bakes every resident tile";
    EXPECT_GE(system.GetDragPreviewBakeCountForTests(), 1u)
        << "throttle starved the band drag of any live preview";

    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::InspectorDrag, false);
}

// SF2's gate: the throttle has to cover EVERY control that moves the bake hash,
// not just the band. Rule Strength is folded into HashModifierState exactly like
// a condition's Min/Max, so dragging the Strength slider on a Global volume
// dirties every resident tile per mouse-move — the same R3 the band arm closes,
// reached through a different widget.
//
// Written against Strength rather than the band deliberately: the band was the
// one control the first implementation wrapped, so a test that only drove it
// could not have caught the four rows that went through their own unwrapped
// handlers.
TEST(TerrainRegionBake, InteractiveRuleStrengthDragThrottleBoundsPreviewCount)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handle = CreateBakedTerrain(svc);
    ASSERT_NE(svc.GetTerrainData(handle), nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTerrainEntity(world, handle);
    auto mod = CreateGlobalRulesModifier(world);

    TerrainModifierSystem system;
    constexpr float32 kIntervalMs = 100.0f;
    system.SetPreviewClockForTests(std::chrono::steady_clock::time_point{});
    system.SetPreviewIntervalForTests(kIntervalMs);
    system.Update(world, 1.0f / 60.0f);

    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::InspectorDrag, true);

    constexpr int32 kFrames = 30;
    constexpr float32 kFrameMs = 16.0f;
    float32 strength = 1.0f;
    for (int32 i = 0; i < kFrames; ++i)
    {
        strength -= 0.02f; // one mouse-move of the Strength slider
        auto* rules = world.GetComponentForWrite<Components::TerrainSurfaceRulesEffect>(mod);
        ASSERT_NE(rules, nullptr);
        rules->Rules[0].Strength = strength;
        system.AdvancePreviewClockForTests(kFrameMs);
        system.Update(world, 1.0f / 60.0f);
    }

    const uint32 maxBakes =
        static_cast<uint32>((static_cast<float32>(kFrames) * kFrameMs) / kIntervalMs) + 1u;
    EXPECT_LE(system.GetDragPreviewBakeCountForTests(), maxBakes)
        << "a Strength drag re-baked more often than the preview cadence allows - its row is "
           "not holding the throttle arm, so every mouse-move re-bakes every resident tile";
    EXPECT_GE(system.GetDragPreviewBakeCountForTests(), 1u)
        << "throttle starved the Strength drag of any live preview";

    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::InspectorDrag, false);
}

// The regression the source-keyed signal exists for.
//
// SceneViewController writes the TransformGizmo source EVERY frame from the
// gizmo's live state — false on all the frames an Inspector drag is running.
// While the signal was one shared bool, an Inspector arming it was therefore
// cleared on the very next frame and the drag re-baked at frame rate anyway:
// the coalescing was reachable in principle and unreachable in practice.
TEST(TerrainRegionBake, GizmoSourceDoesNotClearAnInspectorDragArm)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    using Source = TerrainECS::TerrainService::InteractiveEditSource;

    EXPECT_FALSE(svc.IsInteractiveModifierEdit()) << "default must leave bakes immediate";

    svc.SetInteractiveModifierEdit(Source::InspectorDrag, true);
    EXPECT_TRUE(svc.IsInteractiveModifierEdit());

    // The scene view's every-frame write of its own idle state.
    svc.SetInteractiveModifierEdit(Source::TransformGizmo, false);
    EXPECT_TRUE(svc.IsInteractiveModifierEdit())
        << "the gizmo's idle write cleared an Inspector drag's arm - the two sources are "
           "sharing one bit again";

    // And the converse, so neither source is merely being ignored.
    svc.SetInteractiveModifierEdit(Source::TransformGizmo, true);
    svc.SetInteractiveModifierEdit(Source::InspectorDrag, false);
    EXPECT_TRUE(svc.IsInteractiveModifierEdit());

    svc.SetInteractiveModifierEdit(Source::TransformGizmo, false);
    EXPECT_FALSE(svc.IsInteractiveModifierEdit())
        << "releasing every source must return the terrain to immediate bakes";
}

// A DISCRETE (non-interactive) height edit that grows the global height range
// beyond the committed range must renormalize the whole splat immediately — no
// deferral, no settle pumping — so a single region bake is byte-identical to a
// fresh full bake in one frame. Guards the single-terrain eager branch that
// keeps discrete edits (and the mouse-release settle bake) exact.
TEST(TerrainRegionBake, SingleDiscreteRangeShiftRegionBakeMatchesFullBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handleA = CreateBakedTerrain(svc);
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);
    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTerrainEntity(worldA, handleA);
    auto modA = CreateFlattenModifier(worldA, 40.0f, 40.0f, 20.0f); // norm 0.3125 — in band
    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // full bake commits the in-band range
    ASSERT_TRUE(dataA->SplatBakeRangeValid);

    // TargetHeight 200 > HeightScale (64) escapes the [0,1] procedural band, so the
    // region edit grows the global max past the committed range.
    auto* fmA = worldA.GetComponentForWrite<Components::TerrainFlattenEffect>(modA);
    ASSERT_NE(fmA, nullptr);
    fmA->TargetHeight = 200.0f;
    systemA.Update(worldA, 1.0f / 60.0f); // region height bake + IMMEDIATE full re-splat
    EXPECT_FALSE(dataA->SplatResplatPending) << "a discrete range shift must not defer";

    const TerrainHandle handleB = CreateBakedTerrain(svc);
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);
    ECS::World worldB;
    RegisterModifierLifecycleEvents(worldB);
    CreateTerrainEntity(worldB, handleB);
    CreateFlattenModifier(worldB, 40.0f, 40.0f, 200.0f);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    ASSERT_EQ(dataA->Heightfield.GetSampleCount(), dataB->Heightfield.GetSampleCount());
    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)))
        << "region-bake heightfield != full bake";
    ASSERT_EQ(dataA->Splatmap.size(), dataB->Splatmap.size());
    ASSERT_FALSE(dataA->Splatmap.empty());
    EXPECT_EQ(0, std::memcmp(dataA->Splatmap.data(), dataB->Splatmap.data(), dataA->Splatmap.size()))
        << "discrete range-shift region bake splat != full bake";
}

// PARITY under the deferred range-shift renormalize (edit-realtime, single
// heightfield). A raise that lifts the global max above the committed range
// while an interactive drag is signalled must NOT re-splat the whole map every
// throttled preview (the O(all-samples) hitch that pins the #560 cadence at its
// cap). Instead the touched rect region-splats against the still-committed range
// and the terrain flags a pending flush; once editing goes quiescent the one
// full renormalize runs against the FINAL range. This mirrors the tiled
// TiledDeferredRangeShiftSplatSettlesToFullBake contract for single terrains:
// the settled splat is byte-identical to a fresh full bake, and the deferral
// always settles.
TEST(TerrainRegionBake, SingleInteractiveRangeShiftDefersThenSettlesToFullBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handleA = CreateBakedTerrain(svc);
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);
    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTerrainEntity(worldA, handleA);
    auto modA = CreateFlattenModifier(worldA, 40.0f, 40.0f, 20.0f); // in-band start
    TerrainModifierSystem systemA;
    // Deterministic throttle: injected clock + pinned interval so a preview lands
    // when we advance the clock past it.
    systemA.SetPreviewClockForTests(std::chrono::steady_clock::time_point{});
    systemA.SetPreviewIntervalForTests(50.0f);
    systemA.Update(worldA, 1.0f / 60.0f); // full bake commits the in-band range
    ASSERT_TRUE(dataA->SplatBakeRangeValid);

    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::TransformGizmo, true);
    // Rising-edge drag frame: raises the target out of band. The first bake-worthy
    // drag frame defers once (baseline) without baking.
    {
        auto* fm = worldA.GetComponentForWrite<Components::TerrainFlattenEffect>(modA);
        ASSERT_NE(fm, nullptr);
        fm->TargetHeight = 120.0f; // norm 1.875 -> shifts the max
    }
    systemA.AdvancePreviewClockForTests(60.0f);
    systemA.Update(worldA, 1.0f / 60.0f);

    // Next frame past the interval: a live preview bakes -> range shift -> DEFERRED.
    {
        auto* fm = worldA.GetComponentForWrite<Components::TerrainFlattenEffect>(modA);
        fm->TargetHeight = 200.0f; // norm 3.125 -> grows the max further
    }
    systemA.AdvancePreviewClockForTests(60.0f);
    systemA.Update(worldA, 1.0f / 60.0f);
    EXPECT_GE(systemA.GetDragPreviewBakeCountForTests(), 1u) << "no live preview baked mid-drag";
    EXPECT_TRUE(dataA->SplatResplatPending) << "a range shift mid-drag must defer the re-splat";

    // Release + quiescent frames: the settle bake and the deferred flush run.
    svc.SetInteractiveModifierEdit(
        TerrainECS::TerrainService::InteractiveEditSource::TransformGizmo, false);
    for (int i = 0; i < 5; ++i)
        systemA.Update(worldA, 1.0f / 60.0f);
    EXPECT_FALSE(dataA->SplatResplatPending) << "editing settled -> the deferral must flush";

    // Reference: a fresh full bake of the FINAL modifier state.
    const TerrainHandle handleB = CreateBakedTerrain(svc);
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);
    ECS::World worldB;
    RegisterModifierLifecycleEvents(worldB);
    CreateTerrainEntity(worldB, handleB);
    CreateFlattenModifier(worldB, 40.0f, 40.0f, 200.0f);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    ASSERT_EQ(dataA->Heightfield.GetSampleCount(), dataB->Heightfield.GetSampleCount());
    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)))
        << "settled heightfield != full bake";
    ASSERT_EQ(dataA->Splatmap.size(), dataB->Splatmap.size());
    ASSERT_FALSE(dataA->Splatmap.empty());
    EXPECT_EQ(0, std::memcmp(dataA->Splatmap.data(), dataB->Splatmap.data(), dataA->Splatmap.size()))
        << "settled splat != full bake";
}

// Removing the last modifier must region-reset to the base terrain and match
// a never-modified terrain exactly.
TEST(TerrainRegionBake, ModifierRemovalRestoresBaseTerrain)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handleA = CreateBakedTerrain(svc);
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);

    // Reference: an untouched baked terrain.
    const TerrainHandle handleB = CreateBakedTerrain(svc);
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTerrainEntity(world, handleA);
    auto mod = CreateFlattenModifier(world, 30.0f, 40.0f, 20.0f);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    const uint64 hfCursor = SimulateExtractionConsumed(*dataA);

    world.DestroyEntityImmediate(mod);
    world.SwapLifecycleEvents(); // engine tick's per-frame swap: surface Removed<T>
    system.Update(world, 1.0f / 60.0f);

    DirtyRegionLog::Region hfRegion;
    EXPECT_TRUE(dataA->HeightfieldDirtyLog.CollectSince(hfCursor, hfRegion));
    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)));
    ASSERT_EQ(dataA->Splatmap.size(), dataB->Splatmap.size());
    EXPECT_EQ(0, std::memcmp(dataA->Splatmap.data(), dataB->Splatmap.data(),
                             dataA->Splatmap.size()));
}

// The GPU splat upload may only ride the height-rect band when the CPU did a
// REGION splat re-bake. Whenever ResetSplatmapAndCommitRange runs (full re-bake —
// every texel reset and re-evaluated against the global height range),
// SplatmapFullDirty must be set so extraction uploads full-texture. (Review
// finding on the E2 region uploads: banding a full re-bake leaves stale splat
// weights everywhere outside the band.) Today every height edit takes the
// full-re-bake branch, and paint-only edits take the region branch — assert both.
TEST(TerrainRegionBake, FullSplatRegenFlagsFullDirtyRegionRegenDoesNot)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handle = CreateBakedTerrain(svc);
    auto* data = svc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTerrainEntity(world, handle);
    auto mod = CreateFlattenModifier(world, 30.0f, 40.0f, 20.0f);
    auto paintEntity = world.CreateEntity();
    {
        Components::TerrainModifierVolume paintVol{};
        paintVol.Shape = Components::TerrainVolumeShape::Circle;
        paintVol.Radius = 20.0f;
        paintVol.Falloff = 5.0f;
        world.AddComponentImmediate<Components::TerrainModifierVolume>(paintEntity, paintVol);

        Components::TerrainPaintLayerEffect paint{};
        paint.LayerIndex = 1;
        paint.Strength = 0.8f;
        world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(paintEntity, paint);
        Components::WorldTransform xf{};
        xf.matrix[12] = -30.0f;
        xf.matrix[14] = -30.0f;
        world.AddComponentImmediate<Components::WorldTransform>(paintEntity, xf);
    }

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // initial bake — full regen
    EXPECT_TRUE(data->SplatmapFullDirty) << "initial full bake must force a full-texture upload";
    data->SplatmapDirty = false; // extraction consumed
    data->SplatmapFullDirty = false;

    // Range-shifting height edit: raise the plateau above the noise maximum
    // (raw noise is [0,1] ≈ [0,kHeightScale] world, so 100 world clears it)
    // → global max moves → full splat regen → full upload required.
    world.GetComponentForWrite<Components::TerrainFlattenEffect>(mod)->TargetHeight = 100.0f;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_TRUE(data->SplatmapDirty);
    EXPECT_TRUE(data->SplatmapFullDirty) << "full splat regen must force a full-texture upload";
    data->SplatmapDirty = false;
    data->SplatmapFullDirty = false;

    // Paint-only edit → region splat regen (cached range holds) → the
    // banded upload stays legal.
    world.GetComponentForWrite<Components::TerrainPaintLayerEffect>(paintEntity)->Strength = 0.5f;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_TRUE(data->SplatmapDirty);
    EXPECT_FALSE(data->SplatmapFullDirty) << "region splat regen must not force a full upload";
}

// A paint-only (splatmap) modifier change must not touch the heightfield: no
// HeightfieldVersion bump (which would re-cook the physics collider every
// paint-drag frame), no height re-bake — while the splatmap still matches a
// fresh full bake of the final state.
TEST(TerrainRegionBake, PaintOnlyChangeDoesNotBumpHeightfieldVersion)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handleA = CreateBakedTerrain(svc);
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTerrainEntity(worldA, handleA);
    auto paintEntity = worldA.CreateEntity();
    {
        Components::TerrainModifierVolume paintVol{};
        paintVol.Shape = Components::TerrainVolumeShape::Circle;
        paintVol.Radius = 25.0f;
        paintVol.Falloff = 8.0f;
        worldA.AddComponentImmediate<Components::TerrainModifierVolume>(paintEntity, paintVol);

        Components::TerrainPaintLayerEffect paint{};
        paint.LayerIndex = 1;
        paint.Strength = 0.9f;
        worldA.AddComponentImmediate<Components::TerrainPaintLayerEffect>(paintEntity, paint);
        Components::WorldTransform xf{};
        xf.matrix[12] = 30.0f;
        xf.matrix[14] = 40.0f;
        worldA.AddComponentImmediate<Components::WorldTransform>(paintEntity, xf);
    }

    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // initial bake (full)
    const uint64 hfCursor = SimulateExtractionConsumed(*dataA);

    const uint64 versionAfterFirstBake = dataA->HeightfieldVersion;
    std::vector<float32> heightsBefore(
        dataA->Heightfield.GetRawSamples(),
        dataA->Heightfield.GetRawSamples() + dataA->Heightfield.GetSampleCount());

    // Move the paint stamp (a paint drag).
    auto* xf = worldA.GetComponentForWrite<Components::WorldTransform>(paintEntity);
    ASSERT_NE(xf, nullptr);
    xf->matrix[12] = -20.0f;
    xf->matrix[14] = -35.0f;

    systemA.Update(worldA, 1.0f / 60.0f);

    // No heightfield side effects.
    EXPECT_EQ(dataA->HeightfieldVersion, versionAfterFirstBake);
    DirtyRegionLog::Region hfRegion;
    EXPECT_FALSE(dataA->HeightfieldDirtyLog.CollectSince(hfCursor, hfRegion));
    EXPECT_TRUE(dataA->SplatmapDirty);
    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(), heightsBefore.data(),
                             heightsBefore.size() * sizeof(float32)));

    // Splatmap still matches a fresh full bake of the final state.
    const TerrainHandle handleB = CreateBakedTerrain(svc);
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);

    ECS::World worldB;
    CreateTerrainEntity(worldB, handleB);
    {
        auto e = worldB.CreateEntity();
        Components::TerrainModifierVolume paintVol{};
        paintVol.Shape = Components::TerrainVolumeShape::Circle;
        paintVol.Radius = 25.0f;
        paintVol.Falloff = 8.0f;
        worldB.AddComponentImmediate<Components::TerrainModifierVolume>(e, paintVol);

        Components::TerrainPaintLayerEffect paint{};
        paint.LayerIndex = 1;
        paint.Strength = 0.9f;
        worldB.AddComponentImmediate<Components::TerrainPaintLayerEffect>(e, paint);
        Components::WorldTransform bxf{};
        bxf.matrix[12] = -20.0f;
        bxf.matrix[14] = -35.0f;
        worldB.AddComponentImmediate<Components::WorldTransform>(e, bxf);
    }

    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    ASSERT_EQ(dataA->Splatmap.size(), dataB->Splatmap.size());
    EXPECT_EQ(0, std::memcmp(dataA->Splatmap.data(), dataB->Splatmap.data(),
                             dataA->Splatmap.size()));
}

// The extraction-side normal regen: patching the region (padded by the
// central-difference radius) over an outdated normalmap must reproduce a full
// regeneration bit-exactly.
TEST(TerrainRegionBake, NormalmapRegionMatchesFull)
{
    Terrain::HeightfieldData hf(65, 65);
    hf.FillWithNoise(4.0f, 1.0f, 5, 7);

    std::vector<uint8> normalmap;
    uint32 w = 0, h = 0;
    GenerateNormalmapFromHeightfield(hf, 128.0f, 128.0f, 32.0f, normalmap, w, h);
    ASSERT_EQ(w, 65u);
    ASSERT_EQ(h, 65u);

    // Mutate heights inside a sub-rect (the "modifier bake region").
    constexpr int32 kMinX = 20, kMaxX = 40, kMinZ = 10, kMaxZ = 30;
    for (int32 z = kMinZ; z <= kMaxZ; ++z)
        for (int32 x = kMinX; x <= kMaxX; ++x)
            hf.SetSample(static_cast<uint32>(x), static_cast<uint32>(z),
                         hf.GetSample(static_cast<uint32>(x), static_cast<uint32>(z)) + 0.25f);

    // Reference: full regeneration from the mutated heights.
    std::vector<uint8> fullMap;
    uint32 fw = 0, fh = 0;
    GenerateNormalmapFromHeightfield(hf, 128.0f, 128.0f, 32.0f, fullMap, fw, fh);

    // Region patch of the outdated map, padded by 1 for central differences.
    GenerateNormalmapRegionFromHeightfield(hf, 128.0f, 128.0f, 32.0f, normalmap,
                                           kMinX - 1, kMinZ - 1, kMaxX + 1, kMaxZ + 1);

    ASSERT_EQ(normalmap.size(), fullMap.size());
    EXPECT_EQ(0, std::memcmp(normalmap.data(), fullMap.data(), normalmap.size()));
}

// The region reset must clear EXACTLY its rect and nothing else. Zeroing IS the
// unbaked base now, so an off-by-one here never shows up as a wrong colour — it
// silently erases material the rules bake placed outside the dirty rect, or
// leaves stale weights inside it. Seeded with a non-zero pattern, because a reset
// checked against an already-zero buffer proves nothing.
// WHY THE UNBAKED BASE IS ZERO, as an assertion rather than a comment.
//
// CompositeSplatTexel ADDS a row's weight into the texel and then renormalizes the
// whole texel. So whatever the base already holds stays in the denominator forever:
// with a channel-0-full base, a row asking for ALL of channel 2 at weight 1 lands as
// [1,0,1,0] and renormalizes to 50/50 - a fully-dirt slope reads half grass, and no
// later row can undo it (the next row only adds another term to the same sum).
//
// Zero is the only base that lets a row mean what it says. This is the cheapest place
// that decision can fail, and it fails LOUDLY here rather than as a washed-out look
// nobody can source.
TEST(TerrainRegionBake, AFullWeightRowReachesItsChannelCompletelyFromTheUnbakedBase)
{
    Terrain::HeightfieldData hf(8, 8);
    std::vector<uint8> splat;
    uint32 w = 0, h = 0;
    ResetSplatmap(hf, splat, w, h);
    ASSERT_EQ(splat.size(), static_cast<size_t>(8) * 8 * 4);

    // One row, full weight, on a channel that is NOT channel 0 - the case a channel-0-full
    // base caps at half.
    constexpr uint32 kLayer = 2;
    uint8* pixel = &splat[0];
    CompositeSplatTexel(pixel, kLayer, 1.0f, /*replace*/ false);

    EXPECT_EQ(pixel[kLayer], 255)
        << "a full-weight row reached only " << static_cast<int>(pixel[kLayer])
        << "/255 of its channel: the base it composited onto is holding weight it should not";
    EXPECT_EQ(pixel[0], 0) << "channel 0 carries weight no row asked for";
    EXPECT_EQ(pixel[1], 0);
    EXPECT_EQ(pixel[3], 0);

    // And a SECOND row on a third channel splits with the first rather than with a phantom
    // base: two full-weight rows are 50/50 between THEMSELVES, which is the intended
    // renormalize, not evidence of a base.
    CompositeSplatTexel(pixel, 1u, 1.0f, /*replace*/ false);
    EXPECT_NEAR(static_cast<int>(pixel[kLayer]), 127, 1);
    EXPECT_NEAR(static_cast<int>(pixel[1]), 127, 1);
    EXPECT_EQ(pixel[0], 0);
}

TEST(TerrainRegionBake, SplatmapRegionResetClearsExactlyItsRect)
{
    Terrain::HeightfieldData hf(65, 65);
    hf.FillWithNoise(4.0f, 1.0f, 5, 11);

    std::vector<uint8> splat;
    uint32 w = 0, h = 0;
    ResetSplatmap(hf, splat, w, h);
    ASSERT_EQ(w, 65u);
    ASSERT_EQ(h, 65u);
    ASSERT_EQ(splat.size(), static_cast<size_t>(65) * 65 * 4);

    // A varying non-zero byte per index, so a misplaced clear is detectable rather
    // than masked by a uniform fill.
    for (size_t i = 0; i < splat.size(); ++i)
        splat[i] = static_cast<uint8>((i % 251) + 1);

    constexpr int32 kMinX = 25, kMaxX = 45, kMinZ = 15, kMaxZ = 35;
    ResetSplatmapRegion(hf, splat, kMinX, kMinZ, kMaxX, kMaxZ);

    size_t clearedInside = 0;
    size_t survivedOutside = 0;
    for (int32 z = 0; z < 65; ++z)
        for (int32 x = 0; x < 65; ++x)
        {
            const size_t idx = (static_cast<size_t>(z) * 65 + x) * 4;
            const bool inside = x >= kMinX && x <= kMaxX && z >= kMinZ && z <= kMaxZ;
            const bool zeroed = splat[idx + 0] == 0 && splat[idx + 1] == 0 &&
                                splat[idx + 2] == 0 && splat[idx + 3] == 0;
            clearedInside += (inside && zeroed) ? 1u : 0u;
            survivedOutside += (!inside && !zeroed) ? 1u : 0u;
        }

    const size_t insideCount = static_cast<size_t>(kMaxX - kMinX + 1) *
                               static_cast<size_t>(kMaxZ - kMinZ + 1);
    EXPECT_EQ(clearedInside, insideCount);
    EXPECT_EQ(survivedOutside, static_cast<size_t>(65) * 65 - insideCount);
}

// Steady-state quiescence: with idle (untouched) modifiers, every tick after
// the first must do ZERO bake work. Tile residency is no longer folded into the
// terrain-state hash (that used to turn every frame into a full re-bake + a
// heightfield collider re-cook — the idle-modifier bake loop, 1.4 FPS in the
// editor); quiescence now rests on the per-tile ModifiersApplied flags plus the
// post-apply hash re-baseline. Revision + HeightfieldVersion must stay put idle.
TEST(TerrainRegionBake, TiledIdleModifierDoesNotRebakeEveryTick)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 256.0f;
    cfg.WorldSizeZ = 256.0f;
    cfg.HeightScale = 64.0f;
    cfg.SamplesPerMeter = 1.0f;
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    auto* tile = svc.LoadTile(handle, TileCoord{0, 0});
    ASSERT_NE(tile, nullptr);
    // The synchronous LoadTile path leaves LodState at Empty; the modifier
    // bake only touches Full tiles (streaming sets Full via SetTileHeightfield).
    tile->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    auto terrainEntity = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = cfg.WorldSizeX;
    terrain.SizeZ = cfg.WorldSizeZ;
    terrain.HeightScale = cfg.HeightScale;
    terrain.TiledTerrainHandle = handle.Index;
    terrain.TiledTerrainGeneration = handle.Generation;
    world.AddComponentImmediate<Components::Terrain>(terrainEntity, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(terrainEntity,
                                                            Components::WorldTransform{});
    CreateFlattenModifier(world, 30.0f, 40.0f, 20.0f);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // initial bake applies the modifier

    const uint64 revisionAfterFirstBake = tiled->Revision;
    const uint64 tileVersionAfterFirstBake = tile->HeightfieldVersion;
    ASSERT_GT(revisionAfterFirstBake, 0u);

    // Idle ticks: nothing changed, so no bake may run — Revision and the
    // tile's HeightfieldVersion must not move.
    for (int i = 0; i < 3; ++i)
        system.Update(world, 1.0f / 60.0f);

    EXPECT_EQ(tiled->Revision, revisionAfterFirstBake);
    EXPECT_EQ(tile->HeightfieldVersion, tileVersionAfterFirstBake);
}

// C8 fix-round-3 perf oracle: the tiled global height range is an O(tiles) aggregate of
// cached per-tile min/max — an idle tiled terrain must NOT rescan any tile's samples per
// frame. Pre-fix the range was a full O(all-samples) scan of every resident tile on every
// bake; combined with a per-frame bake trigger that was the tiled-idle fps sink. This
// counts full-tile min/max scans and asserts idle ticks add ZERO (the discriminating
// invariant: a per-frame sample rescan would climb the counter).
TEST(TerrainRegionBake, TiledIdleAddsNoPerFrameHeightScans)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    // Multi-tile terrain so an all-tiles rescan would be visibly larger than a
    // touched-tiles rescan (2048m @ 1/m -> 1024m tiles -> 2x2 = 4 tiles).
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 2048.0f;
    cfg.WorldSizeZ = 2048.0f;
    cfg.HeightScale = 64.0f;
    cfg.SamplesPerMeter = 1.0f;
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    ASSERT_GE(tiled->Config.TilesPerAxisX, 2u);
    for (int32 tz = 0; tz < static_cast<int32>(tiled->Config.TilesPerAxisZ); ++tz)
        for (int32 tx = 0; tx < static_cast<int32>(tiled->Config.TilesPerAxisX); ++tx)
        {
            auto* t = svc.LoadTile(handle, TileCoord{tx, tz});
            ASSERT_NE(t, nullptr);
            t->LodState = TileLodState::Full;
        }

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    auto terrainEntity = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = cfg.WorldSizeX;
    terrain.SizeZ = cfg.WorldSizeZ;
    terrain.HeightScale = cfg.HeightScale;
    terrain.TiledTerrainHandle = handle.Index;
    terrain.TiledTerrainGeneration = handle.Generation;
    world.AddComponentImmediate<Components::Terrain>(terrainEntity, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(terrainEntity, Components::WorldTransform{});
    CreateFlattenModifier(world, 512.0f, 512.0f, 64.0f);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // initial bake — rescans the baked tiles
    const uint64 scansAfterBake = TerrainModifierSystem::GetTileHeightRescanCountForTests();

    // Idle ticks: no input changed, so no bake and NO sample rescans may occur.
    for (int i = 0; i < 5; ++i)
        system.Update(world, 1.0f / 60.0f);

    EXPECT_EQ(TerrainModifierSystem::GetTileHeightRescanCountForTests(), scansAfterBake)
        << "idle tiled terrain rescanned tile heights every frame — O(samples) idle regression";
}

// Stamp-mask parity (E3, design §9.1): a stamp sampling a decoded mask must
// region re-bake bit-identically to a fresh full bake of the final state —
// and the mask must actually shape the displacement (a maskless stamp bakes
// differently).
TEST(TerrainRegionBake, StampMaskRegionBakeMatchesFullBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const GUID maskGuid = GUID::Generate();
    const std::vector<float32> maskTexels = MakeGradientMask(16, 16);

    // Terrain A: masked stamp bake, then a move → region re-bake.
    const TerrainHandle handleA = CreateBakedTerrain(svc);
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTerrainEntity(worldA, handleA);
    auto stampA = CreateStampModifier(worldA, 30.0f, 40.0f, maskGuid);

    TerrainModifierSystem systemA;
    systemA.SeedDecodedStampMaskForTests(maskGuid, maskTexels, 16, 16);
    systemA.Update(worldA, 1.0f / 60.0f); // initial bake (full: no baseline yet)
    const uint64 hfCursor = SimulateExtractionConsumed(*dataA);

    // The mask must have shaped the result: bake a maskless stamp elsewhere
    // and require a different heightfield.
    {
        const TerrainHandle handleC = CreateBakedTerrain(svc);
        auto* dataC = svc.GetTerrainData(handleC);
        ASSERT_NE(dataC, nullptr);
        ECS::World worldC;
        CreateTerrainEntity(worldC, handleC);
        CreateStampModifier(worldC, 30.0f, 40.0f, GUID{});
        TerrainModifierSystem systemC;
        systemC.Update(worldC, 1.0f / 60.0f);
        EXPECT_NE(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                                 dataC->Heightfield.GetRawSamples(),
                                 dataA->Heightfield.GetSampleCount() * sizeof(float32)))
            << "masked stamp must bake differently from a flat (maskless) stamp";
    }

    // Move the stamp → region re-bake over old ∪ new bounds.
    auto* xf = worldA.GetComponentForWrite<Components::WorldTransform>(stampA);
    ASSERT_NE(xf, nullptr);
    xf->matrix[12] = -20.0f;
    xf->matrix[14] = -35.0f;
    systemA.Update(worldA, 1.0f / 60.0f);

    DirtyRegionLog::Region hfRegion;
    ASSERT_TRUE(dataA->HeightfieldDirtyLog.CollectSince(hfCursor, hfRegion));
    EXPECT_GT(hfRegion.MinX, 0);
    EXPECT_GT(hfRegion.MinZ, 0);
    EXPECT_LT(hfRegion.MaxX, static_cast<int32>(kHeightmapDim));
    EXPECT_LT(hfRegion.MaxZ, static_cast<int32>(kHeightmapDim));

    // Terrain B: fresh full bake of the FINAL state, same mask content.
    const TerrainHandle handleB = CreateBakedTerrain(svc);
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);

    ECS::World worldB;
    CreateTerrainEntity(worldB, handleB);
    CreateStampModifier(worldB, -20.0f, -35.0f, maskGuid);

    TerrainModifierSystem systemB;
    systemB.SeedDecodedStampMaskForTests(maskGuid, maskTexels, 16, 16);
    systemB.Update(worldB, 1.0f / 60.0f);

    ASSERT_EQ(dataA->Heightfield.GetSampleCount(), dataB->Heightfield.GetSampleCount());
    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)));
    ASSERT_EQ(dataA->Splatmap.size(), dataB->Splatmap.size());
    ASSERT_FALSE(dataA->Splatmap.empty());
    EXPECT_EQ(0, std::memcmp(dataA->Splatmap.data(), dataB->Splatmap.data(),
                             dataA->Splatmap.size()));
}

// The asset-reload lane: an enqueued invalidation for a referenced mask must
// evict the decoded entry, force a gather past the change gate (no ECS state
// changed), and re-bake with the re-decoded content. Without an AssetManager
// the re-decode negative-caches to flat white — the result must match a
// maskless bake of the same stamp.
TEST(TerrainRegionBake, StampMaskHotReloadEvictionRebakes)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const GUID maskGuid = GUID::Generate();

    const TerrainHandle handleA = CreateBakedTerrain(svc);
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTerrainEntity(world, handleA);
    CreateStampModifier(world, 30.0f, 40.0f, maskGuid);

    TerrainModifierSystem system;
    system.SeedDecodedStampMaskForTests(maskGuid, MakeGradientMask(16, 16), 16, 16);
    system.Update(world, 1.0f / 60.0f); // initial masked bake
    const uint64 versionAfterMaskedBake = dataA->HeightfieldVersion;

    // Idle tick: nothing changed, no bake.
    system.Update(world, 1.0f / 60.0f);
    ASSERT_EQ(dataA->HeightfieldVersion, versionAfterMaskedBake);

    // Simulate the watcher-thread reload event, then tick: drain → evict →
    // forced gather → content-version bump → re-bake (flat white decode).
    svc.EnqueueAssetInvalidation(maskGuid);
    system.Update(world, 1.0f / 60.0f);
    EXPECT_GT(dataA->HeightfieldVersion, versionAfterMaskedBake)
        << "evicting a referenced mask must re-bake";

    // Reference: a maskless stamp bake of the same state.
    const TerrainHandle handleB = CreateBakedTerrain(svc);
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);
    ECS::World worldB;
    CreateTerrainEntity(worldB, handleB);
    CreateStampModifier(worldB, 30.0f, 40.0f, GUID{});
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)));
}

// Heightmap base source (E3, design §9.2 + §3.1): region re-bakes over a
// heightmap-based terrain must reproduce a fresh full bake bit-exactly, and
// the base must actually come from the decoded heightmap.
TEST(TerrainRegionBake, HeightmapBaseRegionBakeMatchesFullBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    // Decoded source at a different resolution than the terrain (33 vs 129)
    // so the bilinear resample path is exercised.
    const GUID heightmapGuid = GUID::Generate();
    Terrain::HeightfieldData source(33, 33);
    source.FillWithNoise(3.0f, 1.0f, 4, 99);
    svc.SeedDecodedHeightmapForTests(heightmapGuid, source);

    const TerrainHandle handleA = CreateBakedTerrainWithBase(
        svc, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);

    // Base sampled from the heightmap: check a sample far from the modifier.
    const float32 inv = 1.0f / static_cast<float32>(kHeightmapDim - 1);
    EXPECT_FLOAT_EQ(dataA->Heightfield.GetSample(2, 2),
                    source.SampleBilinear(2.0f * inv, 2.0f * inv));

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTerrainEntity(worldA, handleA, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);
    auto modA = CreateFlattenModifier(worldA, 30.0f, 40.0f, 20.0f);

    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // initial bake (full: no baseline yet)
    const uint64 hfCursor = SimulateExtractionConsumed(*dataA);

    auto* xf = worldA.GetComponentForWrite<Components::WorldTransform>(modA);
    ASSERT_NE(xf, nullptr);
    xf->matrix[12] = -20.0f;
    xf->matrix[14] = -35.0f;
    systemA.Update(worldA, 1.0f / 60.0f);

    // Region-scoped second bake.
    DirtyRegionLog::Region hfRegion;
    ASSERT_TRUE(dataA->HeightfieldDirtyLog.CollectSince(hfCursor, hfRegion));
    EXPECT_GT(hfRegion.MinX, 0);
    EXPECT_GT(hfRegion.MinZ, 0);
    EXPECT_LT(hfRegion.MaxX, static_cast<int32>(kHeightmapDim));
    EXPECT_LT(hfRegion.MaxZ, static_cast<int32>(kHeightmapDim));

    // Fresh full bake of the final state over the same source.
    const TerrainHandle handleB = CreateBakedTerrainWithBase(
        svc, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);

    ECS::World worldB;
    CreateTerrainEntity(worldB, handleB, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);
    CreateFlattenModifier(worldB, -20.0f, -35.0f, 20.0f);

    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)));
    ASSERT_EQ(dataA->Splatmap.size(), dataB->Splatmap.size());
    ASSERT_FALSE(dataA->Splatmap.empty());
    EXPECT_EQ(0, std::memcmp(dataA->Splatmap.data(), dataB->Splatmap.data(),
                             dataA->Splatmap.size()));
}

// Flat base source: same parity contract, base is constant zero.
TEST(TerrainRegionBake, FlatBaseRegionBakeMatchesFullBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handleA = CreateBakedTerrainWithBase(
        svc, Components::TerrainBaseSource::Flat, GUID{});
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);
    EXPECT_EQ(dataA->Heightfield.GetSample(2, 2), 0.0f);

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTerrainEntity(worldA, handleA, Components::TerrainBaseSource::Flat);
    auto modA = CreateFlattenModifier(worldA, 30.0f, 40.0f, 20.0f);

    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f);

    auto* xf = worldA.GetComponentForWrite<Components::WorldTransform>(modA);
    ASSERT_NE(xf, nullptr);
    xf->matrix[12] = -20.0f;
    xf->matrix[14] = -35.0f;
    systemA.Update(worldA, 1.0f / 60.0f);

    const TerrainHandle handleB = CreateBakedTerrainWithBase(
        svc, Components::TerrainBaseSource::Flat, GUID{});
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);

    ECS::World worldB;
    CreateTerrainEntity(worldB, handleB, Components::TerrainBaseSource::Flat);
    CreateFlattenModifier(worldB, -20.0f, -35.0f, 20.0f);

    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)));
    ASSERT_EQ(dataA->Splatmap.size(), dataB->Splatmap.size());
    EXPECT_EQ(0, std::memcmp(dataA->Splatmap.data(), dataB->Splatmap.data(),
                             dataA->Splatmap.size()));
}

// Work quiescence (#405 pattern) for heightmap-based terrains: the base
// source folds into the terrain-state hash (GUID + content version), which
// must NOT self-trigger — idle ticks after the first bake do zero bake work.
TEST(TerrainRegionBake, HeightmapIdleTerrainDoesNotRebake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const GUID heightmapGuid = GUID::Generate();
    Terrain::HeightfieldData source(33, 33);
    source.FillWithNoise(3.0f, 1.0f, 4, 99);
    svc.SeedDecodedHeightmapForTests(heightmapGuid, source);

    const TerrainHandle handle = CreateBakedTerrainWithBase(
        svc, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);
    auto* data = svc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTerrainEntity(world, handle, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);
    CreateFlattenModifier(world, 30.0f, 40.0f, 20.0f);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // initial bake applies the modifier
    const uint64 versionAfterFirstBake = data->HeightfieldVersion;
    const uint64 hfCursor = SimulateExtractionConsumed(*data);
    ASSERT_GT(versionAfterFirstBake, 0u);

    for (int i = 0; i < 3; ++i)
        system.Update(world, 1.0f / 60.0f);

    EXPECT_EQ(data->HeightfieldVersion, versionAfterFirstBake);
    DirtyRegionLog::Region hfRegion;
    EXPECT_FALSE(data->HeightfieldDirtyLog.CollectSince(hfCursor, hfRegion));
    EXPECT_FALSE(data->SplatmapDirty);
}

// Heightmap content eviction with modifiers present: the reload lane must
// force a full re-bake, and the re-decode failure (no AssetManager in tests)
// must land on the documented flat fallback — matching a fresh full bake
// over an unresolvable heightmap source.
TEST(TerrainRegionBake, HeightmapEvictionRebakesModifiedTerrain)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const GUID heightmapGuid = GUID::Generate();
    Terrain::HeightfieldData source(33, 33);
    source.FillWithNoise(3.0f, 1.0f, 4, 99);
    svc.SeedDecodedHeightmapForTests(heightmapGuid, source);

    const TerrainHandle handleA = CreateBakedTerrainWithBase(
        svc, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);
    auto* dataA = svc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTerrainEntity(worldA, handleA, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);
    CreateFlattenModifier(worldA, 30.0f, 40.0f, 20.0f);

    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // initial heightmap-based bake
    const uint64 versionAfterFirstBake = dataA->HeightfieldVersion;

    // Idle tick: quiet.
    systemA.Update(worldA, 1.0f / 60.0f);
    ASSERT_EQ(dataA->HeightfieldVersion, versionAfterFirstBake);

    // Watcher-thread reload event → drain → evict → full re-bake. The
    // re-decode fails without an AssetManager, so the base falls back flat.
    svc.EnqueueAssetInvalidation(heightmapGuid);
    systemA.Update(worldA, 1.0f / 60.0f);
    EXPECT_GT(dataA->HeightfieldVersion, versionAfterFirstBake)
        << "evicting a referenced heightmap must re-bake";

    // Reference: fresh full bake with an unresolvable heightmap source.
    const GUID missingGuid = GUID::Generate();
    const TerrainHandle handleB = CreateBakedTerrainWithBase(
        svc, Components::TerrainBaseSource::HeightmapAsset, missingGuid);
    auto* dataB = svc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);

    ECS::World worldB;
    CreateTerrainEntity(worldB, handleB, Components::TerrainBaseSource::HeightmapAsset, missingGuid);
    CreateFlattenModifier(worldB, 30.0f, 40.0f, 20.0f);

    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)));
    ASSERT_EQ(dataA->Splatmap.size(), dataB->Splatmap.size());
    EXPECT_EQ(0, std::memcmp(dataA->Splatmap.data(), dataB->Splatmap.data(),
                             dataA->Splatmap.size()));
}

// Modifier-free heightmap terrain (the natural authoring state for pure
// imports) must be completely idle: no bake work on any tick after creation.
TEST(TerrainRegionBake, ModifierFreeHeightmapIdleDoesNotRebake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const GUID heightmapGuid = GUID::Generate();
    Terrain::HeightfieldData source(33, 33);
    source.FillWithNoise(3.0f, 1.0f, 4, 99);
    svc.SeedDecodedHeightmapForTests(heightmapGuid, source);

    const TerrainHandle handle = CreateBakedTerrainWithBase(
        svc, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);
    auto* data = svc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);
    const uint64 versionAfterCreation = data->HeightfieldVersion;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTerrainEntity(world, handle, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);

    TerrainModifierSystem system;
    for (int i = 0; i < 4; ++i)
        system.Update(world, 1.0f / 60.0f);

    EXPECT_EQ(data->HeightfieldVersion, versionAfterCreation)
        << "idle modifier-free heightmap terrain must never re-bake";
}

// A heightmap content eviction must re-fill a MODIFIER-FREE terrain too: the
// empty-modifier hash sentinel can't see it, so the base-input baseline is
// the trigger — and post-bake it must go quiet again (#405 discipline).
TEST(TerrainRegionBake, ModifierFreeHeightmapEvictionRefillsBase)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const GUID heightmapGuid = GUID::Generate();
    Terrain::HeightfieldData source(33, 33);
    source.FillWithNoise(3.0f, 1.0f, 4, 99);
    svc.SeedDecodedHeightmapForTests(heightmapGuid, source);

    const TerrainHandle handle = CreateBakedTerrainWithBase(
        svc, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);
    auto* data = svc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);
    const uint64 versionAfterCreation = data->HeightfieldVersion;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTerrainEntity(world, handle, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // seeds the base-input baseline, no bake
    ASSERT_EQ(data->HeightfieldVersion, versionAfterCreation);
    const float32 inv = 1.0f / static_cast<float32>(kHeightmapDim - 1);
    ASSERT_FLOAT_EQ(data->Heightfield.GetSample(2, 2),
                    source.SampleBilinear(2.0f * inv, 2.0f * inv));

    // Watcher-thread reload event → drain → evict → base-only re-fill. The
    // re-decode fails without an AssetManager, so the base lands flat.
    svc.EnqueueAssetInvalidation(heightmapGuid);
    system.Update(world, 1.0f / 60.0f);
    EXPECT_GT(data->HeightfieldVersion, versionAfterCreation)
        << "heightmap eviction must re-fill even with zero modifiers";
    EXPECT_EQ(data->Heightfield.GetSample(2, 2), 0.0f);
    EXPECT_EQ(data->Heightfield.GetSample(kHeightmapDim / 2, kHeightmapDim / 2), 0.0f);

    // Post-refill quiescence: the baseline re-seeds with the bake.
    const uint64 versionAfterRefill = data->HeightfieldVersion;
    for (int i = 0; i < 3; ++i)
        system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(data->HeightfieldVersion, versionAfterRefill);
}

// ============================================================================
// E6: tiled terrain region bake
// ============================================================================

namespace
{
// A tiled terrain whose grid has `tilesX` tiles across (WorldSizeX =
// tilesX * 1024 at 1 sample/m, so tileWorld caps at 1024) and one tile deep.
// LoadTile fills each tile with world-space noise; we flip LodState to Full
// (streaming sets Full in the editor; the modifier bake only touches Full
// tiles). Every tile is 1025x1025 — that is the fixed per-tile cap.
constexpr float32 kTiledTileWorld = 1024.0f;
constexpr float32 kTiledHeightScale = 64.0f;

TiledTerrainHandle CreateResidentTiledTerrain(TerrainService& svc, uint32 tilesX)
{
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = static_cast<float32>(tilesX) * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    for (uint32 z = 0; z < tiled->Config.TilesPerAxisZ; ++z)
        for (uint32 x = 0; x < tiled->Config.TilesPerAxisX; ++x)
        {
            auto* tile = svc.LoadTile(handle, TileCoord{static_cast<int32>(x), static_cast<int32>(z)});
            tile->LodState = TileLodState::Full;
        }
    return handle;
}

ECS::EntityHandle CreateTiledTerrainEntity(ECS::World& world, TiledTerrainHandle handle, uint32 tilesX)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = static_cast<float32>(tilesX) * kTiledTileWorld;
    terrain.SizeZ = kTiledTileWorld;
    terrain.HeightScale = kTiledHeightScale;
    terrain.TiledTerrainHandle = handle.Index;
    terrain.TiledTerrainGeneration = handle.Generation;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    return e;
}

TerrainTileData* Tile(TiledTerrainData* tiled, int32 x, int32 z)
{
    return tiled->Tiles.at(TileCoord{x, z}).get();
}

// Square NxN resident tiled terrain (the timing harness models the editor's 2048m/spm2
// case = 4x4 tiles of 1025^2). All tiles Full-resident from the start.
TiledTerrainHandle CreateResidentSquareTiledTerrain(TerrainService& svc, uint32 tilesPerAxis)
{
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = static_cast<float32>(tilesPerAxis) * kTiledTileWorld;
    cfg.WorldSizeZ = static_cast<float32>(tilesPerAxis) * kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    for (uint32 z = 0; z < tiled->Config.TilesPerAxisZ; ++z)
        for (uint32 x = 0; x < tiled->Config.TilesPerAxisX; ++x)
            svc.LoadTile(handle, TileCoord{static_cast<int32>(x), static_cast<int32>(z)})->LodState =
                TileLodState::Full;
    return handle;
}

ECS::EntityHandle CreateSquareTiledTerrainEntity(ECS::World& world, TiledTerrainHandle handle,
                                                 uint32 tilesPerAxis)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = static_cast<float32>(tilesPerAxis) * kTiledTileWorld;
    terrain.SizeZ = static_cast<float32>(tilesPerAxis) * kTiledTileWorld;
    terrain.HeightScale = kTiledHeightScale;
    terrain.TiledTerrainHandle = handle.Index;
    terrain.TiledTerrainGeneration = handle.Generation;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    return e;
}
} // namespace

// TIMING HARNESS (DISABLED — run with --gtest_also_run_disabled_tests). Not an
// assertion: measures the per-dab modifier-system CPU cost breakdown and the worst
// settle frame on a 16-tile (4x4 x 1025^2) terrain — the editor's 2048m/spm2 case — so
// the sculpt-residual A/B (Fix 2 spread vs one-shot via GE_TERRAIN_SPREAD_SETTLE) has
// concrete ms numbers, and quantifies Fix 1's CPU normal-regen (whole-tile vs region).
// The GPU-upload half of Fix 1 is measured separately (analytical / editor). Prints via
// the CpuProfiler scopes; run twice (default vs GE_TERRAIN_SPREAD_SETTLE=0) to A/B.
TEST(TerrainRegionBake, DISABLED_SculptStrokeTiming)
{
    using Clock = std::chrono::steady_clock;
    auto& prof = GameEngine::Profiling::CpuProfiler::Get();
    prof.SetEnabled(true);

    // GE_TERRAIN_HARNESS_AXIS: 4 = editor 2048m/spm2 (16 tiles); 8 = 4096m (64 tiles).
    uint32 axis = 4;
    if (const char* a = std::getenv("GE_TERRAIN_HARNESS_AXIS"))
    {
        const long v = std::strtol(a, nullptr, 10);
        if (v >= 1 && v <= 16) axis = static_cast<uint32>(v);
    }

    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentSquareTiledTerrain(svc, axis);
    auto* tiled = svc.GetTiledTerrainData(handle);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateSquareTiledTerrainEntity(world, handle, axis);
    auto mod = CreateFlattenModifier(world, 200.0f, 200.0f, 300.0f); // raise well above the band
    TerrainModifierSystem system;

    prof.BeginFrame();
    system.Update(world, 1.0f / 60.0f); // full bake (warm)

    // Scripted raise stroke: sweep the flatten across tiles AND grow its target each dab so
    // the global height max climbs (a real raise brush accumulates height) — every dab shifts
    // the splat range, arming the deferred whole-terrain renormalize the settle then drains.
    constexpr int kDabs = 24;
    double dabMsMax = 0.0, dabMsSum = 0.0;
    for (int d = 0; d < kDabs; ++d)
    {
        auto* xf = world.GetComponentForWrite<Components::WorldTransform>(mod);
        xf->matrix[12] = 200.0f + static_cast<float32>(d) * 60.0f; // sweep across X (crosses tiles)
        xf->matrix[14] = 200.0f + static_cast<float32>(d % 4) * 40.0f;
        auto* fm = world.GetComponentForWrite<Components::TerrainFlattenEffect>(mod);
        fm->TargetHeight = 120.0f + static_cast<float32>(d) * 16.0f; // climbs the global max each dab
        prof.BeginFrame();
        const auto t0 = Clock::now();
        system.Update(world, 1.0f / 60.0f);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        dabMsSum += ms;
        dabMsMax = std::max(dabMsMax, ms);
    }

    // Settle: time each frame from stroke release until quiescent. The worst single frame is
    // the settle-spike bound (pre-fix: the whole-terrain renormalize in one frame; fix: bounded).
    double settleWorst = 0.0, settleTotal = 0.0;
    int settleFrames = 0;
    for (int i = 0; i < 8192 && tiled->SplatResplatPending; ++i)
    {
        prof.BeginFrame();
        const auto t0 = Clock::now();
        system.Update(world, 1.0f / 60.0f);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        ++settleFrames;
        settleTotal += ms;
        settleWorst = std::max(settleWorst, ms);
    }

    const char* spreadEnv = std::getenv("GE_TERRAIN_SPREAD_SETTLE");
    const bool spread = spreadEnv == nullptr || spreadEnv[0] != '0';
    std::printf("\n[SculptStrokeTiming] mode=%s  tiles=%ux%u  res=1025\n",
                spread ? "SPREAD(fix)" : "one-shot(pre-fix)", axis, axis);
    std::printf("  per-dab (modifier CPU only, excludes GPU upload): avg %.3f ms  worst %.3f ms  "
                "over %d dabs\n", dabMsSum / kDabs, dabMsMax, kDabs);
    std::printf("  SETTLE: %d frames  total %.1f ms  WORST-FRAME %.3f ms\n",
                settleFrames, settleTotal, settleWorst);

    // Per-dab COST ATTRIBUTION. The engine-side GE_CPU_PROFILE scopes record to a
    // different CpuProfiler instance than a unit test can enable (header-only
    // singleton duplicated across the static-lib boundary), so attribute the
    // per-dab cost by micro-benching the same primitives the region bake runs on
    // one 1025^2 tile: the full-tile min/max rescan (RefreshTileHeightRange, run
    // once per touched tile), a dab-sized region noise refill, a dab-sized region
    // splat regen, and the region-vs-whole normal regen. A dab that crosses N
    // tiles pays roughly N x (min/max + region-noise + region-splat + region-normal).
    auto* t = Tile(tiled, 0, 0);
    const uint32 tw = t->Heightfield.GetWidth(), th = t->Heightfield.GetHeight();
    constexpr int reps = 8;

    // Full-tile min/max scan (the RefreshTileHeightRange shape).
    auto tm0 = Clock::now();
    volatile float32 sink = 0.0f;
    for (int r = 0; r < reps; ++r)
    {
        float32 mn = 1e30f, mx = -1e30f;
        for (uint32 z = 0; z < th; ++z)
            for (uint32 x = 0; x < tw; ++x)
            {
                const float32 s = t->Heightfield.GetSample(x, z);
                mn = std::min(mn, s); mx = std::max(mx, s);
            }
        sink = mn + mx;
    }
    const double minmaxMs = std::chrono::duration<double, std::milli>(Clock::now() - tm0).count() / reps;

    // Dab-sized region noise refill (48x48).
    auto tn0 = Clock::now();
    for (int r = 0; r < reps; ++r)
        t->Heightfield.FillRegionWithNoiseWorldSpace(kTileNoiseFrequency, kTileNoiseAmplitude,
            t->WorldOriginX, t->WorldOriginZ, 1024.0f, 1024.0f, 500, 500, 548, 548,
            kTileNoiseOctaves, kTileNoiseSeed);
    const double regionNoiseMs = std::chrono::duration<double, std::milli>(Clock::now() - tn0).count() / reps;

    // Dab-sized region splat RESET (48x48). This is the clear only — the rule
    // evaluation that now places the material is not measured here, so this number
    // is a floor on the splat cost of a dab, not the cost.
    auto ts0 = Clock::now();
    for (int r = 0; r < reps; ++r)
        ResetSplatmapRegion(t->Heightfield, t->Splatmap, 500, 500, 548, 548);
    const double regionSplatMs = std::chrono::duration<double, std::milli>(Clock::now() - ts0).count() / reps;

    std::printf("  PER-DAB PRIMITIVE COSTS (one 1025^2 tile, %s runtime):\n",
#ifdef NDEBUG
                "release"
#else
                "debug/debugfast"
#endif
    );
    std::printf("      full-tile min/max rescan (per touched tile): %.4f ms\n", minmaxMs);
    std::printf("      region noise refill (48x48 dab):             %.4f ms\n", regionNoiseMs);
    std::printf("      region splat reset  (48x48 dab, clear only): %.4f ms\n", regionSplatMs);
    (void)sink;

    // Fix 1 CPU component: whole-tile vs region normal regen on one 1025^2 tile.

    // Fix 1 CPU component: whole-tile vs region normal regen on one 1025^2 tile.
    std::vector<uint8> nm;
    uint32 nw = 0, nh = 0;
    auto tw0 = Clock::now();
    for (int r = 0; r < reps; ++r)
        GenerateNormalmapFromHeightfield(t->Heightfield, 1024.0f, 1024.0f, kTiledHeightScale, nm, nw, nh);
    const double wholeMs = std::chrono::duration<double, std::milli>(Clock::now() - tw0).count() / reps;
    auto tr0 = Clock::now();
    for (int r = 0; r < reps; ++r)
        GenerateNormalmapRegionFromHeightfield(t->Heightfield, 1024.0f, 1024.0f, kTiledHeightScale, nm,
                                               500, 500, 548, 548); // ~48x48 dab patch
    const double regionMs = std::chrono::duration<double, std::milli>(Clock::now() - tr0).count() / reps;
    std::printf("  Fix1 normal regen (1025^2 tile): whole %.3f ms vs 48x48 region %.4f ms (%.0fx)\n\n",
                wholeMs, regionMs, wholeMs / std::max(regionMs, 1e-6));

    prof.SetEnabled(false);
}

// PLANAR GPU-BAKE DRAG TIMING (DISABLED — run with --gtest_also_run_disabled_tests). The #602
// flatten-drag before/after: a radius-100 flatten dragged across a spm2 512m-tile planar terrain,
// timing the per-drag-tick MODIFIER-SYSTEM CPU cost with the GPU eval-skip path OFF (the shipped
// per-tile CPU region bake — base-noise refill + splat regen + region normal + block min/max on
// every tile the footprint straddles) vs ON (record the straddled tiles into the batch + return;
// height/normal/splat are produced on the GPU). Not an assertion — prints min/avg/worst per-tick ms
// for both paths so the ~6-14 ms CPU tick is shown collapsing to a sub-2 ms record-and-dispatch
// tick. Effect-side, min-of-N over the sweep. GE_TERRAIN_HARNESS_WORLD picks the terrain extent
// (default 2048 m = 4x4 tiles; 4096 = 8x8, the 4096 m, 64-tile configuration).
TEST(TerrainRegionBake, DISABLED_PlanarGpuBakeDragTiming)
{
    using Clock = std::chrono::steady_clock;

    float32 worldSize = 2048.0f;
    if (const char* a = std::getenv("GE_TERRAIN_HARNESS_WORLD"))
    {
        const double v = std::strtod(a, nullptr);
        if (v >= 1024.0 && v <= 8192.0) worldSize = static_cast<float32>(v);
    }

    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = worldSize;
    cfg.WorldSizeZ = worldSize;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 2.0f; // #602 spm2 -> 512 m / 1025-res tiles
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    const uint32 tilesX = tiled->Config.TilesPerAxisX;
    const uint32 tilesZ = tiled->Config.TilesPerAxisZ;
    for (uint32 z = 0; z < tilesZ; ++z)
        for (uint32 x = 0; x < tilesX; ++x)
            svc.LoadTile(handle, TileCoord{static_cast<int32>(x), static_cast<int32>(z)})->LodState =
                TileLodState::Full;
    tiled->AtlasGpuBakeEligible = true; // whole-resident: the flag extraction sets, the drag path reads

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = worldSize;
    terrain.SizeZ = worldSize;
    terrain.HeightScale = kTiledHeightScale;
    terrain.TiledTerrainHandle = handle.Index;
    terrain.TiledTerrainGeneration = handle.Generation;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    auto mod = CreateFlattenModifier(world, 512.0f, 512.0f, 60.0f); // on a 512 m tile seam
    world.GetComponentForWrite<Components::TerrainModifierVolume>(mod)->Radius = 100.0f; // #602 footprint

    TerrainModifierSystem system;

    // Drag sweep: creep the flatten in 10 m steps along a tile seam so each tick's old-union-new
    // rect straddles 2-4 tiles (the #602 fan-out). The GPU cost is independent of the height state,
    // so running the CPU sweep first (which mutates heights) then the GPU sweep on the same terrain
    // is a fair A/B. Returns {min, avg, worst} per-tick ms.
    auto runSweep = [&](bool gpu) {
        SetGpuHeightBakeEnabledForTests(gpu ? 1 : 0);
        world.GetComponentForWrite<Components::WorldTransform>(mod)->matrix[12] = 512.0f;
        world.GetComponentForWrite<Components::WorldTransform>(mod)->matrix[14] = 512.0f;
        system.Update(world, 1.0f / 60.0f); // warm: settle onto the start position
        constexpr int kTicks = 20;
        double mn = 1e30, sum = 0.0, mx = 0.0;
        for (int d = 0; d < kTicks; ++d)
        {
            auto* xf = world.GetComponentForWrite<Components::WorldTransform>(mod);
            xf->matrix[12] = 512.0f + static_cast<float32>(d + 1) * 10.0f; // creep across the seam
            xf->matrix[14] = 512.0f + static_cast<float32>((d % 5) * 6);   // wiggle -> continuous drag
            const auto t0 = Clock::now();
            system.Update(world, 1.0f / 60.0f);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            mn = std::min(mn, ms);
            sum += ms;
            mx = std::max(mx, ms);
        }
        SetGpuHeightBakeEnabledForTests(-1);
        return std::array<double, 3>{mn, sum / kTicks, mx};
    };

    const std::array<double, 3> cpu = runSweep(false);
    const std::array<double, 3> gpu = runSweep(true);

    std::printf("\n[PlanarGpuBakeDragTiming] world=%.0fm  tiles=%ux%u  res=1025  radius=100  %s\n",
                worldSize, tilesX, tilesZ,
#ifdef NDEBUG
                "release");
#else
                "debug/debugfast");
#endif
    std::printf("  CPU path (GE_TERRAIN_GPU_BAKE off): per-tick  min %.3f  avg %.3f  worst %.3f ms\n",
                cpu[0], cpu[1], cpu[2]);
    std::printf("  GPU path (eval-skip record+return):  per-tick  min %.3f  avg %.3f  worst %.3f ms\n",
                gpu[0], gpu[1], gpu[2]);
    std::printf("  speedup (avg): %.1fx   (min-of-N CPU %.3f -> GPU %.3f ms)\n\n",
                cpu[1] / std::max(gpu[1], 1e-6), cpu[0], gpu[0]);
}

// TILED MODIFIER BAKE ATTRIBUTION (DISABLED — run with --gtest_also_run_disabled_tests).
// Reproduces the coordinator's round-5 hitch (a LARGE-radius flatten modifier nudged
// across a tiled terrain) and splits the per-nudge cost into (1) the modifier-system
// CPU bake and (2) the extraction-thread normal regen, replicating the EXACT whole-vs-
// region decision patchSlot makes (TerrainExtractionSystem.cpp) so the ~184 ms editor
// number can be attributed without a GPU. The existing SculptStrokeTiming harness uses
// a radius-25 modifier (CreateFlattenModifier hardcodes it), whose old∪new dirty rect
// is small — it does NOT surface the large-radius modifier fan-out this measures.
//   GE_TERRAIN_HARNESS_AXIS: tiles per axis (default 4). GE_TERRAIN_MOD_RADIUS: flatten
//   radius in world units (default 200). GE_TERRAIN_MOD_NUDGE: per-step move (default 50).
TEST(TerrainRegionBake, DISABLED_TiledModifierBakeAttribution)
{
    using Clock = std::chrono::steady_clock;

    uint32 axis = 4;
    if (const char* a = std::getenv("GE_TERRAIN_HARNESS_AXIS"))
    {
        const long v = std::strtol(a, nullptr, 10);
        if (v >= 1 && v <= 16) axis = static_cast<uint32>(v);
    }
    float32 radius = 200.0f;
    if (const char* r = std::getenv("GE_TERRAIN_MOD_RADIUS"))
        radius = static_cast<float32>(std::atof(r));
    float32 nudge = 50.0f;
    if (const char* n = std::getenv("GE_TERRAIN_MOD_NUDGE"))
        nudge = static_cast<float32>(std::atof(n));

    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentSquareTiledTerrain(svc, axis);
    auto* tiled = svc.GetTiledTerrainData(handle);

    ECS::World world;
    // A pool lets the modifier bake fan its per-tile row bands across workers
    // (GE_TERRAIN_MODIFIER_PARALLEL=0 forces serial for the A/B).
    JobSystem::WorkStealingThreadPool pool(std::max(2u, std::thread::hardware_concurrency()));
    world.SetJobSystem(&pool);
    RegisterModifierLifecycleEvents(world);
    CreateSquareTiledTerrainEntity(world, handle, axis);

    // Large-radius flatten centered on the terrain, nudged along X (the coordinator's burst).
    const float32 terrainWorld = static_cast<float32>(axis) * kTiledTileWorld;
    const float32 startX = terrainWorld * 0.5f - static_cast<float32>(3) * nudge; // start left-of-center
    const float32 centerZ = terrainWorld * 0.5f;
    auto modEntity = world.CreateEntity();
    {
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::Circle;
        vol.Radius = radius;
        vol.Falloff = 16.0f;
        world.AddComponentImmediate<Components::TerrainModifierVolume>(modEntity, vol);

        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = false;
        fx.TargetHeight = 60.0f;
        world.AddComponentImmediate<Components::TerrainFlattenEffect>(modEntity, fx);
        Components::WorldTransform xf{};
        xf.matrix[12] = startX;
        xf.matrix[14] = centerZ;
        world.AddComponentImmediate<Components::WorldTransform>(modEntity, xf);
    }

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // full bake (warm)

    // Seed each resident tile's persistent normal (the extraction cook does this once)
    // so the region fast-path is eligible on the first nudge, matching the editor after
    // the terrain has rendered a frame. NormalmapVersion := HeightfieldVersion (current).
    const uint32 res = Tile(tiled, 0, 0)->Heightfield.GetWidth();
    for (auto& [coord, tp] : tiled->Tiles)
    {
        if (!tp || tp->LodState != TileLodState::Full) continue;
        uint32 nw = 0, nh = 0;
        GenerateNormalmapFromHeightfield(tp->Heightfield, kTiledTileWorld, kTiledTileWorld,
                                         kTiledHeightScale, tp->Normalmap, nw, nh);
        tp->NormalmapWidth = nw;
        tp->NormalmapHeight = nh;
        tp->NormalmapVersion = tp->HeightfieldVersion;
        tp->ClearDirty();
        tp->SplatmapDirty = false;
    }

    // Replicate patchSlot's whole-vs-region decision + time the normal regen it drives.
    // Returns the extraction CPU ms and fills the path counters. Mirrors
    // TerrainExtractionSystem.cpp:1177-1199 (region gate) + 1252 (whole) / 1109 (region).
    constexpr int32 kEditRegionPad = 2;
    auto extractionCostThisFrame = [&](int32& outWholeTiles, int32& outRegionTiles,
                                       double& outNormalMs, uint64& outUploadBytes) {
        outWholeTiles = 0; outRegionTiles = 0; outNormalMs = 0.0; outUploadBytes = 0;
        const int32 last = static_cast<int32>(res) - 1;
        std::vector<uint8> nmScratch;
        for (auto& [coord, tp] : tiled->Tiles)
        {
            if (!tp || tp->LodState != TileLodState::Full) continue;
            if (!tp->HeightfieldDirty) continue;
            const bool normalSeeded = tp->Normalmap.size() ==
                                          static_cast<size_t>(res) * res * 4u &&
                                      tp->NormalmapWidth == res && tp->NormalmapHeight == res;
            const bool splatNative = tp->Splatmap.size() ==
                                         static_cast<size_t>(res) * res * 4u &&
                                     tp->SplatmapWidth == res && tp->SplatmapHeight == res;
            bool tookRegion = false;
            int32 rx0 = 0, rz0 = 0, rx1 = last, rz1 = last;
            if (normalSeeded && splatNative && tp->HeightfieldVersion > tp->NormalmapVersion)
            {
                DirtyRegionLog::Region dirty;
                if (tp->HeightfieldDirtyLog.CollectSince(tp->NormalmapVersion, dirty) &&
                    !dirty.IsEmpty())
                {
                    rx0 = std::max(dirty.MinX - kEditRegionPad, 0);
                    rz0 = std::max(dirty.MinZ - kEditRegionPad, 0);
                    rx1 = std::min(dirty.MaxX - 1 + kEditRegionPad, last);
                    rz1 = std::min(dirty.MaxZ - 1 + kEditRegionPad, last);
                    const bool wholeTile = rx0 == 0 && rz0 == 0 && rx1 == last && rz1 == last;
                    tookRegion = !wholeTile;
                }
            }
            const auto t0 = Clock::now();
            if (tookRegion)
            {
                GenerateNormalmapRegionFromHeightfield(tp->Heightfield, kTiledTileWorld,
                                                       kTiledTileWorld, kTiledHeightScale,
                                                       tp->Normalmap, rx0, rz0, rx1, rz1);
                ++outRegionTiles;
                const uint64 w = static_cast<uint64>(rx1 - rx0 + 1);
                const uint64 h = static_cast<uint64>(rz1 - rz0 + 1);
                outUploadBytes += w * h * 4u * 3u; // height + normal + splat
            }
            else
            {
                uint32 nw = 0, nh = 0;
                GenerateNormalmapFromHeightfield(tp->Heightfield, kTiledTileWorld, kTiledTileWorld,
                                                 kTiledHeightScale, tp->Normalmap, nw, nh);
                ++outWholeTiles;
                outUploadBytes += static_cast<uint64>(res) * res * 4u * 3u;
            }
            outNormalMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            tp->NormalmapVersion = tp->HeightfieldVersion;
            tp->ClearDirty();
            tp->SplatmapDirty = false;
        }
    };

    constexpr int kNudges = 8;
    double bakeMsSum = 0, bakeMsWorst = 0, extractMsSum = 0, extractMsWorst = 0;
    int wholeTilesTotal = 0, regionTilesTotal = 0;
    uint64 uploadBytesTotal = 0;
    for (int i = 0; i < kNudges; ++i)
    {
        auto* xf = world.GetComponentForWrite<Components::WorldTransform>(modEntity);
        xf->matrix[12] = startX + static_cast<float32>(i + 1) * nudge;

        const auto b0 = Clock::now();
        system.Update(world, 1.0f / 60.0f);
        const double bakeMs = std::chrono::duration<double, std::milli>(Clock::now() - b0).count();
        bakeMsSum += bakeMs; bakeMsWorst = std::max(bakeMsWorst, bakeMs);

        int32 wholeT = 0, regionT = 0; double normalMs = 0; uint64 uploadBytes = 0;
        extractionCostThisFrame(wholeT, regionT, normalMs, uploadBytes);
        extractMsSum += normalMs; extractMsWorst = std::max(extractMsWorst, normalMs);
        wholeTilesTotal += wholeT; regionTilesTotal += regionT; uploadBytesTotal += uploadBytes;
    }

    std::printf("\n[TiledModifierBakeAttribution] tiles=%ux%u res=%u radius=%.0f nudge=%.0f "
                "runtime=%s\n", axis, axis, res, radius, nudge,
#ifdef NDEBUG
                "release"
#else
                "debug/debugfast"
#endif
    );
    std::printf("  MODIFIER CPU BAKE:   avg %.3f ms  worst %.3f ms  (over %d nudges)\n",
                bakeMsSum / kNudges, bakeMsWorst, kNudges);
    std::printf("  EXTRACTION NORMAL:   avg %.3f ms  worst %.3f ms\n",
                extractMsSum / kNudges, extractMsWorst);
    std::printf("  PER-NUDGE FRAME EST: avg %.3f ms  worst %.3f ms  (bake + normal regen)\n",
                (bakeMsSum + extractMsSum) / kNudges, bakeMsWorst + extractMsWorst);
    std::printf("  extraction tiles/nudge: whole-bail %.1f  region %.1f\n",
                static_cast<double>(wholeTilesTotal) / kNudges,
                static_cast<double>(regionTilesTotal) / kNudges);
    std::printf("  upload bytes/nudge: %.2f MB\n\n",
                (static_cast<double>(uploadBytesTotal) / kNudges) / (1024.0 * 1024.0));
}

// Both the initial full bake and a subsequent region edit must match the serial path byte for
// byte. Global normalized-height rules make the splat oracle depend on the completed shared height
// range, and the parallel update runs from a worker to exercise the scheduler's nested join.
TEST(TerrainRegionBake, ParallelTiledModifierBakeMatchesSerial)
{
    constexpr uint32 kAxis = 3;
    auto bake = [&](JobSystem::WorkStealingThreadPool* pool,
                    std::vector<std::vector<float32>>& outHeights,
                    std::vector<std::vector<uint8>>& outSplats, double& fullBakeMs) {
        ScopedTerrainService scoped; // fresh service per run (sequential, not nested)
        auto& svc = TerrainService::Get();
        const auto handle = CreateResidentSquareTiledTerrain(svc, kAxis);
        auto* tiled = svc.GetTiledTerrainData(handle);

        ECS::World world;
        if (pool)
            world.SetJobSystem(pool);
        RegisterModifierLifecycleEvents(world);
        CreateSquareTiledTerrainEntity(world, handle, kAxis);
        CreateGlobalRulesModifier(world);

        const float32 terrainWorld = static_cast<float32>(kAxis) * kTiledTileWorld;
        auto e = world.CreateEntity();
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::Circle;
        vol.Radius = 200.0f; // wide region -> exceeds the parallel-dispatch threshold
        vol.Falloff = 16.0f;
        world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);

        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = false;
        fx.TargetHeight = 80.0f;
        world.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
        Components::WorldTransform xf{};
        xf.matrix[12] = terrainWorld * 0.5f;
        xf.matrix[14] = terrainWorld * 0.5f;
        world.AddComponentImmediate<Components::WorldTransform>(e, xf);

        TerrainModifierSystem system;
        const auto fullStart = std::chrono::steady_clock::now();
        system.Update(world, 1.0f / 60.0f);
        fullBakeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - fullStart).count();

        std::vector<TileCoord> coords;
        for (auto& [c, tp] : tiled->Tiles)
            coords.push_back(c);
        std::sort(coords.begin(), coords.end());
        const auto snapshot = [&]() {
            for (const auto& c : coords)
            {
                auto* tp = tiled->Tiles.at(c).get();
                outHeights.emplace_back(
                    tp->Heightfield.GetRawSamples(),
                    tp->Heightfield.GetRawSamples() + tp->Heightfield.GetSampleCount());
                outSplats.push_back(tp->Splatmap);
            }
        };
        snapshot(); // initial full bake must already be correct before any region work can repair it
        auto* w = world.GetComponentForWrite<Components::WorldTransform>(e);
        w->matrix[12] += 60.0f;
        system.Update(world, 1.0f / 60.0f);
        snapshot();
    };

    std::vector<std::vector<float32>> hSerial, hParallel;
    std::vector<std::vector<uint8>> sSerial, sParallel;
    double serialFullMs = 0.0, parallelFullMs = 0.0;
    bake(nullptr, hSerial, sSerial, serialFullMs);
    {
        JobSystem::WorkStealingThreadPool pool(4);
        JobSystem::JobCounter counter;
        std::promise<void> started;
        auto workerStarted = started.get_future();
        pool.Run([&]() {
            started.set_value();
            bake(&pool, hParallel, sParallel, parallelFullMs);
        }, counter);
        // Do not let the main thread's helping Wait execute the outer update itself.
        workerStarted.wait();
        pool.Wait(counter);
    }
    std::printf("[FullModifierBake] tiles=%u serial=%.3fms parallel=%.3fms workers=4\n",
                kAxis * kAxis, serialFullMs, parallelFullMs);

    ASSERT_EQ(hSerial.size(), hParallel.size());
    ASSERT_EQ(hSerial.size(), 2u * kAxis * kAxis);
    std::size_t weighted = 0;
    for (size_t i = 0; i < hSerial.size(); ++i)
    {
        ASSERT_EQ(hSerial[i].size(), hParallel[i].size());
        EXPECT_EQ(0, std::memcmp(hSerial[i].data(), hParallel[i].data(),
                                 hSerial[i].size() * sizeof(float32)))
            << "tile index " << i << " heightfield differs (parallel vs serial)";
        ASSERT_EQ(sSerial[i].size(), sParallel[i].size());
        EXPECT_EQ(0, std::memcmp(sSerial[i].data(), sParallel[i].data(), sSerial[i].size()))
            << "tile index " << i << " splatmap differs (parallel vs serial)";
        weighted += WeightedTexelCount(sSerial[i]);
    }
    EXPECT_GT(weighted, 0u) << "global surface rules must produce nontrivial splat weights";
}

// A single (untiled) terrain's bakes run their height and splat passes in row bands
// across the pool once the rect is large; the bands must reproduce the serial bake
// byte for byte, for the first full bake and for a later region edit, over the
// modifier kinds a scene combines (flatten, noise, paint, global rules, a spline path).
TEST(TerrainRegionBake, ParallelSingleTerrainModifierBakeMatchesSerial)
{
    constexpr uint32 kDim = 513; // 263,169 samples: above the parallel-dispatch threshold
    auto bake = [&](JobSystem::WorkStealingThreadPool* pool, std::vector<std::vector<float32>>& outHeights,
                    std::vector<std::vector<uint8>>& outSplats) {
        ScopedTerrainService scoped; // fresh services per run (sequential, not nested)
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
        SplineECS::SplineService::Initialize();
        auto& svc = TerrainService::Get();
        auto& splineSvc = SplineECS::SplineService::Get();
        const TerrainHandle handle =
            CreateBakedTerrainWithBase(svc, Components::TerrainBaseSource::ProceduralNoise, GUID{}, kDim);
        auto* data = svc.GetTerrainData(handle);

        ECS::World world;
        if (pool)
            world.SetJobSystem(pool);
        RegisterModifierLifecycleEvents(world);
        CreateTerrainEntity(world, handle);
        CreateGlobalRulesModifier(world);
        const auto flatten = CreateFlattenModifier(world, 30.0f, 40.0f, 20.0f);
        CreateNoiseModifier(world, -50.0f, 10.0f);
        {
            auto e = world.CreateEntity();
            Components::TerrainModifierVolume paintVol{};
            paintVol.Shape = Components::TerrainVolumeShape::Circle;
            paintVol.Radius = 20.0f;
            paintVol.Falloff = 5.0f;
            world.AddComponentImmediate<Components::TerrainModifierVolume>(e, paintVol);
            Components::TerrainPaintLayerEffect paint{};
            paint.LayerIndex = 1;
            paint.Strength = 0.8f;
            world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(e, paint);
            Components::WorldTransform xf{};
            xf.matrix[12] = -30.0f;
            xf.matrix[14] = -60.0f;
            world.AddComponentImmediate<Components::WorldTransform>(e, xf);
        }
        const SplineECS::SplineHandle splineHandle =
            splineSvc.CreateSpline(Spline::SplineType::CatmullRom, /*closed*/ false);
        auto* splineData = splineSvc.GetSplineData(splineHandle);
        for (int i = 0; i < 4; ++i)
            splineData->AddPoint(Mathematics::Vector3(-100.0f + 60.0f * static_cast<float32>(i), 0.0f,
                                                      80.0f - 20.0f * static_cast<float32>(i)),
                                 12.0f);
        splineSvc.RebuildCache(splineHandle);
        const auto path = world.CreateEntity();
        Components::SplineComponent splineComp{};
        splineComp.SplineDataIndex = splineHandle.Index();
        splineComp.SplineDataGeneration = splineHandle.Generation();
        world.AddComponentImmediate<Components::SplineComponent>(path, splineComp);
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::SplinePath;
        vol.Falloff = 6.0f;
        world.AddComponentImmediate<Components::TerrainModifierVolume>(path, vol);
        Components::TerrainFlattenEffect pathFlatten{};
        pathFlatten.UseVolumeHeight = false;
        pathFlatten.TargetHeight = 12.0f;
        world.AddComponentImmediate<Components::TerrainFlattenEffect>(path, pathFlatten);
        world.AddComponentImmediate<Components::WorldTransform>(path, Components::WorldTransform{});

        TerrainModifierSystem system;
        const auto snapshot = [&]() {
            outHeights.emplace_back(data->Heightfield.GetRawSamples(),
                                    data->Heightfield.GetRawSamples() + data->Heightfield.GetSampleCount());
            outSplats.push_back(data->Splatmap);
        };
        system.Update(world, 1.0f / 60.0f); // the first bake of an open: full
        // 513 rows in 32-row bands: a pool must split the pass, never run it whole.
        EXPECT_EQ(system.GetLastBakeBandCountForTests(), pool ? 17u : 0u);
        snapshot();
        // A wide region edit: the flatten grows past the threshold and moves.
        world.GetComponentForWrite<Components::TerrainModifierVolume>(flatten)->Radius = 110.0f;
        world.GetComponentForWrite<Components::WorldTransform>(flatten)->matrix[12] = 10.0f;
        system.Update(world, 1.0f / 60.0f);
        snapshot();
        SplineECS::SplineService::Shutdown();
    };

    std::vector<std::vector<float32>> hSerial, hParallel;
    std::vector<std::vector<uint8>> sSerial, sParallel;
    bake(nullptr, hSerial, sSerial);
    {
        JobSystem::WorkStealingThreadPool pool(4);
        JobSystem::JobCounter counter;
        std::promise<void> started;
        auto workerStarted = started.get_future();
        pool.Run([&]() {
            started.set_value();
            bake(&pool, hParallel, sParallel);
        }, counter);
        // The bake runs on a worker, as the scheduler runs it; the main thread only joins.
        workerStarted.wait();
        pool.Wait(counter);
    }

    ASSERT_EQ(hSerial.size(), 2u);
    ASSERT_EQ(hParallel.size(), 2u);
    for (size_t i = 0; i < hSerial.size(); ++i)
    {
        ASSERT_EQ(hSerial[i].size(), hParallel[i].size());
        EXPECT_EQ(0, std::memcmp(hSerial[i].data(), hParallel[i].data(), hSerial[i].size() * sizeof(float32)))
            << "bake " << i << ": heightfield differs (parallel vs serial)";
        ASSERT_EQ(sSerial[i].size(), sParallel[i].size());
        EXPECT_EQ(0, std::memcmp(sSerial[i].data(), sParallel[i].data(), sSerial[i].size()))
            << "bake " << i << ": splatmap differs (parallel vs serial)";
        EXPECT_GT(WeightedTexelCount(sSerial[i]), 0u) << "the rules and the paint must weight the splat";
    }
}

// The min-texel gate both callers share: below kParallelBakeMinTexels the rectangle runs whole on
// the calling thread (a small grass edit never fans out); above it, it is cut into row bands.
TEST(TerrainRowBands, ASmallRectangleRunsWholeAndALargeOneIsSplitIntoBands)
{
    JobSystem::WorkStealingThreadPool pool(4);
    std::atomic<uint32> calls{0};
    std::atomic<uint32> rows{0};

    const BakeBandRegion small{0, 0, 32, 32};
    std::vector<std::array<int32, 4>> whole;
    const uint32 smallBands =
        RunRowBands(&pool, {&small, 1}, [&whole](std::size_t, int32 minX, int32 maxX, int32 z0, int32 z1) {
            whole.push_back({minX, maxX, z0, z1});
        });
    EXPECT_EQ(smallBands, 0u);
    ASSERT_EQ(whole.size(), 1u);
    EXPECT_EQ(whole[0], (std::array<int32, 4>{0, 32, 0, 32}));

    const BakeBandRegion large{0, 0, 320, 320};
    const uint32 largeBands =
        RunRowBands(&pool, {&large, 1}, [&calls, &rows](std::size_t, int32, int32, int32 z0, int32 z1) {
            calls.fetch_add(1);
            rows.fetch_add(static_cast<uint32>(z1 - z0 + 1));
        });
    EXPECT_EQ(largeBands, static_cast<uint32>((321 + kBakeBandRows - 1) / kBakeBandRows));
    EXPECT_EQ(calls.load(), static_cast<uint32>((321 + kBakeBandRows - 1) / kBakeBandRows));
    EXPECT_EQ(rows.load(), 321u);
}

// INCREMENTAL MIN/MAX BYTE-IDENTITY: after each region edit, the per-tile
// CachedMin/MaxH the incremental block grid produces must equal an exhaustive
// full-tile min/max scan — the invariant that lets the O(region) refresh replace
// the O(tile-samples) rescan without drifting the splat normalization range. The
// stroke raises (grows the max) then lowers past the noise band (shrinks a tile's
// max — the case a naive union would miss), across tiles, so both directions and
// the multi-tile aggregate are exercised. A drifted range would also surface as a
// splat mismatch in TiledRegionBakeMatchesFullBake; this pins the range directly.
TEST(TerrainRegionBake, IncrementalTileRangeMatchesFullScan)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    constexpr uint32 kAxis = 3;
    const auto handle = CreateResidentSquareTiledTerrain(svc, kAxis);
    auto* tiled = svc.GetTiledTerrainData(handle);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateSquareTiledTerrainEntity(world, handle, kAxis);
    auto mod = CreateFlattenModifier(world, 200.0f, 200.0f, 400.0f);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // full bake

    auto verifyAllTiles = [&](const char* stage) {
        for (auto& [coord, tilePtr] : tiled->Tiles)
        {
            if (!tilePtr || tilePtr->LodState != TileLodState::Full)
                continue;
            const auto& hf = tilePtr->Heightfield;
            const std::size_t count = hf.GetSampleCount();
            if (count == 0)
                continue;
            const float32* s = hf.GetRawSamples();
            float32 lo = s[0], hi = s[0];
            for (std::size_t i = 1; i < count; ++i)
            {
                lo = std::min(lo, s[i]);
                hi = std::max(hi, s[i]);
            }
            EXPECT_FLOAT_EQ(tilePtr->CachedMinH, lo)
                << stage << " tile (" << coord.X << "," << coord.Z << ") min drifted";
            EXPECT_FLOAT_EQ(tilePtr->CachedMaxH, hi)
                << stage << " tile (" << coord.X << "," << coord.Z << ") max drifted";
        }
    };
    verifyAllTiles("full-bake");

    // Sweep the flatten across tiles, first raising the target (grows the max), then
    // dropping it well below the noise band (a lowering dab that removes a tile's
    // current max — the region patch must re-aggregate the surviving blocks exactly).
    const float32 targets[] = {450.0f, 900.0f, -300.0f, 120.0f, -50.0f};
    float32 px = 200.0f;
    for (float32 target : targets)
    {
        px += 140.0f;
        auto* xf = world.GetComponentForWrite<Components::WorldTransform>(mod);
        xf->matrix[12] = px;
        xf->matrix[14] = 260.0f;
        auto* fm = world.GetComponentForWrite<Components::TerrainFlattenEffect>(mod);
        fm->TargetHeight = target;
        system.Update(world, 1.0f / 60.0f);
        verifyAllTiles("region-edit");
    }
}

// REGION-PROPORTIONALITY: an edit inside one tile re-bakes exactly that tile.
// Pre-fix (full re-bake of every resident tile) bumps EVERY tile's version, so
// the untouched-tile assertion fails on old code.
TEST(TerrainRegionBake, TiledRegionEditTouchesOnlyIntersectedTile)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const auto handle = CreateResidentTiledTerrain(svc, /*tilesX*/ 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    ASSERT_EQ(tiled->Config.TilesPerAxisX, 2u);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    // Tile (0,0) spans world x in [0,1024]; place the modifier well inside it.
    auto mod = CreateFlattenModifier(world, 300.0f, 400.0f, 20.0f);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // full bake (no baseline yet)

    const uint64 v00 = Tile(tiled, 0, 0)->HeightfieldVersion;
    const uint64 v10 = Tile(tiled, 1, 0)->HeightfieldVersion;
    ASSERT_GT(v00, 0u);
    ASSERT_GT(v10, 0u);

    // Move the modifier, staying inside tile (0,0): a region bake.
    auto* xf = world.GetComponentForWrite<Components::WorldTransform>(mod);
    ASSERT_NE(xf, nullptr);
    xf->matrix[12] = 340.0f;
    xf->matrix[14] = 360.0f;
    system.Update(world, 1.0f / 60.0f);

    EXPECT_GT(Tile(tiled, 0, 0)->HeightfieldVersion, v00) << "edited tile must re-bake";
    EXPECT_EQ(Tile(tiled, 1, 0)->HeightfieldVersion, v10) << "untouched tile must NOT re-bake";
}

// RESIDENCY-INDEPENDENCE of baked heights. A tile's height content is a pure
// function of (world position, modifier stack): the tiled base is world-space
// noise addressed by world XZ, and ApplyHeightModifiers evaluates every effect at
// terrainOrigin + index * spacing. Neither reads a neighbour. So the same tile
// must bake bit-identically whether the rest of the terrain is resident or not.
//
// This is the property any camera-independent ground source rests on: it says the
// heights a far-away tile WOULD hold can be computed without streaming it in.
TEST(TerrainRegionBake, TileHeightsDoNotDependOnNeighbourResidency)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    // The modifier straddles the tile boundary at x = 1024 so tile (1,0) carries
    // both base noise and modifier-applied samples — a tile that agreed only on
    // untouched noise would prove nothing about the modifier stack.
    constexpr float32 kModX = kTiledTileWorld;
    constexpr float32 kModZ = 500.0f;

    // A: both tiles resident.
    const auto handleA = CreateResidentTiledTerrain(svc, 2);
    auto* tiledA = svc.GetTiledTerrainData(handleA);
    ASSERT_NE(tiledA, nullptr);
    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTiledTerrainEntity(worldA, handleA, 2);
    CreateFlattenModifier(worldA, kModX, kModZ, 20.0f);
    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f);

    // B: same terrain and same modifier, but ONLY tile (1,0) ever streamed in —
    // the state a camera parked far from tile (0,0) leaves behind.
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 2.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const TiledTerrainHandle handleB = svc.CreateTiledTerrain(cfg);
    auto* tiledB = svc.GetTiledTerrainData(handleB);
    ASSERT_NE(tiledB, nullptr);
    svc.LoadTile(handleB, TileCoord{1, 0})->LodState = TileLodState::Full;
    ASSERT_EQ(tiledB->Tiles.size(), 1u) << "tile (0,0) must be absent, not merely Empty";

    ECS::World worldB;
    RegisterModifierLifecycleEvents(worldB);
    CreateTiledTerrainEntity(worldB, handleB, 2);
    CreateFlattenModifier(worldB, kModX, kModZ, 20.0f);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    auto* ta = Tile(tiledA, 1, 0);
    auto* tb = Tile(tiledB, 1, 0);
    ASSERT_EQ(ta->Heightfield.GetSampleCount(), tb->Heightfield.GetSampleCount());
    ASSERT_GT(ta->Heightfield.GetSampleCount(), 0u);
    EXPECT_EQ(0, std::memcmp(ta->Heightfield.GetRawSamples(), tb->Heightfield.GetRawSamples(),
                             ta->Heightfield.GetSampleCount() * sizeof(float32)))
        << "tile (1,0) baked differently with its neighbour absent";

    // Positive control: the modifier really did reach this tile, so the memcmp
    // above compared modifier-applied samples and not just untouched base noise.
    const auto* base = ta->Heightfield.GetRawSamples();
    const float32 flattenNormalized = 20.0f / kTiledHeightScale;
    bool anyFlattened = false;
    for (uint32 i = 0; i < ta->Heightfield.GetSampleCount() && !anyFlattened; ++i)
        anyFlattened = std::fabs(base[i] - flattenNormalized) < 1.0e-4f;
    EXPECT_TRUE(anyFlattened) << "the straddling modifier never reached tile (1,0)";
}

// THE COMPOSED-GROUND ORACLE. ComposeTiledGroundBlock answers ground queries for
// tiles the camera never streamed in, so what it returns has to be what a resident
// tile holds — otherwise water would be built against a bed the terrain does not
// render. Composed block vs. the baked tile store, across a tile seam, bit for bit.
TEST(TerrainRegionBake, ComposedGroundBlockMatchesTheBakedTiles)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const auto handle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    // On the seam, so the block reads modifier-applied samples from BOTH tiles.
    CreateFlattenModifier(world, kTiledTileWorld, 300.0f, 20.0f);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const int64 interior = static_cast<int64>(tiled->Config.TileConfig.HeightmapWidth) - 1;
    ASSERT_GT(interior, 0);

    // A block straddling the seam at global index `interior`.
    const int64 firstX = interior - 40;
    const int64 firstZ = 260;
    constexpr uint32 kCountX = 96;
    constexpr uint32 kCountZ = 24;

    std::vector<float32> composed;
    ASSERT_TRUE(system.ComposeTiledGroundBlock(*tiled, kTiledHeightScale, /*terrainOriginY*/ 0.0f,
                                               firstX, firstZ, kCountX, kCountZ, composed));
    ASSERT_EQ(composed.size(), static_cast<std::size_t>(kCountX) * kCountZ);

    uint32 compared = 0;
    for (uint32 j = 0; j < kCountZ; ++j)
        for (uint32 i = 0; i < kCountX; ++i)
        {
            const int64 gx = firstX + i;
            const int64 gz = firstZ + j;
            const int32 tx = static_cast<int32>(gx / interior);
            const int32 ix = static_cast<int32>(gx % interior);
            const float32 baked = Tile(tiled, tx, static_cast<int32>(gz / interior))
                                      ->Heightfield.GetSample(static_cast<uint32>(ix),
                                                              static_cast<uint32>(gz % interior));
            ASSERT_EQ(composed[static_cast<std::size_t>(j) * kCountX + i], baked)
                << "composed ground differs from the baked tile at global (" << gx << "," << gz
                << ") — tile " << tx << " sample " << ix;
            ++compared;
        }
    EXPECT_EQ(compared, kCountX * kCountZ);

    // Positive control: the block really did span the seam and really did carry
    // flattened samples, so the comparison above was not over untouched noise.
    ASSERT_LT(firstX, interior);
    ASSERT_GT(firstX + static_cast<int64>(kCountX) - 1, interior);
    const float32 flattenNormalized = 20.0f / kTiledHeightScale;
    bool anyFlattened = false;
    for (float32 v : composed)
        if (std::fabs(v - flattenNormalized) < 1.0e-4f) { anyFlattened = true; break; }
    EXPECT_TRUE(anyFlattened) << "the seam modifier never reached the composed block";
}

// The block must compose the same heights with NOTHING resident — that is the whole
// point of it. Same terrain, same modifier, zero loaded tiles.
TEST(TerrainRegionBake, ComposedGroundBlockNeedsNoResidentTile)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const auto residentHandle = CreateResidentTiledTerrain(svc, 2);
    auto* resident = svc.GetTiledTerrainData(residentHandle);
    ASSERT_NE(resident, nullptr);
    ECS::World residentWorld;
    RegisterModifierLifecycleEvents(residentWorld);
    CreateTiledTerrainEntity(residentWorld, residentHandle, 2);
    CreateFlattenModifier(residentWorld, kTiledTileWorld, 300.0f, 20.0f);
    TerrainModifierSystem residentSystem;
    residentSystem.Update(residentWorld, 1.0f / 60.0f);

    // The same terrain with no tile ever loaded.
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 2.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const TiledTerrainHandle emptyHandle = svc.CreateTiledTerrain(cfg);
    auto* empty = svc.GetTiledTerrainData(emptyHandle);
    ASSERT_NE(empty, nullptr);
    ASSERT_TRUE(empty->Tiles.empty()) << "this arm must compose with zero residency";

    ECS::World emptyWorld;
    RegisterModifierLifecycleEvents(emptyWorld);
    CreateTiledTerrainEntity(emptyWorld, emptyHandle, 2);
    CreateFlattenModifier(emptyWorld, kTiledTileWorld, 300.0f, 20.0f);
    TerrainModifierSystem emptySystem;
    emptySystem.Update(emptyWorld, 1.0f / 60.0f);

    const int64 interior = static_cast<int64>(cfg.TileConfig.HeightmapWidth) - 1;
    const int64 firstX = interior - 40;
    constexpr uint32 kCountX = 96;
    constexpr uint32 kCountZ = 24;

    std::vector<float32> fromResident;
    std::vector<float32> fromEmpty;
    ASSERT_TRUE(residentSystem.ComposeTiledGroundBlock(*resident, kTiledHeightScale, 0.0f, firstX,
                                                       260, kCountX, kCountZ, fromResident));
    ASSERT_TRUE(emptySystem.ComposeTiledGroundBlock(*empty, kTiledHeightScale, 0.0f, firstX, 260,
                                                    kCountX, kCountZ, fromEmpty));
    ASSERT_EQ(fromResident.size(), fromEmpty.size());
    EXPECT_EQ(0, std::memcmp(fromResident.data(), fromEmpty.data(),
                             fromResident.size() * sizeof(float32)))
        << "composing over an unstreamed terrain produced different ground";
}

// THE BAKE-VS-PAGES ORACLE (Q1 (a)). A modified tiled terrain pages: its height pages are its base
// pages plus the modifier overlay, so at level 0 they must be the baked tiles the unpaged path
// renders. Two flatten modifiers, one on the tile seam; every level-0 page against the tiles.
TEST(TerrainRegionBake, TwoModifiersReachTheHeightPagesAsTheyReachTheBakedTiles)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    CreateFlattenModifier(world, kTiledTileWorld, 300.0f, 20.0f);
    CreateFlattenModifier(world, 0.5f * kTiledTileWorld, 120.0f, 35.0f);
    tiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap; // the driver's: the terrain would page
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_TRUE(tiled->PageOverlayCurrent);
    ASSERT_NE(tiled->PageOverlay, nullptr) << "two height modifiers reach the terrain";

    // The driver's page source for a noise base: the generated provider over the tile lattice.
    PageStreaming::GeneratedHeightLattice lattice;
    lattice.Source = PageStreaming::GeneratedHeightSource::Noise;
    lattice.TerrainOriginX = tiled->WorldOriginX;
    lattice.TerrainOriginZ = tiled->WorldOriginZ;
    lattice.TileWorldSize = tiled->Config.TileWorldSize;
    lattice.TileSamples = tiled->Config.TileConfig.HeightmapWidth;
    lattice.TilesX = tiled->Config.TilesPerAxisX;
    lattice.TilesZ = tiled->Config.TilesPerAxisZ;
    lattice.Noise = {kTileNoiseFrequency, kTileNoiseAmplitude, kTileNoiseOctaves, kTileNoiseSeed};
    const PageStreaming::GeneratedHeightPageProvider provider(lattice);
    ASSERT_FALSE(provider.Levels().empty());
    const PageStreaming::PageStoreLevel& level0 = provider.Levels().front();

    const uint32 interior = tiled->Config.TileConfig.HeightmapWidth - 1u;
    std::vector<float32> page(PageStreaming::kPageSampleCount);
    float32 worst = 0.0f;
    uint32 modifiedSamples = 0;
    for (uint32 pz = 0; pz < level0.PagesZ; ++pz)
        for (uint32 px = 0; px < level0.PagesX; ++px)
        {
            const PageStreaming::PageAddress address{0, 0, px, pz};
            ASSERT_TRUE(provider.FillPage(address, page));
            tiled->PageOverlay->ApplyToPage(address, page);
            for (uint32 j = 0; j < PageStreaming::kPageOwnedSamples; ++j)
                for (uint32 i = 0; i < PageStreaming::kPageOwnedSamples; ++i)
                {
                    const uint32 gx = px * PageStreaming::kPageOwnedSamples + i;
                    const uint32 gz = pz * PageStreaming::kPageOwnedSamples + j;
                    if (gx >= level0.SamplesX || gz >= level0.SamplesZ)
                        continue;
                    const uint32 tx = std::min(gx / interior, tiled->Config.TilesPerAxisX - 1u);
                    const uint32 tz = std::min(gz / interior, tiled->Config.TilesPerAxisZ - 1u);
                    const float32 baked = Tile(tiled, static_cast<int32>(tx), static_cast<int32>(tz))
                                              ->Heightfield.GetSample(gx - tx * interior, gz - tz * interior);
                    const float32 paged = page[(j + PageStreaming::kPageApronSamples) * PageStreaming::kPageStrideSamples +
                                               i + PageStreaming::kPageApronSamples];
                    worst = std::max(worst, std::fabs(paged - baked));
                    if (std::fabs(baked - 20.0f / kTiledHeightScale) < 1.0e-4f ||
                        std::fabs(baked - 35.0f / kTiledHeightScale) < 1.0e-4f)
                        ++modifiedSamples;
                }
        }
    // The generated level 0 is the tile fill bit for bit; the overlay adds baked minus base in float.
    EXPECT_LE(worst, 1.0e-6f) << "a level-0 page differs from the baked tile under it";
    EXPECT_GT(modifiedSamples, 100u) << "the comparison never reached a flattened sample";
}

// The noise lattice of a tiled terrain made by CreateResidentTiledTerrain: the driver's page
// source for a noise base, whose level 0 is the tiles' samples bit for bit.
PageStreaming::GeneratedHeightLattice TileNoiseLattice(const TiledTerrainData& tiled)
{
    PageStreaming::GeneratedHeightLattice lattice;
    lattice.Source = PageStreaming::GeneratedHeightSource::Noise;
    lattice.TerrainOriginX = tiled.WorldOriginX;
    lattice.TerrainOriginZ = tiled.WorldOriginZ;
    lattice.TileWorldSize = tiled.Config.TileWorldSize;
    lattice.TileSamples = tiled.Config.TileConfig.HeightmapWidth;
    lattice.TilesX = tiled.Config.TilesPerAxisX;
    lattice.TilesZ = tiled.Config.TilesPerAxisZ;
    lattice.Noise = {kTileNoiseFrequency, kTileNoiseAmplitude, kTileNoiseOctaves, kTileNoiseSeed};
    return lattice;
}

std::vector<PageStreaming::PageStoreLevel> TileLatticeLevels(const TiledTerrainData& tiled)
{
    const PageStreaming::PageStoreHeader header =
        MakeSurfaceStoreHeader(tiled.Config, PageStreaming::PageFieldKind::Splat, 0u);
    return PageStreaming::BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
}

// The tile bake's splat at level-0 sample (gx, gz), from the tile that owns the sample.
const uint8* TileSplatTexel(TiledTerrainData& tiled, uint32 gx, uint32 gz)
{
    const uint32 interior = tiled.Config.TileConfig.HeightmapWidth - 1u;
    const uint32 tx = std::min(gx / interior, tiled.Config.TilesPerAxisX - 1u);
    const uint32 tz = std::min(gz / interior, tiled.Config.TilesPerAxisZ - 1u);
    const TerrainTileData* tile = Tile(&tiled, static_cast<int32>(tx), static_cast<int32>(tz));
    return &tile->Splatmap[(static_cast<std::size_t>(gz - tz * interior) * tile->SplatmapWidth + (gx - tx * interior)) *
                           4u];
}

// Counts of a comparison of surface pages against the tile bake.
struct SplatPageComparison
{
    std::size_t Compared = 0;
    std::size_t Weighted = 0;
};

// Expects level-0 surface page (px, pz) to hold the tile bake's splat at every stride texel inside
// the terrain. A tile's first row and column inside the terrain are skipped: the tile bake clamps
// its slope there (#5027), and the page reads across the seam.
void ExpectSplatPageMatchesTiles(TiledTerrainData& tiled, uint32 px, uint32 pz, std::span<const uint8> page,
                                 SplatPageComparison& comparison)
{
    const uint32 interior = tiled.Config.TileConfig.HeightmapWidth - 1u;
    const PageStreaming::PageStoreLevel level0 = TileLatticeLevels(tiled).front();
    constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
    ASSERT_EQ(page.size(), static_cast<std::size_t>(PageStreaming::kPageSampleCount) * 4u);
    for (uint32 j = 0; j < kStride; ++j)
        for (uint32 i = 0; i < kStride; ++i)
        {
            const int64 sx = static_cast<int64>(px) * PageStreaming::kPageOwnedSamples + i - 1;
            const int64 sz = static_cast<int64>(pz) * PageStreaming::kPageOwnedSamples + j - 1;
            if (sx < 0 || sz < 0 || sx >= level0.SamplesX || sz >= level0.SamplesZ)
                continue;
            const uint32 gx = static_cast<uint32>(sx);
            const uint32 gz = static_cast<uint32>(sz);
            if ((gx > 0u && gx % interior == 0u) || (gz > 0u && gz % interior == 0u))
                continue;
            const uint8* paged = &page[(static_cast<std::size_t>(j) * kStride + i) * 4u];
            ASSERT_TRUE(std::equal(paged, paged + 4, TileSplatTexel(tiled, gx, gz)))
                << "page (" << px << ", " << pz << ") stride texel (" << i << ", " << j << ")";
            ++comparison.Compared;
            if (paged[1] != 0u)
                ++comparison.Weighted;
        }
}

// A level-0 surface page's splat is the tile bake's splat at the same samples, bit for bit, apron
// included, but for the tile seam ExpectSplatPageMatchesTiles skips.
TEST(TerrainRegionBake, ALevel0SurfacePageHoldsTheTileBakesSplatBitForBit)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    CreateGlobalRulesModifier(world);
    CreateFlattenModifier(world, kTiledTileWorld, 300.0f, 20.0f);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_TRUE(tiled->SplatBakeRangeValid);
    const std::shared_ptr<const TerrainSurfaceBakeInputs> inputs = system.SnapshotSurfaceBake(*tiled, {});

    const uint32 interior = tiled->Config.TileConfig.HeightmapWidth - 1u;
    const uint32 tilesX = tiled->Config.TilesPerAxisX;
    const uint32 tilesZ = tiled->Config.TilesPerAxisZ;
    const auto ownerTile = [&](uint32 gx, uint32 gz) {
        return Tile(tiled, static_cast<int32>(std::min(gx / interior, tilesX - 1u)),
                    static_cast<int32>(std::min(gz / interior, tilesZ - 1u)));
    };
    const auto tileLocal = [&](uint32 g, uint32 tiles) { return g - std::min(g / interior, tiles - 1u) * interior; };

    const PageStreaming::PageStoreLevel level0 = TileLatticeLevels(*tiled).front();
    SplatPageComparison comparison;
    std::vector<uint8> page;
    for (uint32 pz = 0; pz < level0.PagesZ; ++pz)
        for (uint32 px = 0; px < level0.PagesX; ++px)
        {
            SurfaceHeightWindow window;
            window.Rect = SurfaceHeightRect(level0, px, pz, PageStreaming::PageFieldFormat::Rgba8);
            for (uint32 gz = window.Rect.MinZ; gz <= window.Rect.MaxZ; ++gz)
                for (uint32 gx = window.Rect.MinX; gx <= window.Rect.MaxX; ++gx)
                    window.Heights.push_back(ownerTile(gx, gz)->Heightfield.GetSample(tileLocal(gx, tilesX),
                                                                                       tileLocal(gz, tilesZ)));
            ASSERT_TRUE(TerrainModifierSystem::BakeSurfaceSplatPage(*inputs, PageStreaming::PageAddress{0, 0, px, pz},
                                                                     level0, window, page));
            ExpectSplatPageMatchesTiles(*tiled, px, pz, page, comparison);
            if (HasFatalFailure())
                return;
        }
    EXPECT_GT(comparison.Compared, 2000000u);
    EXPECT_GT(comparison.Weighted, 1000u) << "the rules never reached a compared texel";
}

// A store access on the lattice of `tiled` for its `plane` store.
std::shared_ptr<PageStreaming::PageStoreAccess> SurfaceAccess(const TiledTerrainData& tiled,
                                                              PageStreaming::PageFieldKind plane)
{
    return std::make_shared<PageStreaming::PageStoreAccess>(plane, MakeSurfaceStoreHeader(tiled.Config, plane, 0u));
}

// The height pages of a generated lattice, with the terrain's modifier overlay when it has one. A
// page equal to `*refused` is reported as not on hand.
SurfaceHeightPageFetch GeneratedHeightPages(const PageStreaming::GeneratedHeightPageProvider& provider,
                                            const TiledTerrainData& tiled,
                                            const std::optional<PageStreaming::PageAddress>* refused = nullptr)
{
    return [&provider, &tiled, refused](const PageStreaming::PageAddress& address, std::span<float32> samples) {
        if ((refused && *refused == address) || !provider.FillPage(address, samples))
            return false;
        if (tiled.PageOverlay)
            tiled.PageOverlay->ApplyToPage(address, samples);
        return true;
    };
}

// The level-0 pages whose first owned sample lies in tile (tx, tz): one tile's batch.
std::vector<PageStreaming::PageAddress> TilePageBatch(const TiledTerrainData& tiled, uint32 tx, uint32 tz)
{
    const uint32 interior = tiled.Config.TileConfig.HeightmapWidth - 1u;
    const PageStreaming::PageStoreLevel level0 = TileLatticeLevels(tiled).front();
    std::vector<PageStreaming::PageAddress> pages;
    for (uint32 pz = 0; pz < level0.PagesZ; ++pz)
        for (uint32 px = 0; px < level0.PagesX; ++px)
            if (std::min(px * PageStreaming::kPageOwnedSamples / interior, tiled.Config.TilesPerAxisX - 1u) == tx &&
                std::min(pz * PageStreaming::kPageOwnedSamples / interior, tiled.Config.TilesPerAxisZ - 1u) == tz)
                pages.push_back(PageStreaming::PageAddress{0, 0, px, pz});
    return pages;
}

std::vector<uint8> StoreSplatPage(const PageStreaming::PageStoreReader& reader, const PageStreaming::PageAddress& address)
{
    std::vector<uint8> samples(static_cast<std::size_t>(PageStreaming::kPageSampleCount) * 4u);
    EXPECT_TRUE(reader.ReadPageSamples(address, samples))
        << "level " << int(address.Level) << " page (" << address.X << ", " << address.Z << ")";
    return samples;
}

// A scratch folder for one test's stores, removed after it.
struct ScopedStoreFolder
{
    std::filesystem::path Path;
    explicit ScopedStoreFolder(const char* name)
        : Path(std::filesystem::temp_directory_path() / ("ge-surface-pages-" + std::string(name)))
    {
        std::filesystem::remove_all(Path);
        std::filesystem::create_directories(Path);
    }
    // After a failed assert, a job can still hold a store file open, so the removal ignores errors.
    ~ScopedStoreFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(Path, error);
    }
};

// A tiled terrain with the global surface rules baked, for the splat store tests.
struct SplatStoreTerrain
{
    ScopedTerrainService Scoped;
    TiledTerrainData* Tiled = nullptr;
    ECS::EntityHandle Entity;
    ECS::World World;
    TerrainModifierSystem System;
    ECS::EntityHandle Rules; // the global surface rules

    // `tilesX` by one tile, or a square of `tilesX` tiles a side when `tilesZ` is not one.
    explicit SplatStoreTerrain(uint32 tilesX = 2u, uint32 tilesZ = 1u)
    {
        auto& svc = TerrainService::Get();
        const auto handle =
            tilesZ == 1u ? CreateResidentTiledTerrain(svc, tilesX) : CreateResidentSquareTiledTerrain(svc, tilesX);
        Tiled = svc.GetTiledTerrainData(handle);
        RegisterModifierLifecycleEvents(World);
        Entity = CreateTiledTerrainEntity(World, handle, tilesX);
        World.GetComponentForWrite<Components::Terrain>(Entity)->SizeZ = Tiled->Config.WorldSizeZ;
        // The extraction system places the tiles at the entity's corner, and no extraction runs here.
        // So the entity is centered on the corner that the tiles hold.
        Components::WorldTransform* transform = World.GetComponentForWrite<Components::WorldTransform>(Entity);
        transform->matrix[12] = Tiled->WorldOriginX + Tiled->Config.WorldSizeX * 0.5f;
        transform->matrix[14] = Tiled->WorldOriginZ + Tiled->Config.WorldSizeZ * 0.5f;
        Rules = CreateGlobalRulesModifier(World);
        System.Update(World, 1.0f / 60.0f);
    }
};

// A splat store is cold-baked coarsest level first and holds no level-0 page; tile batches then
// add level 0, which is the tile bake's splat, and every coarser level settles on the weights tent
// of level 0 whatever the tile order.
TEST(TerrainRegionBake, ASplatStoreBakesItsCoarseLevelsFirstAndSettlesOnTheTentOfLevel0)
{
    SplatStoreTerrain terrain;
    ASSERT_NE(terrain.Tiled, nullptr);
    ASSERT_TRUE(terrain.Tiled->SplatBakeRangeValid);
    TiledTerrainData& tiled = *terrain.Tiled;
    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    JobSystem::WorkStealingThreadPool pool(4);
    TerrainSurfacePageBaker baker(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                  SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                  GeneratedHeightPages(provider, tiled), &pool);
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    ASSERT_GE(levels.size(), 5u);
    constexpr uint32 kFirstCoarse = 3u;
    ScopedStoreFolder folder("coarse-first");
    const std::filesystem::path forward = folder.Path / "forward.gepage";
    const std::filesystem::path backward = folder.Path / "backward.gepage";
    ASSERT_EQ(baker.CreateStore(forward, 7u, kFirstCoarse), "");
    {
        PageStreaming::PageStoreReader cold;
        ASSERT_EQ(cold.Open(forward, PageStreaming::PageFieldKind::Splat), "");
        for (uint32 level = 0; level < levels.size(); ++level)
            for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
                for (uint32 px = 0; px < levels[level].PagesX; ++px)
                {
                    const PageStreaming::PageAddress address{0, static_cast<uint8>(level), px, pz};
                    EXPECT_EQ(cold.Layout().FindPresent(address) != nullptr, level >= kFirstCoarse)
                        << "level " << level << " page (" << px << ", " << pz << ")";
                }
    }
    ASSERT_EQ(baker.CreateStore(backward, 7u, kFirstCoarse), "");
    for (uint32 tx = 0; tx < 2u; ++tx)
    {
        ASSERT_EQ(baker.BakeLevel0Pages(forward, TilePageBatch(tiled, tx, 0u)), "");
        ASSERT_EQ(baker.Finish(), "");
        ASSERT_EQ(baker.BakeLevel0Pages(backward, TilePageBatch(tiled, 1u - tx, 0u)), "");
        ASSERT_EQ(baker.Finish(), "");
    }

    // The settled pyramid: every level-0 page written, then every coarser page refiltered from it.
    const std::filesystem::path settled = folder.Path / "settled.gepage";
    PageStreaming::PageStoreReader forwardStore;
    ASSERT_EQ(forwardStore.Open(forward, PageStreaming::PageFieldKind::Splat), "");
    {
        PageStreaming::PageStoreWriter writer;
        ASSERT_EQ(writer.Create(settled, PageStreaming::MakePageStoreLayout(
            MakeSurfaceStoreHeader(tiled.Config, PageStreaming::PageFieldKind::Splat, 7u))), "");
        PageStreaming::PageStorePatcher patcher(writer, nullptr);
        for (uint32 pz = 0; pz < levels[0].PagesZ; ++pz)
            for (uint32 px = 0; px < levels[0].PagesX; ++px)
            {
                const PageStreaming::PageAddress address{0, 0, px, pz};
                ASSERT_TRUE(patcher.WritePage(address, StoreSplatPage(forwardStore, address)));
            }
        ASSERT_TRUE(patcher.RefilterAncestors(
            PageStreaming::PageSampleRect{0u, 0u, levels[0].SamplesX - 1u, levels[0].SamplesZ - 1u}));
        ASSERT_EQ(writer.Finish(), "");
    }
    PageStreaming::PageStoreReader backwardStore;
    PageStreaming::PageStoreReader settledStore;
    ASSERT_EQ(backwardStore.Open(backward, PageStreaming::PageFieldKind::Splat), "");
    ASSERT_EQ(settledStore.Open(settled, PageStreaming::PageFieldKind::Splat), "");
    SplatPageComparison comparison;
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < levels[level].PagesX; ++px)
            {
                const PageStreaming::PageAddress address{0, static_cast<uint8>(level), px, pz};
                const std::vector<uint8> page = StoreSplatPage(forwardStore, address);
                ASSERT_EQ(page, StoreSplatPage(settledStore, address))
                    << "level " << level << " page (" << px << ", " << pz << ") is not the tent of level 0";
                ASSERT_EQ(page, StoreSplatPage(backwardStore, address))
                    << "level " << level << " page (" << px << ", " << pz << ") depends on the tile order";
                if (level == 0u)
                    ExpectSplatPageMatchesTiles(tiled, px, pz, page, comparison);
                if (HasFatalFailure())
                    return;
            }
    EXPECT_GT(comparison.Compared, 2000000u);
    EXPECT_GT(comparison.Weighted, 1000u) << "the rules never reached a compared texel";
}

// The height pages of `provider` with height page `neighbor` made from the edge of height page
// `own`: each of its samples is the nearest sample that `own` stores, as a page bake clamps them.
SurfaceHeightPageFetch EdgeExtendedHeightPages(const PageStreaming::GeneratedHeightPageProvider& provider,
                                               const TiledTerrainData& tiled, PageStreaming::PageAddress neighbor,
                                               PageStreaming::PageAddress own)
{
    const SurfaceHeightPageFetch pages = GeneratedHeightPages(provider, tiled);
    return [pages, neighbor, own](const PageStreaming::PageAddress& address, std::span<float32> samples) {
        if (address != neighbor)
            return pages(address, samples);
        std::vector<float32> owned(PageStreaming::kPageSampleCount);
        if (!pages(own, owned))
            return false;
        constexpr int64 kStride = PageStreaming::kPageStrideSamples;
        const auto first = [](uint32 page) {
            return static_cast<int64>(page) * PageStreaming::kPageOwnedSamples - PageStreaming::kPageApronSamples;
        };
        for (int64 row = 0; row < kStride; ++row)
            for (int64 column = 0; column < kStride; ++column)
            {
                const int64 x = std::clamp(first(neighbor.X) + column, first(own.X), first(own.X) + kStride - 1);
                const int64 z = std::clamp(first(neighbor.Z) + row, first(own.Z), first(own.Z) + kStride - 1);
                samples[static_cast<std::size_t>(row * kStride + column)] =
                    owned[static_cast<std::size_t>((z - first(own.Z)) * kStride + x - first(own.X))];
            }
        return true;
    };
}

// The stored bytes of page `address` of the store at `file`.
std::vector<uint8> StoredPageBytes(const std::filesystem::path& file, PageStreaming::PageFieldKind plane,
                                   const PageStreaming::PageAddress& address)
{
    PageStreaming::PageStoreReader store;
    EXPECT_EQ(store.Open(file, plane), "");
    std::vector<uint8> bytes;
    EXPECT_TRUE(store.ReadPageBytes(address, bytes));
    return bytes;
}

// A page at a tile boundary reads the edge rows of its neighbor height page. With the neighbor on
// hand, its apron is the tile bake's. Without it, the apron takes the nearest heights of the page's
// own height page. Without its own height page, the page is not baked and the bake says so.
TEST(TerrainRegionBake, ASurfacePageReadsItsNeighborsHeightsForItsApronAndClampsItWhenTheyAreAbsent)
{
    SplatStoreTerrain terrain;
    ASSERT_NE(terrain.Tiled, nullptr);
    ASSERT_TRUE(terrain.Tiled->SplatBakeRangeValid);
    TiledTerrainData& tiled = *terrain.Tiled;

    // Page 8 owns samples 1024 to 1151, tile 1's first; its apron column is sample 1023, whose
    // slope reads sample 1022 of height page 7.
    const uint32 interior = tiled.Config.TileConfig.HeightmapWidth - 1u;
    const PageStreaming::PageAddress boundary{0, 0, interior / PageStreaming::kPageOwnedSamples, 0};
    ASSERT_EQ(boundary.X * PageStreaming::kPageOwnedSamples, interior);
    const PageStreaming::PageAddress neighbor{0, 0, boundary.X - 1u, 0};
    std::optional<PageStreaming::PageAddress> refused = neighbor;
    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    ScopedStoreFolder folder("apron");
    const PageStreaming::PageAddress batch[] = {boundary};

    // The normal reads the slope at its apron: the clamped apron is the bake over edge-extended heights.
    const PageStreaming::PageFieldKind normal = PageStreaming::PageFieldKind::Normal;
    TerrainSurfacePageBaker refusedNormal(terrain.System.SnapshotSurfaceBake(tiled, {}), normal,
                                          SurfaceAccess(tiled, normal),
                                          GeneratedHeightPages(provider, tiled, &refused), nullptr);
    TerrainSurfacePageBaker extendedNormal(terrain.System.SnapshotSurfaceBake(tiled, {}), normal,
                                           SurfaceAccess(tiled, normal),
                                           EdgeExtendedHeightPages(provider, tiled, neighbor, boundary), nullptr);
    TerrainSurfacePageBaker wholeNormal(terrain.System.SnapshotSurfaceBake(tiled, {}), normal,
                                        SurfaceAccess(tiled, normal), GeneratedHeightPages(provider, tiled), nullptr);
    const std::filesystem::path refusedFile = folder.Path / "refused.gepage";
    const std::filesystem::path extendedFile = folder.Path / "extended.gepage";
    const std::filesystem::path wholeFile = folder.Path / "whole.gepage";
    for (const auto& [baker, file] : {std::pair{&refusedNormal, &refusedFile}, std::pair{&extendedNormal, &extendedFile},
                                      std::pair{&wholeNormal, &wholeFile}})
    {
        ASSERT_EQ(baker->CreateStore(*file, 7u, 3u), "");
        ASSERT_EQ(baker->BakeLevel0Pages(*file, batch), "") << "an absent neighbor clamps the apron";
        ASSERT_EQ(baker->Finish(), "");
    }
    const std::vector<uint8> clamped = StoredPageBytes(refusedFile, normal, boundary);
    EXPECT_EQ(clamped, StoredPageBytes(extendedFile, normal, boundary)) << "the apron is not the clamped heights'";
    EXPECT_NE(clamped, StoredPageBytes(wholeFile, normal, boundary)) << "the neighbor's heights never reached the apron";

    // With the neighbor on hand, the splat apron is the tile bake's.
    TerrainSurfacePageBaker baker(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                  SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                  GeneratedHeightPages(provider, tiled, &refused), nullptr);
    const std::filesystem::path file = folder.Path / "splat.gepage";
    ASSERT_EQ(baker.CreateStore(file, 7u, 3u), "");
    refused.reset();
    ASSERT_EQ(baker.BakeLevel0Pages(file, batch), "");
    ASSERT_EQ(baker.Finish(), "");
    {
        PageStreaming::PageStoreReader store;
        ASSERT_EQ(store.Open(file, PageStreaming::PageFieldKind::Splat), "");
        SplatPageComparison comparison;
        ExpectSplatPageMatchesTiles(tiled, boundary.X, boundary.Z, StoreSplatPage(store, boundary), comparison);
        EXPECT_GT(comparison.Compared, 16000u);
    }

    // Without its own height page, the page is not baked and the bake names it.
    refused = boundary;
    const PageStreaming::PageAddress unbaked[] = {PageStreaming::PageAddress{0, 0, boundary.X, 1u}, boundary};
    const std::string reason = baker.BakeLevel0Pages(file, unbaked);
    EXPECT_NE(reason.find("level 0 page (" + std::to_string(boundary.X) + ", 0) is not on hand"), std::string::npos)
        << reason;
}

// The tile bake's normal map of every tile, as two bytes a texel the way a normal page stores them.
std::vector<std::vector<uint8>> TileNormalBytes(TiledTerrainData& tiled)
{
    std::vector<std::vector<uint8>> tiles;
    for (uint32 tz = 0; tz < tiled.Config.TilesPerAxisZ; ++tz)
        for (uint32 tx = 0; tx < tiled.Config.TilesPerAxisX; ++tx)
        {
            const TerrainTileData* tile = Tile(&tiled, static_cast<int32>(tx), static_cast<int32>(tz));
            std::vector<uint8> halves;
            uint32 width = 0;
            uint32 height = 0;
            GenerateNormalmapFromHeightfield(tile->Heightfield, tiled.Config.TileWorldSize, tiled.Config.TileWorldSize,
                                             tiled.Config.HeightScale, halves, width, height);
            std::vector<uint8> bytes(static_cast<std::size_t>(width) * height * 2u);
            const auto* half = reinterpret_cast<const uint16*>(halves.data());
            for (std::size_t t = 0; t < static_cast<std::size_t>(width) * height; ++t)
            {
                const float32 x = Mathematics::HalfToFloat(half[t * 2u]);
                const float32 z = Mathematics::HalfToFloat(half[t * 2u + 1u]);
                PageStreaming::EncodeNormalSample(Mathematics::Vector3(x, 0.0f, z), &bytes[t * 2u]);
            }
            tiles.push_back(std::move(bytes));
        }
    return tiles;
}

// A Normal store's level-0 pages are the tile bake's normal map. Its samples cache holds the
// unencoded samples within one step of the tile map (the tile map holds half floats); the Bc5 pages
// decode within kBc5Error of the tile map. Deleting the store deletes its cache.
TEST(TerrainRegionBake, ANormalStoresLevel0PagesAreTheTileNormalMapWithinTheBc5Error)
{
    // The stated bound for this terrain's smooth slopes: BC5 keeps 8 levels a block per channel.
    constexpr int32 kBc5Error = 8;
    SplatStoreTerrain terrain;
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    JobSystem::WorkStealingThreadPool pool(4);
    TerrainSurfacePageBaker baker(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Normal,
                           SurfaceAccess(tiled, PageStreaming::PageFieldKind::Normal),
                           GeneratedHeightPages(provider, tiled), &pool);
    ScopedStoreFolder folder("normal");
    const std::filesystem::path file = folder.Path / "normal.gepage";
    ASSERT_EQ(baker.CreateStore(file, 7u, 3u), "");
    for (uint32 tx = 0; tx < tiled.Config.TilesPerAxisX; ++tx)
        ASSERT_EQ(baker.BakeLevel0Pages(file, TilePageBatch(tiled, tx, 0u)), "");
    ASSERT_EQ(baker.Finish(), "");

    PageStreaming::PageStoreReader store;
    PageStreaming::PageStoreReader cache;
    ASSERT_EQ(store.Open(file, PageStreaming::PageFieldKind::Normal), "");
    ASSERT_EQ(cache.Open(SurfaceSamplesCachePath(file), PageStreaming::PageFieldKind::NormalSamples), "");
    const std::vector<std::vector<uint8>> tileNormals = TileNormalBytes(tiled);
    const uint32 interior = tiled.Config.TileConfig.HeightmapWidth - 1u;
    const uint32 tileWidth = tiled.Config.TileConfig.HeightmapWidth;
    const uint32 stride = PageStreaming::PageStrideTexels(PageStreaming::PageFieldFormat::Bc5);
    const int64 apron = (stride - PageStreaming::kPageOwnedSamples) / 2u;
    const PageStreaming::PageStoreLevel level0 = TileLatticeLevels(tiled).front();
    std::vector<uint8> stored(static_cast<std::size_t>(stride) * stride * 2u);
    // The cache holds the byte-format stride, the center of the Bc5 stride.
    const uint32 cacheStride = PageStreaming::PageStrideTexels(PageStreaming::PageFieldFormat::Rg8);
    const uint32 cacheInset = (stride - cacheStride) / 2u;
    std::vector<uint8> samples(static_cast<std::size_t>(cacheStride) * cacheStride * 2u);
    std::size_t compared = 0;
    int32 worstBc5 = 0;
    for (uint32 pz = 0; pz < level0.PagesZ; ++pz)
        for (uint32 px = 0; px < level0.PagesX; ++px)
        {
            const PageStreaming::PageAddress address{0, 0, px, pz};
            ASSERT_TRUE(store.ReadPageSamples(address, stored));
            ASSERT_TRUE(cache.ReadPageSamples(address, samples));
            for (uint32 j = 0; j < stride; ++j)
                for (uint32 i = 0; i < stride; ++i)
                {
                    const int64 sx = static_cast<int64>(px) * PageStreaming::kPageOwnedSamples + i - apron;
                    const int64 sz = static_cast<int64>(pz) * PageStreaming::kPageOwnedSamples + j - apron;
                    if (sx < 0 || sz < 0 || sx >= level0.SamplesX || sz >= level0.SamplesZ)
                        continue;
                    const uint32 gx = static_cast<uint32>(sx);
                    const uint32 gz = static_cast<uint32>(sz);
                    // A tile clamps its difference at its edges (#5027); the page reads across them.
                    if (gx % interior == 0u || gz % interior == 0u)
                        continue;
                    const uint32 tx = gx / interior;
                    const uint32 tz = gz / interior;
                    const uint8* expected =
                        &tileNormals[tz * tiled.Config.TilesPerAxisX + tx]
                                    [(static_cast<std::size_t>(gz - tz * interior) * tileWidth + (gx - tx * interior)) * 2u];
                    const uint8* bc5 = &stored[(static_cast<std::size_t>(j) * stride + i) * 2u];
                    for (uint32 c = 0; c < 2u; ++c)
                        worstBc5 = std::max(worstBc5, std::abs(int32(bc5[c]) - int32(expected[c])));
                    ++compared;
                    if (i < cacheInset || j < cacheInset || i - cacheInset >= cacheStride || j - cacheInset >= cacheStride)
                        continue;
                    const uint8* unencoded =
                        &samples[(static_cast<std::size_t>(j - cacheInset) * cacheStride + (i - cacheInset)) * 2u];
                    for (uint32 c = 0; c < 2u; ++c)
                        ASSERT_LE(std::abs(int32(unencoded[c]) - int32(expected[c])), 1)
                            << "page (" << px << ", " << pz << ") stride texel (" << i << ", " << j << ")";
                }
        }
    EXPECT_GT(compared, 2000000u);
    EXPECT_LE(worstBc5, kBc5Error);

    EXPECT_TRUE(RemoveSurfaceStore(file));
    EXPECT_FALSE(std::filesystem::exists(file));
    EXPECT_FALSE(std::filesystem::exists(SurfaceSamplesCachePath(file)));
}

// A GrassControl store's level-0 pages are the tile bake's grass control field (a terrain this size
// has one unified field), within one step: the page places a texel from its tile's origin.
TEST(TerrainRegionBake, AGrassControlStoresLevel0PagesAreTheTileGrassField)
{
    SplatStoreTerrain terrain;
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    // A grass region across the seam of tiles 0 and 1.
    const ECS::EntityHandle region = terrain.World.CreateEntity();
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Circle;
    volume.Radius = 250.0f;
    volume.Falloff = 60.0f;
    terrain.World.AddComponentImmediate<Components::TerrainModifierVolume>(region, volume);
    Components::TerrainGrassEffect grass{};
    grass.HeightScale = 0.3f;
    grass.DensityScale = 0.5f;
    terrain.World.AddComponentImmediate<Components::TerrainGrassEffect>(region, grass);
    Components::WorldTransform transform{};
    transform.matrix[12] = tiled.WorldOriginX + tiled.Config.TileWorldSize;
    transform.matrix[14] = tiled.WorldOriginZ + 300.0f;
    terrain.World.AddComponentImmediate<Components::WorldTransform>(region, transform);
    // The grass compose runs as a job: each update swaps in the one before.
    for (int32 settle = 0; settle < 3; ++settle)
    {
        terrain.World.SwapLifecycleEvents();
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        terrain.System.WaitForGrassComposeForTests();
    }

    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    JobSystem::WorkStealingThreadPool pool(4);
    TerrainSurfacePageBaker baker(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::GrassControl,
                           SurfaceAccess(tiled, PageStreaming::PageFieldKind::GrassControl),
                           GeneratedHeightPages(provider, tiled), &pool);
    ScopedStoreFolder folder("grass");
    const std::filesystem::path file = folder.Path / "grass.gepage";
    ASSERT_EQ(baker.CreateStore(file, 7u, 3u), "");
    for (uint32 tx = 0; tx < tiled.Config.TilesPerAxisX; ++tx)
        ASSERT_EQ(baker.BakeLevel0Pages(file, TilePageBatch(tiled, tx, 0u)), "");
    ASSERT_EQ(baker.Finish(), "");

    PageStreaming::PageStoreReader store;
    ASSERT_EQ(store.Open(file, PageStreaming::PageFieldKind::GrassControl), "");
    constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
    const PageStreaming::PageStoreLevel level0 = TileLatticeLevels(tiled).front();
    const TerrainGrassField& field = tiled.GrassUnifiedField;
    ASSERT_EQ(field.Width, level0.SamplesX);
    ASSERT_EQ(field.Height, level0.SamplesZ);
    std::vector<uint8> page(static_cast<std::size_t>(PageStreaming::kPageSampleCount) * 2u);
    std::size_t compared = 0;
    std::size_t shaped = 0;
    for (uint32 pz = 0; pz < level0.PagesZ; ++pz)
        for (uint32 px = 0; px < level0.PagesX; ++px)
        {
            ASSERT_TRUE(store.ReadPageSamples(PageStreaming::PageAddress{0, 0, px, pz}, page));
            for (uint32 j = 0; j < kStride; ++j)
                for (uint32 i = 0; i < kStride; ++i)
                {
                    const int64 sx = static_cast<int64>(px) * PageStreaming::kPageOwnedSamples + i - 1;
                    const int64 sz = static_cast<int64>(pz) * PageStreaming::kPageOwnedSamples + j - 1;
                    if (sx < 0 || sz < 0 || sx >= level0.SamplesX || sz >= level0.SamplesZ)
                        continue;
                    const uint32 gx = static_cast<uint32>(sx);
                    const uint32 gz = static_cast<uint32>(sz);
                    const std::size_t at = (static_cast<std::size_t>(gz) * field.Width + gx) * 2u;
                    const uint8* paged = &page[(static_cast<std::size_t>(j) * kStride + i) * 2u];
                    for (uint32 c = 0; c < 2u; ++c)
                    {
                        const int32 expected = field.Pixels.empty() ? 255 : field.Pixels[at + c];
                        ASSERT_LE(std::abs(int32(paged[c]) - expected), 1)
                            << "page (" << px << ", " << pz << ") stride texel (" << i << ", " << j
                            << ") channel " << c;
                    }
                    ++compared;
                    if (paged[1] < 200u)
                        ++shaped;
                }
        }
    EXPECT_GT(compared, 2000000u);
    EXPECT_GT(shaped, 10000u) << "the grass region never reached a compared texel";
}

// A Color store's level-0 pages are the tile bake's color map (a terrain this size has one unified
// map), within two steps: the page samples the grass control on its own lattice. Tile seams are skipped
// as for the splat.
TEST(TerrainRegionBake, AColorStoresLevel0PagesAreTheTileColorMap)
{
    SplatStoreTerrain terrain;
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    // The grass's ground write reads the splat; a grass region and a color volume cross the tile seam.
    Components::TerrainGrass terrainGrass{};
    terrainGrass.GroundDarkening = 0.6f;
    terrain.World.AddComponentImmediate<Components::TerrainGrass>(terrain.Entity, terrainGrass);
    const ECS::EntityHandle region = terrain.World.CreateEntity();
    Components::TerrainModifierVolume circle{};
    circle.Shape = Components::TerrainVolumeShape::Circle;
    circle.Radius = 250.0f;
    circle.Falloff = 60.0f;
    terrain.World.AddComponentImmediate<Components::TerrainModifierVolume>(region, circle);
    Components::TerrainGrassEffect grass{};
    grass.DensityScale = 0.4f;
    terrain.World.AddComponentImmediate<Components::TerrainGrassEffect>(region, grass);
    Components::WorldTransform regionAt{};
    regionAt.matrix[12] = tiled.WorldOriginX + tiled.Config.TileWorldSize;
    regionAt.matrix[14] = tiled.WorldOriginZ + 300.0f;
    terrain.World.AddComponentImmediate<Components::WorldTransform>(region, regionAt);
    const ECS::EntityHandle paintVolume = terrain.World.CreateEntity();
    Components::TerrainModifierVolume rectangle{};
    rectangle.Shape = Components::TerrainVolumeShape::Rectangle;
    rectangle.RectHalfX = 200.0f;
    rectangle.RectHalfZ = 120.0f;
    rectangle.Falloff = 40.0f;
    terrain.World.AddComponentImmediate<Components::TerrainModifierVolume>(paintVolume, rectangle);
    Components::TerrainColorEffect color{};
    color.Color.r = 0.8f;
    color.Color.g = 0.3f;
    color.Color.b = 0.1f;
    color.Strength = 0.7f;
    terrain.World.AddComponentImmediate<Components::TerrainColorEffect>(paintVolume, color);
    Components::WorldTransform paintedAt{};
    paintedAt.matrix[12] = tiled.WorldOriginX + tiled.Config.TileWorldSize;
    paintedAt.matrix[14] = tiled.WorldOriginZ + 700.0f;
    terrain.World.AddComponentImmediate<Components::WorldTransform>(paintVolume, paintedAt);
    // The grass compose runs as a job and the color map follows the field it lands.
    for (int32 settle = 0; settle < 4; ++settle)
    {
        terrain.World.SwapLifecycleEvents();
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        terrain.System.WaitForGrassComposeForTests();
    }
    const TerrainColorMap& map = tiled.ColorUnifiedMap;
    const PageStreaming::PageStoreLevel level0 = TileLatticeLevels(tiled).front();
    ASSERT_TRUE(map.IsActive());
    ASSERT_EQ(map.Width, level0.SamplesX);
    ASSERT_EQ(map.Height, level0.SamplesZ);

    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    JobSystem::WorkStealingThreadPool pool(4);
    TerrainSurfacePageBaker baker(
        terrain.System.SnapshotSurfaceBake(tiled, TerrainColorMapBaker::ResolveGroundWrite(terrain.World, terrain.Entity)),
        PageStreaming::PageFieldKind::Color, SurfaceAccess(tiled, PageStreaming::PageFieldKind::Color),
        GeneratedHeightPages(provider, tiled), &pool);
    ScopedStoreFolder folder("color");
    const std::filesystem::path file = folder.Path / "color.gepage";
    ASSERT_EQ(baker.CreateStore(file, 7u, 3u), "");
    for (uint32 tx = 0; tx < tiled.Config.TilesPerAxisX; ++tx)
        ASSERT_EQ(baker.BakeLevel0Pages(file, TilePageBatch(tiled, tx, 0u)), "");
    ASSERT_EQ(baker.Finish(), "");

    PageStreaming::PageStoreReader store;
    ASSERT_EQ(store.Open(file, PageStreaming::PageFieldKind::Color), "");
    constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
    const uint32 interior = tiled.Config.TileConfig.HeightmapWidth - 1u;
    std::vector<uint8> page(static_cast<std::size_t>(PageStreaming::kPageSampleCount) * 4u);
    std::size_t compared = 0;
    std::size_t darkened = 0;
    std::size_t painted = 0;
    for (uint32 pz = 0; pz < level0.PagesZ; ++pz)
        for (uint32 px = 0; px < level0.PagesX; ++px)
        {
            ASSERT_TRUE(store.ReadPageSamples(PageStreaming::PageAddress{0, 0, px, pz}, page));
            for (uint32 j = 0; j < kStride; ++j)
                for (uint32 i = 0; i < kStride; ++i)
                {
                    const int64 sx = static_cast<int64>(px) * PageStreaming::kPageOwnedSamples + i - 1;
                    const int64 sz = static_cast<int64>(pz) * PageStreaming::kPageOwnedSamples + j - 1;
                    if (sx < 0 || sz < 0 || sx >= level0.SamplesX || sz >= level0.SamplesZ)
                        continue;
                    const uint32 gx = static_cast<uint32>(sx);
                    const uint32 gz = static_cast<uint32>(sz);
                    if ((gx > 0u && gx % interior == 0u) || (gz > 0u && gz % interior == 0u))
                        continue;
                    const uint8* expected = &map.Pixels[(static_cast<std::size_t>(gz) * map.Width + gx) * 4u];
                    const uint8* paged = &page[(static_cast<std::size_t>(j) * kStride + i) * 4u];
                    for (uint32 c = 0; c < 4u; ++c)
                        ASSERT_LE(std::abs(int32(paged[c]) - int32(expected[c])), 2)
                            << "page (" << px << ", " << pz << ") stride texel (" << i << ", " << j
                            << ") channel " << c;
                    ++compared;
                    // The color volume starts 540 m in: below that only the ground write writes.
                    darkened += gz < 500u && paged[3] > 0u ? 1u : 0u;
                    painted += paged[0] > paged[1] + 20u ? 1u : 0u;
                }
        }
    EXPECT_GT(compared, 2000000u);
    EXPECT_GT(darkened, 1000u) << "the ground write never reached a compared texel";
    EXPECT_GT(painted, 10000u) << "the color volume never reached a compared texel";
}

// Bakes every page of `baker`'s store at `file`: the coarse levels, then level 0 tile by tile.
std::string BakeWholeStore(TerrainSurfacePageBaker& baker, const TiledTerrainData& tiled, const std::filesystem::path& file)
{
    std::string failure = baker.CreateStore(file, 7u, 3u);
    for (uint32 tz = 0; tz < tiled.Config.TilesPerAxisZ && failure.empty(); ++tz)
        for (uint32 tx = 0; tx < tiled.Config.TilesPerAxisX && failure.empty(); ++tx)
            failure = baker.BakeLevel0Pages(file, TilePageBatch(tiled, tx, tz));
    return failure.empty() ? baker.Finish() : failure;
}

// The hash of every stored page of the store at `file`, level by level.
std::vector<uint64> StorePageHashes(const std::filesystem::path& file,
                                    std::span<const PageStreaming::PageStoreLevel> levels,
                                    PageStreaming::PageFieldKind plane = PageStreaming::PageFieldKind::Splat)
{
    PageStreaming::PageStoreReader store;
    EXPECT_EQ(store.Open(file, plane), "");
    std::vector<uint64> hashes;
    std::vector<uint8> bytes;
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < levels[level].PagesX; ++px)
            {
                const PageStreaming::PageAddress address{0, static_cast<uint8>(level), px, pz};
                const bool read = store.ReadPageBytes(address, bytes);
                hashes.push_back(read ? Hashing::Fnv1a64(bytes.data(), bytes.size()) : 0u);
            }
    return hashes;
}

// A paint dab on layer 3 at radius 14 m on the corner of tiles (0, 0) and (1, 1).
ECS::EntityHandle AddCornerPaintDab(SplatStoreTerrain& terrain)
{
    const ECS::EntityHandle dab = terrain.World.CreateEntity();
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Circle;
    volume.Radius = 14.0f;
    volume.Falloff = 4.0f;
    terrain.World.AddComponentImmediate<Components::TerrainModifierVolume>(dab, volume);
    Components::TerrainPaintLayerEffect paint{};
    paint.LayerIndex = 3;
    paint.Strength = 0.8f;
    terrain.World.AddComponentImmediate<Components::TerrainPaintLayerEffect>(dab, paint);
    Components::WorldTransform at{};
    at.matrix[12] = terrain.Tiled->WorldOriginX + terrain.Tiled->Config.TileWorldSize;
    at.matrix[14] = terrain.Tiled->WorldOriginZ + terrain.Tiled->Config.TileWorldSize;
    terrain.World.AddComponentImmediate<Components::WorldTransform>(dab, at);
    return dab;
}

// A brush dab on the corner of four tiles of a 3 by 3 tile terrain publishes its rect. The pages
// that read the rect bake again with their ancestors, and the store is then the full bake of the
// painted terrain. The undo returns every page to its hash before the dab.
TEST(TerrainRegionBake, AStrokeRebakesThePagesItsRectReachesAndItsUndoRestoresThem)
{
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    JobSystem::WorkStealingThreadPool pool(4);
    TerrainSurfacePageBaker baker(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                           SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                           GeneratedHeightPages(provider, tiled), &pool);
    ScopedStoreFolder folder("stroke");
    const std::filesystem::path file = folder.Path / "stroked.gepage";
    ASSERT_EQ(BakeWholeStore(baker, tiled, file), "");
    const std::vector<uint64> before = StorePageHashes(file, levels);

    const ECS::EntityHandle dab = AddCornerPaintDab(terrain);
    const uint64 unpainted = tiled.SurfaceVersion;
    terrain.World.SwapLifecycleEvents();
    terrain.System.Update(terrain.World, 1.0f / 60.0f);
    ASSERT_GT(tiled.SurfaceVersion, unpainted) << "the dab published no surface rect";
    const uint32 corner = tiled.Config.TileConfig.HeightmapWidth - 1u;
    ASSERT_GT(TileSplatTexel(tiled, corner, corner)[3], 100u) << "the tile bake did not paint the dab";
    EXPECT_EQ(tiled.SurfaceChangedSince, unpainted);
    EXPECT_EQ(tiled.SurfaceRefresh, SurfaceRefreshReason::Edit);
    EXPECT_LT(tiled.SurfaceChanged.Width(), 64u);
    EXPECT_LT(tiled.SurfaceChanged.Height(), 64u);
    const std::vector<PageStreaming::PageAddress> pages = baker.Level0PagesReading(tiled.SurfaceChanged);
    ASSERT_FALSE(pages.empty());
    EXPECT_LE(pages.size(), 4u);
    baker.SetInputs(terrain.System.SnapshotSurfaceBake(tiled, {}), GeneratedHeightPages(provider, tiled));
    const uint64 writtenBefore = baker.PagesWritten();
    ASSERT_EQ(baker.BakeLevel0Pages(file, pages), "");
    ASSERT_EQ(baker.Finish(), "");
    // The dab's pages and at most one ancestor each per coarser level.
    EXPECT_GT(baker.PagesWritten(), writtenBefore);
    EXPECT_LE(baker.PagesWritten() - writtenBefore, pages.size() * levels.size());

    TerrainSurfacePageBaker fresh(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                           SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                           GeneratedHeightPages(provider, tiled), &pool);
    const std::filesystem::path full = folder.Path / "full.gepage";
    ASSERT_EQ(BakeWholeStore(fresh, tiled, full), "");
    const std::vector<uint64> painted = StorePageHashes(file, levels);
    EXPECT_EQ(painted, StorePageHashes(full, levels));
    EXPECT_NE(painted, before);

    const uint64 paintedVersion = tiled.SurfaceVersion;
    terrain.World.DestroyEntityImmediate(dab);
    terrain.World.SwapLifecycleEvents();
    terrain.System.Update(terrain.World, 1.0f / 60.0f);
    ASSERT_GT(tiled.SurfaceVersion, paintedVersion) << "the undo published no surface rect";
    baker.SetInputs(terrain.System.SnapshotSurfaceBake(tiled, {}), GeneratedHeightPages(provider, tiled));
    ASSERT_EQ(baker.BakeLevel0Pages(file, baker.Level0PagesReading(tiled.SurfaceChanged)), "");
    ASSERT_EQ(baker.Finish(), "");
    EXPECT_EQ(StorePageHashes(file, levels), before);
}

// Gives the terrain of `terrain` scene `scene`, a scene tag and the cache folder `folder`, then
// runs one update: the driver opens the terrain's surface stores.
void GiveSurfaceStores(SplatStoreTerrain& terrain, const std::filesystem::path& folder, const GUID& scene)
{
    HeightPageStoreLocation cache;
    cache.CacheDirectory = folder;
    TerrainService::Get().SetHeightPageStoreLocation(cache);
    Components::SceneEntityTag tag{};
    std::strncpy(tag.value, "terrain-near", sizeof(tag.value));
    terrain.World.AddComponentImmediate<Components::SceneEntityTag>(terrain.Entity, tag);
    terrain.World.GetComponentForWrite<Components::Terrain>(terrain.Entity)->BakeOriginScene = scene;
    terrain.Tiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;
    terrain.System.Update(terrain.World, 1.0f / 60.0f);
}

TiledTerrainHandle TiledHandleOf(const SplatStoreTerrain& terrain)
{
    const Components::Terrain& component = *terrain.World.GetComponent<Components::Terrain>(terrain.Entity);
    return TiledTerrainHandle{component.TiledTerrainHandle, component.TiledTerrainGeneration};
}

// Updates `terrain` past the settle of an edit made before the call, then until its driver has no
// level-0 page to bake, at most `frames` frames. False when pages stay.
bool SettleSurfacePages(SplatStoreTerrain& terrain, uint32 frames)
{
    for (uint32 frame = 1; frame <= frames; ++frame)
    {
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        if (frame > TerrainSurfacePageDriver::kSurfaceSettleFrames &&
            terrain.System.SurfacePages().PendingPages(TiledHandleOf(terrain)) == 0u)
            return true;
    }
    return false;
}

// The frames a terrain of `level0` takes to bake its level-0 pages with no pool (one job of
// kLevel0PagesAJob pages a frame, run in its frame), with the settle.
uint32 FramesToSettle(const PageStreaming::PageStoreLevel& level0)
{
    const std::size_t pages = static_cast<std::size_t>(level0.PagesX) * level0.PagesZ;
    return static_cast<uint32>((pages + TerrainSurfacePageDriver::kLevel0PagesAJob - 1u) /
                               TerrainSurfacePageDriver::kLevel0PagesAJob) +
           TerrainSurfacePageDriver::kSurfaceSettleFrames + 1u;
}

// A scene load makes the stores with the pinned levels whole and no level-0 page; the next frames
// bake level 0, one job of kLevel0PagesAJob pages a frame. A stroke bakes again, at its settle, only
// the pages that read its rect and their ancestors. Each settled store is the full bake of the terrain.
TEST(TerrainRegionBake, ASceneLoadMakesTheSurfaceStoresAndAStrokeRebakesOnlyItsPages)
{
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    ScopedStoreFolder folder("driver");
    const GUID scene = GUID::Generate();
    GiveSurfaceStores(terrain, folder.Path, scene);

    const std::vector<SceneSurfaceStore> stores = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(stores.size(), 2u) << "a terrain with no grass region and no color writer has Splat and Normal";
    EXPECT_EQ(stores[0].Plane, PageStreaming::PageFieldKind::Splat);
    EXPECT_EQ(stores[1].Plane, PageStreaming::PageFieldKind::Normal);
    const std::filesystem::path* splatFile = &stores[0].File;
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    {
        // The pinned levels are whole, from the top down; the levels below them hold no page yet.
        PageStreaming::PageStoreReader store;
        ASSERT_EQ(store.Open(*splatFile, PageStreaming::PageFieldKind::Splat), "");
        uint32 firstWhole = static_cast<uint32>(levels.size());
        for (uint32 level = static_cast<uint32>(levels.size()); level-- > 0u;)
        {
            std::size_t present = 0;
            for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
                for (uint32 px = 0; px < levels[level].PagesX; ++px)
                    present += store.Layout().FindPresent(PageStreaming::PageAddress{0, static_cast<uint8>(level), px,
                                                                                     pz}) != nullptr;
            const std::size_t pages = static_cast<std::size_t>(levels[level].PagesX) * levels[level].PagesZ;
            if (present == pages && firstWhole == level + 1u)
                firstWhole = level;
            else
                EXPECT_EQ(present, 0u) << "level " << level << " is neither pinned nor empty";
        }
        EXPECT_LT(firstWhole, levels.size()) << "the top level is pinned";
        EXPECT_GT(firstWhole, 0u) << "the cold open bakes no level-0 page at once";
    }
    const Components::Terrain& component = *terrain.World.GetComponent<Components::Terrain>(terrain.Entity);
    const TiledTerrainHandle handle{component.TiledTerrainHandle, component.TiledTerrainGeneration};
    const std::size_t pages = static_cast<std::size_t>(levels.front().PagesX) * levels.front().PagesZ;
    EXPECT_EQ(terrain.System.SurfacePages().PendingPages(handle), pages);
    terrain.System.Update(terrain.World, 1.0f / 60.0f);
    EXPECT_EQ(terrain.System.SurfacePages().PendingPages(handle), pages - TerrainSurfacePageDriver::kLevel0PagesAJob)
        << "the cold open bakes one job of kLevel0PagesAJob pages a frame";
    ASSERT_TRUE(SettleSurfacePages(terrain, FramesToSettle(levels.front())));

    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    JobSystem::WorkStealingThreadPool pool(4);
    const std::filesystem::path full = folder.Path.parent_path() / "ge-surface-pages-driver-full.gepage";
    const auto expectFullBake = [&](const char* state) {
        TerrainSurfacePageBaker baker(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                               SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                               GeneratedHeightPages(provider, tiled), &pool);
        ASSERT_EQ(BakeWholeStore(baker, tiled, full), "");
        EXPECT_EQ(StorePageHashes(*splatFile, levels), StorePageHashes(full, levels)) << state;
        RemoveSurfaceStore(full);
    };
    expectFullBake("opened");
    const std::vector<uint64> before = StorePageHashes(*splatFile, levels);

    AddCornerPaintDab(terrain);
    terrain.World.SwapLifecycleEvents();
    ASSERT_TRUE(SettleSurfacePages(terrain, FramesToSettle(levels.front())));
    EXPECT_EQ(SceneSurfaceStores(folder.Path, scene), stores) << "a stroke patches the stores in place";
    const std::vector<uint64> painted = StorePageHashes(*splatFile, levels);
    const TerrainSurfacePageBaker reader(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                  SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                  GeneratedHeightPages(provider, tiled), nullptr);
    const std::vector<PageStreaming::PageAddress> reading = reader.Level0PagesReading(tiled.SurfaceChanged);
    std::size_t changed = 0;
    std::size_t at = 0;
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < levels[level].PagesX; ++px, ++at)
            {
                if (painted[at] == before[at])
                    continue;
                ++changed;
                const bool readsRect = std::any_of(reading.begin(), reading.end(), [&](const auto& page) {
                    return page.X >> level == px && page.Z >> level == pz;
                });
                EXPECT_TRUE(readsRect) << "level " << level << " page (" << px << ", " << pz << ") changed";
            }
    EXPECT_GT(changed, 0u) << "the stroke changed no page";
    expectFullBake("painted");
}

// The surface pages read the terrain's height scale and its Y origin. A change of either makes the
// stores again under a new key, and the store settles on the full bake at the new scale.
TEST(TerrainRegionBake, AHeightScaleOrYOriginChangeMakesTheSurfaceStoresAgainUnderANewKey)
{
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    ScopedStoreFolder folder("height-scale");
    const GUID scene = GUID::Generate();
    GiveSurfaceStores(terrain, folder.Path, scene);
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    const uint32 kSettleFrames = FramesToSettle(levels.front());
    ASSERT_TRUE(SettleSurfacePages(terrain, kSettleFrames));
    const std::vector<SceneSurfaceStore> opened = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(opened.size(), 2u);

    terrain.World.GetComponentForWrite<Components::Terrain>(terrain.Entity)->HeightScale *= 2.0f;
    tiled.Config.HeightScale *= 2.0f; // the extraction system's copy of the component's scale
    ASSERT_TRUE(SettleSurfacePages(terrain, kSettleFrames));
    const std::vector<SceneSurfaceStore> scaled = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(scaled.size(), 2u);
    EXPECT_NE(scaled[0].File, opened[0].File) << "the splat store kept the key of the old height scale";
    EXPECT_NE(scaled[1].File, opened[1].File) << "the normal store kept the key of the old height scale";
    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    JobSystem::WorkStealingThreadPool pool(4);
    TerrainSurfacePageBaker fresh(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Normal,
                                  SurfaceAccess(tiled, PageStreaming::PageFieldKind::Normal),
                                  GeneratedHeightPages(provider, tiled), &pool);
    const std::filesystem::path full = folder.Path.parent_path() / "ge-surface-pages-height-scale-full.gepage";
    ASSERT_EQ(BakeWholeStore(fresh, tiled, full), "");
    EXPECT_EQ(StorePageHashes(scaled[1].File, levels, PageStreaming::PageFieldKind::Normal),
              StorePageHashes(full, levels, PageStreaming::PageFieldKind::Normal))
        << "the normal store is not the bake at the new height scale";
    RemoveSurfaceStore(full);

    terrain.World.GetComponentForWrite<Components::WorldTransform>(terrain.Entity)->matrix[13] += 50.0f;
    ASSERT_TRUE(SettleSurfacePages(terrain, kSettleFrames));
    const std::vector<SceneSurfaceStore> raised = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(raised.size(), 2u);
    EXPECT_NE(raised[0].File, scaled[0].File) << "the splat store kept the key of the old Y origin";
}

// The dabs of a stroke bake its pages once, at the stroke's settle: the stores hold their pages
// from before the stroke while the dabs come, then the full bake of the painted terrain.
TEST(TerrainRegionBake, AStrokesDabsBakeTheSurfacePagesOnceAtItsSettle)
{
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    ScopedStoreFolder folder("stroke-settle");
    const GUID scene = GUID::Generate();
    GiveSurfaceStores(terrain, folder.Path, scene);
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    ASSERT_TRUE(SettleSurfacePages(terrain, FramesToSettle(levels.front())));
    const std::vector<SceneSurfaceStore> stores = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(stores.size(), 2u);
    const std::vector<uint64> before = StorePageHashes(stores[0].File, levels);

    const ECS::EntityHandle dab = AddCornerPaintDab(terrain);
    terrain.World.SwapLifecycleEvents();
    for (uint32 frame = 0; frame < 2u * TerrainSurfacePageDriver::kSurfaceSettleFrames; ++frame)
    {
        // One dab a frame: the stroke's paint grows.
        terrain.World.GetComponentForWrite<Components::TerrainPaintLayerEffect>(dab)->Strength =
            0.4f + 0.02f * static_cast<float32>(frame);
        const uint64 version = tiled.SurfaceVersion;
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        ASSERT_GT(tiled.SurfaceVersion, version) << "frame " << frame << " published no dab";
        ASSERT_EQ(terrain.System.SurfacePages().PendingPages(TiledHandleOf(terrain)), 0u) << "frame " << frame;
    }
    EXPECT_EQ(StorePageHashes(stores[0].File, levels), before) << "a dab baked the store before the settle";

    for (uint32 frame = 1; frame < TerrainSurfacePageDriver::kSurfaceSettleFrames; ++frame)
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
    EXPECT_EQ(StorePageHashes(stores[0].File, levels), before) << "the stroke baked before its settle";
    ASSERT_TRUE(SettleSurfacePages(terrain, FramesToSettle(levels.front())));
    EXPECT_EQ(SceneSurfaceStores(folder.Path, scene), stores) << "a stroke patches the stores in place";
    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    JobSystem::WorkStealingThreadPool pool(4);
    TerrainSurfacePageBaker fresh(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                  SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                  GeneratedHeightPages(provider, tiled), &pool);
    const std::filesystem::path full = folder.Path.parent_path() / "ge-surface-pages-stroke-settle-full.gepage";
    ASSERT_EQ(BakeWholeStore(fresh, tiled, full), "");
    const std::vector<uint64> painted = StorePageHashes(stores[0].File, levels);
    EXPECT_EQ(painted, StorePageHashes(full, levels));
    EXPECT_NE(painted, before);
    RemoveSurfaceStore(full);
}

// Holds every worker of a pool of `workers` workers busy until Release: a job submitted meanwhile
// cannot start. The holds are Background jobs, so they queue after a surface job submitted before
// them, and the constructor returns when that job has ended.
class BusyPool
{
public:
    BusyPool(JobSystem::WorkStealingThreadPool& pool, std::size_t workers)
    {
        for (std::size_t worker = 0; worker < workers; ++worker)
            m_Tasks.push_back(pool.Submit([this] { HoldWorker(); }, JobSystem::JobPriority::Background));
        while (m_Started.load() < workers)
            std::this_thread::yield();
    }
    ~BusyPool() { Release(); }
    BusyPool(const BusyPool&) = delete;
    BusyPool& operator=(const BusyPool&) = delete;

    // Frees the workers; callable from any thread.
    void Unblock() { m_Released.store(true); }

    void Release()
    {
        m_Released.store(true);
        for (JobSystem::TaskHandle& task : m_Tasks)
            task.Wait();
        m_Tasks.clear();
    }

private:
    void HoldWorker()
    {
        ++m_Started;
        while (!m_Released.load())
            std::this_thread::yield();
    }

    std::vector<JobSystem::TaskHandle> m_Tasks;
    std::atomic<std::size_t> m_Started{0};
    std::atomic<bool> m_Released{false};
};

// Updates `terrain`, whose world runs the surface jobs on a pool, until its driver has no level-0
// page to bake and no job, at most 120 s. False when pages stay.
bool DrainSurfacePages(SplatStoreTerrain& terrain)
{
    JobSystem::WorkStealingThreadPool& pool = *terrain.World.GetJobSystem();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (uint32 frame = 1; std::chrono::steady_clock::now() < deadline; ++frame)
    {
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        if (frame > TerrainSurfacePageDriver::kSurfaceSettleFrames &&
            terrain.System.SurfacePages().PendingPages(TiledHandleOf(terrain)) == 0u)
        {
            // A job with no level-0 page, such as the job that opens the stores, can still run. It
            // ends before the holds start, and the next frame takes the pages it queues.
            {
                BusyPool flush(pool, pool.GetWorkerCount());
            }
            terrain.System.Update(terrain.World, 1.0f / 60.0f);
            if (terrain.System.SurfacePages().PendingPages(TiledHandleOf(terrain)) == 0u)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// Updates `terrain` until its driver has `pages` level-0 pages to bake, at most twice the settle.
// False when it never has.
bool UpdateUntilPending(SplatStoreTerrain& terrain, std::size_t pages)
{
    for (uint32 frame = 0; frame < 2u * TerrainSurfacePageDriver::kSurfaceSettleFrames; ++frame)
    {
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        if (terrain.System.SurfacePages().PendingPages(TiledHandleOf(terrain)) == pages)
            return true;
    }
    return false;
}

// The settle of a stroke starts a job, and the frame thread bakes no page. Two more settles while
// that job cannot run queue the stroke's pages once: when the job ends, one job bakes them from the
// newest inputs, and the store is the full bake of the last stroke.
TEST(TerrainRegionBake, SettlesWhileASurfaceJobRunsCoalesceIntoOneJobOfTheNewestInputs)
{
    constexpr std::size_t kWorkers = 2;
    JobSystem::WorkStealingThreadPool pool(kWorkers);
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    terrain.World.SetJobSystem(&pool);
    ScopedStoreFolder folder("coalesce");
    const GUID scene = GUID::Generate();
    GiveSurfaceStores(terrain, folder.Path, scene);
    ASSERT_TRUE(DrainSurfacePages(terrain));
    const std::vector<SceneSurfaceStore> stores = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(stores.size(), 2u);
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    const std::vector<uint64> before = StorePageHashes(stores[0].File, levels);

    const ECS::EntityHandle dab = AddCornerPaintDab(terrain);
    terrain.World.SwapLifecycleEvents();
    BusyPool busy(pool, kWorkers);
    terrain.System.Update(terrain.World, 1.0f / 60.0f);
    const TerrainSurfacePageBaker reader(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                         SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                         SurfaceHeightPageFetch{}, nullptr);
    const std::size_t pages = reader.Level0PagesReading(tiled.SurfaceChanged).size();
    ASSERT_GT(pages, 0u);
    ASSERT_LE(pages, TerrainSurfacePageDriver::kLevel0PagesAJob);
    ASSERT_TRUE(UpdateUntilPending(terrain, pages)) << "the settle started no job, or baked on the frame thread";
    EXPECT_EQ(StorePageHashes(stores[0].File, levels), before) << "the frame thread baked the stroke";

    for (const float32 strength : {0.5f, 0.3f})
    {
        terrain.World.GetComponentForWrite<Components::TerrainPaintLayerEffect>(dab)->Strength = strength;
        ASSERT_TRUE(UpdateUntilPending(terrain, 2u * pages)) << "strength " << strength;
        for (uint32 frame = 0; frame < TerrainSurfacePageDriver::kSurfaceSettleFrames; ++frame)
            terrain.System.Update(terrain.World, 1.0f / 60.0f);
        EXPECT_EQ(terrain.System.SurfacePages().PendingPages(TiledHandleOf(terrain)), 2u * pages)
            << "a settle while the job runs queued its pages twice, or started a second job";
    }
    busy.Release();
    ASSERT_TRUE(DrainSurfacePages(terrain));

    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    TerrainSurfacePageBaker fresh(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                  SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                  GeneratedHeightPages(provider, tiled), &pool);
    const std::filesystem::path full = folder.Path.parent_path() / "ge-surface-pages-coalesce-full.gepage";
    ASSERT_EQ(BakeWholeStore(fresh, tiled, full), "");
    EXPECT_EQ(StorePageHashes(stores[0].File, levels), StorePageHashes(full, levels))
        << "the store is not the bake of the newest stroke";
    RemoveSurfaceStore(full);
}

// A run of the cold open keeps the stores open between its jobs. When the terrain closes, a job
// publishes the stores off the frame thread; the splat store holds the run's pages as the full bake
// has them, and no temporary file stays.
TEST(TerrainRegionBake, AStoreThatARunHoldsOpenIsPublishedWhenItsTerrainCloses)
{
    constexpr std::size_t kWorkers = 2;
    JobSystem::WorkStealingThreadPool pool(kWorkers);
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    ScopedStoreFolder folder("close");
    const GUID scene = GUID::Generate();
    const TiledTerrainHandle handle = TiledHandleOf(terrain);
    GiveSurfaceStores(terrain, folder.Path, scene);
    const std::vector<SceneSurfaceStore> stores = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(stores.size(), 2u);
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    const std::size_t pages = static_cast<std::size_t>(levels.front().PagesX) * levels.front().PagesZ;
    ASSERT_GT(pages, 2u * TerrainSurfacePageDriver::kLevel0PagesAJob);

    terrain.World.SetJobSystem(&pool);
    terrain.System.Update(terrain.World, 1.0f / 60.0f);
    {
        // The first job of the run is queued before the holds, so it has ended when the constructor returns.
        BusyPool busy(pool, kWorkers);
        EXPECT_FALSE(std::filesystem::exists(stores[0].File)) << "the first job of the run published the store";
        terrain.World.DestroyEntityImmediate(terrain.Entity);
        terrain.World.SwapLifecycleEvents();
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        EXPECT_FALSE(std::filesystem::exists(stores[0].File)) << "the frame thread published the splat store";
        EXPECT_FALSE(std::filesystem::exists(stores[1].File)) << "the frame thread published the normal store";
    }
    {
        // The close job is queued before the holds, so it has ended when the constructor returns.
        BusyPool flush(pool, kWorkers);
    }
    ASSERT_TRUE(std::filesystem::exists(stores[0].File)) << "the closed terrain left its splat store open";
    ASSERT_TRUE(std::filesystem::exists(stores[1].File)) << "the closed terrain left its normal store open";
    for (uint32 frame = 0; frame < 3u; ++frame)
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
    EXPECT_EQ(terrain.System.SurfacePages().PendingPages(handle), 0u) << "the driver still holds the terrain";

    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    TerrainSurfacePageBaker fresh(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                  SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                  GeneratedHeightPages(provider, tiled), &pool);
    const std::filesystem::path full = folder.Path.parent_path() / "ge-surface-pages-close-full.gepage";
    ASSERT_EQ(BakeWholeStore(fresh, tiled, full), "");
    const std::vector<uint64> closed = StorePageHashes(stores[0].File, levels);
    const std::vector<uint64> whole = StorePageHashes(full, levels);
    std::size_t baked = 0;
    for (std::size_t page = 0; page < pages; ++page)
    {
        if (closed[page] == 0u)
            continue;
        ++baked;
        EXPECT_EQ(closed[page], whole[page]) << "level-0 page " << page;
    }
    EXPECT_EQ(baked, TerrainSurfacePageDriver::kLevel0PagesAJob) << "the published store lost the run's pages";
    std::size_t temporaries = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder.Path))
        if (entry.path().extension() == ".tmp")
            ++temporaries;
    EXPECT_EQ(temporaries, 0u) << "a temporary store file stayed";
    RemoveSurfaceStore(full);
}

// A job bakes from the snapshot of its settle. When the live rules change before the job runs, the
// store holds the bake of the old rules.
TEST(TerrainRegionBake, ASurfaceJobBakesFromItsSnapshotWhenTheLiveRulesChange)
{
    constexpr std::size_t kWorkers = 2;
    JobSystem::WorkStealingThreadPool pool(kWorkers);
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    terrain.World.SetJobSystem(&pool);
    ScopedStoreFolder folder("snapshot-rules");
    const GUID scene = GUID::Generate();
    GiveSurfaceStores(terrain, folder.Path, scene);
    ASSERT_TRUE(DrainSurfacePages(terrain));
    const std::vector<SceneSurfaceStore> stores = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(stores.size(), 2u);
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    const std::filesystem::path full = folder.Path.parent_path() / "ge-surface-pages-snapshot-rules-full.gepage";

    AddCornerPaintDab(terrain);
    terrain.World.SwapLifecycleEvents();
    BusyPool busy(pool, kWorkers);
    terrain.System.Update(terrain.World, 1.0f / 60.0f);
    std::vector<uint64> firstRules;
    {
        TerrainSurfacePageBaker fresh(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                      SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                      GeneratedHeightPages(provider, tiled), nullptr);
        ASSERT_EQ(BakeWholeStore(fresh, tiled, full), "");
        firstRules = StorePageHashes(full, levels);
    }
    const TerrainSurfacePageBaker reader(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                         SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                         SurfaceHeightPageFetch{}, nullptr);
    const std::size_t pages = reader.Level0PagesReading(tiled.SurfaceChanged).size();
    ASSERT_GT(pages, 0u);
    ASSERT_TRUE(UpdateUntilPending(terrain, pages)) << "the settle started no job";

    terrain.World.GetComponentForWrite<Components::TerrainSurfaceRulesEffect>(terrain.Rules)->Rules[0].Conditions[0].Max =
        0.4f;
    for (uint32 frame = 0; frame < 3u * TerrainSurfacePageDriver::kSurfaceSettleFrames; ++frame)
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
    std::vector<uint64> secondRules;
    {
        TerrainSurfacePageBaker fresh(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                      SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                      GeneratedHeightPages(provider, tiled), nullptr);
        ASSERT_EQ(BakeWholeStore(fresh, tiled, full), "");
        secondRules = StorePageHashes(full, levels);
    }
    ASSERT_NE(firstRules, secondRules) << "the rules change left the splat unchanged";

    busy.Release();
    // The queued job is before the holds, so it has run when the constructor returns; the job of the
    // rules change waits.
    BusyPool after(pool, kWorkers);
    terrain.System.Update(terrain.World, 1.0f / 60.0f);
    ASSERT_TRUE(std::filesystem::exists(stores[0].File)) << "the job did not publish the store";
    EXPECT_EQ(StorePageHashes(stores[0].File, levels), firstRules) << "the job read the live rules, not its snapshot";
    after.Release();
    ASSERT_TRUE(DrainSurfacePages(terrain));
    RemoveSurfaceStore(full);
}

// The frame thread never waits for a surface job. With every worker held, the frames of a cold open
// and a settle return and make no store; once the workers are free, the jobs make the splat and
// normal stores as the full bakes of the painted terrain.
TEST(TerrainRegionBake, TheFrameThreadNeverWaitsForASurfaceJob)
{
    constexpr std::size_t kWorkers = 2;
    JobSystem::WorkStealingThreadPool pool(kWorkers);
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    terrain.World.SetJobSystem(&pool);
    ScopedStoreFolder folder("never-waits");
    const GUID scene = GUID::Generate();

    // A frame that waited for the job would wait until the watchdog frees the workers.
    constexpr auto kWatchdog = std::chrono::seconds(60);
    BusyPool busy(pool, kWorkers);
    std::promise<void> done;
    const std::future<void> finished = done.get_future();
    JobSystem::WorkStealingThreadPool watchdogPool(1);
    JobSystem::TaskHandle watchdog = watchdogPool.Submit([&busy, &finished, kWatchdog] {
        if (finished.wait_for(kWatchdog) == std::future_status::timeout)
            busy.Unblock();
    });
    const auto start = std::chrono::steady_clock::now();
    GiveSurfaceStores(terrain, folder.Path, scene);
    AddCornerPaintDab(terrain);
    terrain.World.SwapLifecycleEvents();
    for (uint32 frame = 0; frame < 3u * TerrainSurfacePageDriver::kSurfaceSettleFrames; ++frame)
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
    const bool storesBeforeRelease = !SceneSurfaceStores(folder.Path, scene).empty();
    const TerrainSurfacePageBaker reader(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                         SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                         SurfaceHeightPageFetch{}, nullptr);
    const std::size_t stroked = reader.Level0PagesReading(tiled.SurfaceChanged).size();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    done.set_value();
    watchdog.Wait();
    EXPECT_LT(elapsed, kWatchdog) << "a frame waited for a surface job";
    EXPECT_FALSE(storesBeforeRelease) << "the frame thread made the stores";
    EXPECT_EQ(terrain.System.SurfacePages().PendingPages(TiledHandleOf(terrain)), stroked)
        << "the stroke's pages are not queued behind the open job";

    busy.Release();
    ASSERT_TRUE(DrainSurfacePages(terrain));
    const std::vector<SceneSurfaceStore> stores = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(stores.size(), 2u);
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    for (const SceneSurfaceStore& store : stores)
    {
        TerrainSurfacePageBaker fresh(terrain.System.SnapshotSurfaceBake(tiled, {}), store.Plane,
                                      SurfaceAccess(tiled, store.Plane), GeneratedHeightPages(provider, tiled), &pool);
        const std::filesystem::path full = folder.Path.parent_path() / "ge-surface-pages-never-waits-full.gepage";
        ASSERT_EQ(BakeWholeStore(fresh, tiled, full), "");
        EXPECT_EQ(StorePageHashes(store.File, levels, store.Plane), StorePageHashes(full, levels, store.Plane))
            << SurfacePlaneName(store.Plane) << " store";
        RemoveSurfaceStore(full);
    }
}

namespace
{

// The pages of `wanted` of `level` that stream `stream` of `residency` holds.
std::size_t LoadedPages(const PageStreaming::PageResidencyManager& residency, uint32 stream,
                        std::span<const PageStreaming::PageWant> wanted, uint32 level)
{
    std::size_t loaded = 0;
    for (const PageStreaming::PageWant& want : wanted)
        if (want.Address.Level == level && residency.Bytes(stream, want.Address) != nullptr)
            ++loaded;
    return loaded;
}

// Every page of `levels`, nearest first by page column and row.
std::vector<PageStreaming::PageWant> EverySurfacePage(std::span<const PageStreaming::PageStoreLevel> levels)
{
    std::vector<PageStreaming::PageWant> wanted;
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
                wanted.push_back({PageStreaming::PageAddress{0, static_cast<uint8>(level), x, z},
                                  static_cast<float32>(x + z), false});
    return wanted;
}

// Updates `residency` until stream `stream` holds `count` pages of `level` of `wanted`, at most 30 s.
void UpdateUntilLoaded(PageStreaming::PageResidencyManager& residency, uint64& frame, uint32 stream,
                       std::span<const PageStreaming::PageWant> wanted, uint32 level, std::size_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (LoadedPages(residency, stream, wanted, level) < count && std::chrono::steady_clock::now() < deadline)
    {
        residency.Update(++frame, 1.0f / 60.0f, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// The loaded bytes of page `address` of the store at `file`.
std::vector<uint8> PublishedPayload(const std::filesystem::path& file, const PageStreaming::PageAddress& address)
{
    PageStreaming::PageStoreReader store;
    EXPECT_EQ(store.Open(file, PageStreaming::PageFieldKind::Splat), "");
    std::vector<uint8> stored;
    std::vector<uint8> payload;
    EXPECT_TRUE(store.ReadPageBytes(address, stored) && store.DecodePayload(address, stored, payload));
    return payload;
}

// Page `address` of `access`, read on a worker of `pool`.
PageStreaming::PageFetchOutcome FetchOnWorker(JobSystem::WorkStealingThreadPool& pool,
                                              const PageStreaming::PageStoreAccess& access,
                                              const PageStreaming::PageAddress& address)
{
    PageStreaming::PageFetchOutcome outcome;
    pool.Submit([&] { outcome = access.Fetch(address); }, JobSystem::JobPriority::Background).Wait();
    return outcome;
}

} // namespace

// A stream over the splat store of a terrain reads the level-0 pages of a run's first job before the
// run ends. After the run, a stream configured before it reads every page of the published store
// through the same store access. No stream is configured again.
TEST(TerrainRegionBake, ASurfacePageBakedMidRunIsReadByItsStreamBeforeTheRunEndsWithNoConfigure)
{
    constexpr std::size_t kWorkers = 2;
    JobSystem::WorkStealingThreadPool pool(kWorkers);
    JobSystem::WorkStealingThreadPool readers(1);
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    ScopedStoreFolder folder("live-read");
    const GUID scene = GUID::Generate();
    const TiledTerrainHandle handle = TiledHandleOf(terrain);
    GiveSurfaceStores(terrain, folder.Path, scene);
    const std::shared_ptr<const PageStreaming::PageStoreAccess> access =
        terrain.System.SurfacePages().SurfaceStore(handle, PageStreaming::PageFieldKind::Splat);
    ASSERT_NE(access, nullptr);
    const std::vector<SceneSurfaceStore> stores = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(stores.size(), 2u);
    const std::filesystem::path splat =
        stores[0].Plane == PageStreaming::PageFieldKind::Splat ? stores[0].File : stores[1].File;
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    const std::vector<PageStreaming::PageWant> wanted = EverySurfacePage(levels);
    const std::size_t level0 = static_cast<std::size_t>(levels.front().PagesX) * levels.front().PagesZ;
    ASSERT_GT(level0, 2u * TerrainSurfacePageDriver::kLevel0PagesAJob);

    PageStreaming::PageResidencyBudget budget;
    budget.CpuBytes = 1ull << 30;
    PageStreaming::PageResidencyManager residency(&readers, nullptr, budget);
    residency.ConfigureField(0, 2u * static_cast<uint32>(wanted.size()), 2, 0.0f);
    residency.Configure(0, 0, PageStreaming::PageSource{nullptr, nullptr, access});
    residency.Configure(1, 0, PageStreaming::PageSource{nullptr, nullptr, access});
    residency.Request(0, wanted);
    residency.Request(1, {});
    uint64 frame = 0;
    const uint32 top = static_cast<uint32>(levels.size()) - 1u;
    const std::size_t topPages = static_cast<std::size_t>(levels[top].PagesX) * levels[top].PagesZ;
    UpdateUntilLoaded(residency, frame, 0, wanted, top, topPages);
    ASSERT_EQ(LoadedPages(residency, 0, wanted, top), topPages)
        << "the published pinned levels did not load";
    ASSERT_EQ(LoadedPages(residency, 0, wanted, 0u), 0u) << "a level-0 page loaded before the run";

    terrain.World.SetJobSystem(&pool);
    terrain.System.Update(terrain.World, 1.0f / 60.0f);
    {
        // The run's first job is queued before the holds, so it has ended when the constructor
        // returns; the holds keep the run from going on.
        BusyPool busy(pool, kWorkers);
        UpdateUntilLoaded(residency, frame, 0, wanted, 0u, TerrainSurfacePageDriver::kLevel0PagesAJob);
        EXPECT_EQ(LoadedPages(residency, 0, wanted, 0u), TerrainSurfacePageDriver::kLevel0PagesAJob)
            << "the first job's pages did not load while the run was open";
        EXPECT_GT(terrain.System.SurfacePages().PendingPages(handle), 0u) << "the run ended";
        EXPECT_FALSE(std::filesystem::exists(splat)) << "the run published the store";
    }
    ASSERT_TRUE(DrainSurfacePages(terrain));
    ASSERT_TRUE(std::filesystem::exists(splat));
    EXPECT_EQ(terrain.System.SurfacePages().SurfaceStore(handle, PageStreaming::PageFieldKind::Splat), access)
        << "the run gave the plane another store access";
    UpdateUntilLoaded(residency, frame, 0, wanted, 0u, level0);
    ASSERT_EQ(LoadedPages(residency, 0, wanted, 0u), level0) << "a page baked in the run did not load";
    for (const PageStreaming::PageWant& want : wanted)
        if (want.Address.Level == 0u)
            EXPECT_EQ(*residency.Bytes(0, want.Address), PublishedPayload(splat, want.Address))
                << "level-0 page " << want.Address.X << "," << want.Address.Z;

    residency.Request(1, wanted);
    UpdateUntilLoaded(residency, frame, 1, wanted, 0u, level0);
    for (const PageStreaming::PageWant& want : wanted)
    {
        const std::vector<uint8>* bytes = residency.Bytes(1, want.Address);
        ASSERT_NE(bytes, nullptr);
        EXPECT_EQ(*bytes, PublishedPayload(splat, want.Address))
            << "level " << int(want.Address.Level) << " page " << want.Address.X << "," << want.Address.Z;
    }
    readers.Shutdown();
}

// The driver goes away while a job of a run waits for a worker and a stream reads the store. The
// stream's store access stays readable, the job bakes and publishes its pages, and the stream reads them.
TEST(TerrainRegionBake, ASurfaceStoreAccessOutlivesItsDriverAndReadsTheRunItsLastJobPublishes)
{
    constexpr std::size_t kWorkers = 2;
    JobSystem::WorkStealingThreadPool pool(kWorkers);
    JobSystem::WorkStealingThreadPool readers(1);
    auto terrain = std::make_unique<SplatStoreTerrain>(3u, 3u);
    ASSERT_NE(terrain->Tiled, nullptr);
    ScopedStoreFolder folder("live-read-outlives");
    const GUID scene = GUID::Generate();
    const TiledTerrainHandle handle = TiledHandleOf(*terrain);
    GiveSurfaceStores(*terrain, folder.Path, scene);
    const std::shared_ptr<const PageStreaming::PageStoreAccess> access =
        terrain->System.SurfacePages().SurfaceStore(handle, PageStreaming::PageFieldKind::Splat);
    ASSERT_NE(access, nullptr);
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(*terrain->Tiled);
    const std::vector<PageStreaming::PageWant> wanted = EverySurfacePage(levels);
    const std::vector<SceneSurfaceStore> stores = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(stores.size(), 2u);
    const std::filesystem::path splat =
        stores[0].Plane == PageStreaming::PageFieldKind::Splat ? stores[0].File : stores[1].File;

    PageStreaming::PageResidencyBudget budget;
    budget.CpuBytes = 1ull << 30;
    PageStreaming::PageResidencyManager residency(&readers, nullptr, budget);
    residency.ConfigureField(0, static_cast<uint32>(wanted.size()), 2, 0.0f);
    residency.Configure(0, 0, PageStreaming::PageSource{nullptr, nullptr, access});
    residency.Request(0, wanted);
    uint64 frame = 0;

    terrain->World.SetJobSystem(&pool);
    terrain->System.Update(terrain->World, 1.0f / 60.0f);
    {
        BusyPool first(pool, kWorkers); // the run's first job has ended
    }
    {
        BusyPool busy(pool, kWorkers);
        terrain->System.Update(terrain->World, 1.0f / 60.0f); // takes the first job, queues the second
        ASSERT_GT(terrain->System.SurfacePages().PendingPages(handle), 0u);
        residency.Update(++frame, 1.0f / 60.0f, false); // reads start
        terrain.reset();
        residency.Update(++frame, 1.0f / 60.0f, false);
    }
    {
        BusyPool flush(pool, kWorkers); // the second job has run and let its stores go
    }
    // The job lets its stores go once it has run, which can be just after the flush.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(splat) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(std::filesystem::exists(splat)) << "the job of the driver that went away did not publish the store";
    UpdateUntilLoaded(residency, frame, 0, wanted, 0u, 2u * TerrainSurfacePageDriver::kLevel0PagesAJob);
    EXPECT_EQ(LoadedPages(residency, 0, wanted, 0u), 2u * TerrainSurfacePageDriver::kLevel0PagesAJob)
        << "the stream did not read the pages of the run's two jobs";
    for (const PageStreaming::PageWant& want : wanted)
        if (const std::vector<uint8>* bytes = want.Address.Level == 0u ? residency.Bytes(0, want.Address) : nullptr)
            EXPECT_EQ(*bytes, PublishedPayload(splat, want.Address))
                << "level-0 page " << want.Address.X << "," << want.Address.Z;
    std::size_t temporaries = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder.Path))
        if (entry.path().extension() == ".tmp")
            ++temporaries;
    EXPECT_EQ(temporaries, 0u) << "a temporary store file stayed";
    readers.Shutdown();
}

// A plane that loses its store (the ground write turned off: the Color store's key is 0) closes its
// store access before its file is deleted. A read of the access on a worker finds every page absent,
// with no error, and the driver gives the plane no access.
TEST(TerrainRegionBake, ASurfacePlaneThatLosesItsStoreClosesItsAccessBeforeTheFileIsDeleted)
{
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    ScopedStoreFolder folder("lost-plane");
    const GUID scene = GUID::Generate();
    const TiledTerrainHandle handle = TiledHandleOf(terrain);
    Components::TerrainGrass grass{};
    grass.GroundDarkening = 0.6f;
    terrain.World.AddComponentImmediate<Components::TerrainGrass>(terrain.Entity, grass);
    terrain.World.SwapLifecycleEvents();
    GiveSurfaceStores(terrain, folder.Path, scene);
    const std::shared_ptr<const PageStreaming::PageStoreAccess> access =
        terrain.System.SurfacePages().SurfaceStore(handle, PageStreaming::PageFieldKind::Color);
    ASSERT_NE(access, nullptr);
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(*terrain.Tiled);
    const PageStreaming::PageAddress top{0, static_cast<uint8>(levels.size() - 1u), 0, 0};
    JobSystem::WorkStealingThreadPool readers(1);
    ASSERT_NE(FetchOnWorker(readers, *access, top).Bytes, nullptr) << "the Color store's pinned page did not read";

    // The run publishes the Color store into its access, then the open job deletes the store.
    terrain.World.GetComponentForWrite<Components::TerrainGrass>(terrain.Entity)->GroundDarkening = 0.0f;
    for (uint32 frame = 0; frame <= 2u * TerrainSurfacePageDriver::kSurfaceSettleFrames &&
                           terrain.System.SurfacePages().SurfaceStore(handle, PageStreaming::PageFieldKind::Color);
         ++frame)
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
    ASSERT_EQ(terrain.System.SurfacePages().SurfaceStore(handle, PageStreaming::PageFieldKind::Color), nullptr);
    const std::vector<SceneSurfaceStore> stores = SceneSurfaceStores(folder.Path, scene);
    EXPECT_EQ(stores.size(), 2u);
    EXPECT_TRUE(std::none_of(stores.begin(), stores.end(), [](const SceneSurfaceStore& store) {
        return store.Plane == PageStreaming::PageFieldKind::Color;
    })) << "the Color store stayed on disk";
    const PageStreaming::PageFetchOutcome outcome = FetchOnWorker(readers, *access, top);
    EXPECT_TRUE(outcome.Absent) << "the access of the deleted Color store read a page";
    EXPECT_EQ(outcome.Bytes, nullptr);
    EXPECT_TRUE(outcome.Error.empty()) << outcome.Error;
    readers.Shutdown();
}

// A stroke that settles while the job that opens kept stores waits for a worker bakes after the
// open: the open does not drop the pages the stroke queued. The stroke comes after the frame that
// starts the open, so the stores keep the key they were opened under.
TEST(TerrainRegionBake, AStrokeThatSettlesWhileKeptStoresOpenBakesAfterTheOpen)
{
    constexpr std::size_t kWorkers = 2;
    JobSystem::WorkStealingThreadPool pool(kWorkers);
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    terrain.World.SetJobSystem(&pool);
    ScopedStoreFolder folder("stroke-during-open");
    const GUID scene = GUID::Generate();
    GiveSurfaceStores(terrain, folder.Path, scene);
    ASSERT_TRUE(DrainSurfacePages(terrain));
    const std::vector<SceneSurfaceStore> stores = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(stores.size(), 2u);

    // The driver forgets the stores, then opens them again: they are current, so they are kept.
    TerrainService::Get().SetHeightPageStoreLocation(HeightPageStoreLocation{});
    terrain.System.Update(terrain.World, 1.0f / 60.0f);
    HeightPageStoreLocation cache;
    cache.CacheDirectory = folder.Path;
    TerrainService::Get().SetHeightPageStoreLocation(cache);
    {
        BusyPool busy(pool, kWorkers);
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        AddCornerPaintDab(terrain);
        terrain.World.SwapLifecycleEvents();
        for (uint32 frame = 0; frame < 3u * TerrainSurfacePageDriver::kSurfaceSettleFrames; ++frame)
            terrain.System.Update(terrain.World, 1.0f / 60.0f);
        ASSERT_GT(terrain.System.SurfacePages().PendingPages(TiledHandleOf(terrain)), 0u)
            << "the stroke queued no page while the stores opened";
    }
    ASSERT_TRUE(DrainSurfacePages(terrain));
    EXPECT_EQ(SceneSurfaceStores(folder.Path, scene), stores) << "the stores were not kept";

    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    const PageStreaming::GeneratedHeightPageProvider provider(TileNoiseLattice(tiled));
    TerrainSurfacePageBaker fresh(terrain.System.SnapshotSurfaceBake(tiled, {}), PageStreaming::PageFieldKind::Splat,
                                  SurfaceAccess(tiled, PageStreaming::PageFieldKind::Splat),
                                  GeneratedHeightPages(provider, tiled), &pool);
    const std::filesystem::path full = folder.Path.parent_path() / "ge-surface-pages-stroke-during-open-full.gepage";
    ASSERT_EQ(BakeWholeStore(fresh, tiled, full), "");
    EXPECT_EQ(StorePageHashes(stores[0].File, levels), StorePageHashes(full, levels))
        << "the open dropped the stroke's pages";
    RemoveSurfaceStore(full);
}

// A drag of a global rule changes the whole surface on every preview bake. The stores keep their
// names while the drag holds and through the settle frames, then are made again once.
TEST(TerrainRegionBake, ARuleDragMakesTheSurfaceStoresAgainOnceAtItsSettle)
{
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    ScopedStoreFolder folder("rule-drag");
    const GUID scene = GUID::Generate();
    GiveSurfaceStores(terrain, folder.Path, scene);
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    ASSERT_TRUE(SettleSurfacePages(terrain, FramesToSettle(levels.front())));
    const std::vector<SceneSurfaceStore> opened = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(opened.size(), 2u);

    TerrainService& service = TerrainService::Get();
    service.SetInteractiveModifierEdit(TerrainService::InteractiveEditSource::InspectorDrag, true);
    constexpr uint32 kDragFrames = 6;
    for (uint32 frame = 0; frame < kDragFrames; ++frame)
    {
        terrain.World.GetComponentForWrite<Components::TerrainSurfaceRulesEffect>(terrain.Rules)->Rules[0].Conditions[0].Max =
            0.8f - 0.02f * static_cast<float32>(frame + 1u);
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        ASSERT_EQ(SceneSurfaceStores(folder.Path, scene), opened) << "drag frame " << frame << " made the stores again";
    }
    service.SetInteractiveModifierEdit(TerrainService::InteractiveEditSource::InspectorDrag, false);
    for (uint32 frame = 0; frame + 1u < TerrainSurfacePageDriver::kSurfaceSettleFrames; ++frame)
    {
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        ASSERT_EQ(SceneSurfaceStores(folder.Path, scene), opened) << "settle frame " << frame;
    }
    // The release frame may publish the drag's last bake: the settle counts from it.
    std::vector<SceneSurfaceStore> settled = opened;
    for (uint32 frame = 0; frame < 2u && settled == opened; ++frame)
    {
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        settled = SceneSurfaceStores(folder.Path, scene);
    }
    ASSERT_EQ(settled.size(), 2u);
    EXPECT_NE(settled[0].File, opened[0].File) << "the settle did not make the splat store under the new rule";
    EXPECT_EQ(terrain.System.SurfacePages().PendingPages(TiledHandleOf(terrain)),
              static_cast<std::size_t>(levels.front().PagesX) * levels.front().PagesZ);
}

// A publish of the whole surface under an unchanged key makes the stores again at its settle, with
// every level-0 page to bake. A ground write turned on adds the Color store at its settle.
TEST(TerrainRegionBake, AWholeSurfacePublishOrAGroundWriteMakesTheSurfaceStoresAgainAtItsSettle)
{
    SplatStoreTerrain terrain(3u, 3u);
    ASSERT_NE(terrain.Tiled, nullptr);
    TiledTerrainData& tiled = *terrain.Tiled;
    ScopedStoreFolder folder("whole-surface");
    const GUID scene = GUID::Generate();
    GiveSurfaceStores(terrain, folder.Path, scene);
    const std::vector<PageStreaming::PageStoreLevel> levels = TileLatticeLevels(tiled);
    ASSERT_TRUE(SettleSurfacePages(terrain, FramesToSettle(levels.front())));
    const std::vector<SceneSurfaceStore> opened = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(opened.size(), 2u);

    const uint32 interior = tiled.Config.TileConfig.HeightmapWidth - 1u;
    tiled.SurfaceChanged = PageStreaming::PageSampleRect{0u, 0u, tiled.Config.TilesPerAxisX * interior,
                                                         tiled.Config.TilesPerAxisZ * interior};
    ++tiled.SurfaceVersion;
    std::size_t pending = 0;
    for (uint32 frame = 0; frame <= TerrainSurfacePageDriver::kSurfaceSettleFrames && pending == 0u; ++frame)
    {
        terrain.System.Update(terrain.World, 1.0f / 60.0f);
        pending = terrain.System.SurfacePages().PendingPages(TiledHandleOf(terrain));
    }
    EXPECT_EQ(SceneSurfaceStores(folder.Path, scene), opened) << "the key changed";
    EXPECT_EQ(pending, static_cast<std::size_t>(levels.front().PagesX) * levels.front().PagesZ);
    ASSERT_TRUE(SettleSurfacePages(terrain, FramesToSettle(levels.front())));

    Components::TerrainGrass grass{};
    grass.GroundDarkening = 0.6f;
    terrain.World.AddComponentImmediate<Components::TerrainGrass>(terrain.Entity, grass);
    terrain.World.SwapLifecycleEvents();
    ASSERT_TRUE(SettleSurfacePages(terrain, FramesToSettle(levels.front())));
    const std::vector<SceneSurfaceStore> written = SceneSurfaceStores(folder.Path, scene);
    ASSERT_EQ(written.size(), 3u);
    EXPECT_TRUE(std::any_of(written.begin(), written.end(), [](const SceneSurfaceStore& store) {
        return store.Plane == PageStreaming::PageFieldKind::Color;
    }));
}

// The overlay is built only for a terrain that would page (the driver sets its cap), and a terrain
// that starts to page under an unchanged modifier set (its cooked store opened) gets it at the next
// update.
TEST(TerrainRegionBake, AHeightPageOverlayIsBuiltOnlyOnceTheTerrainWouldPage)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    CreateFlattenModifier(world, kTiledTileWorld, 300.0f, 20.0f);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(tiled->PageOverlay, nullptr) << "a terrain that keeps its height texture holds no overlay";
    EXPECT_FALSE(tiled->PageOverlayCurrent);

    tiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_TRUE(tiled->PageOverlayCurrent);
    EXPECT_NE(tiled->PageOverlay, nullptr) << "the terrain pages now and its modifier reaches it";
}

// A stroke inside the overlay's rect rebakes the overlay in place: the same object, a new version
// derived from the one the pages hold, and only the edited rect changed, so an edit never copies a
// large overlay (82.9 MiB at the sample count of 1 km^2 at 0.25 m).
TEST(TerrainRegionBake, AnEditInsideTheHeightPageOverlayRebakesItInPlace)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    CreateFlattenModifier(world, kTiledTileWorld, 500.0f, 20.0f, 300.0f);
    const auto dab = CreateFlattenModifier(world, kTiledTileWorld, 500.0f, 35.0f, 20.0f);
    tiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_NE(tiled->PageOverlay, nullptr);
    const PageStreaming::HeightPageOverlay* before = tiled->PageOverlay.get();
    const PageStreaming::PageSampleRect reach = before->Level0Rect();
    const uint64 version = tiled->PageOverlayVersion;

    world.GetComponentForWrite<Components::TerrainFlattenEffect>(dab)->TargetHeight = 40.0f;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(tiled->PageOverlay.get(), before) << "an edit inside the overlay copied it";
    EXPECT_NE(tiled->PageOverlayVersion, version);
    EXPECT_EQ(tiled->PageOverlayChangedSince, version) << "the pages refresh only the edited rect";
    const PageStreaming::PageSampleRect& changed = tiled->PageOverlayChanged;
    EXPECT_LT(changed.Width(), reach.Width());
    EXPECT_LT(changed.Height(), reach.Height());
    EXPECT_NE(tiled->PageOverlay->Delta(0, changed.MinX + changed.Width() / 2u, changed.MinZ + changed.Height() / 2u),
              0.0f);
}

namespace
{
// Every sample of every level outside the reach of the reported changed rect holds the same delta in
// `before` and `after`, bit for bit: the pages may skip it.
void ExpectSameDeltasOutsideTheChangedRect(const TiledTerrainData& tiled, const PageStreaming::HeightPageOverlay& before,
                                           const PageStreaming::HeightPageOverlay& after)
{
    const uint32 samplesX = tiled.Config.TilesPerAxisX * (tiled.Config.TileConfig.HeightmapWidth - 1u) + 1u;
    const uint32 samplesZ = tiled.Config.TilesPerAxisZ * (tiled.Config.TileConfig.HeightmapHeight - 1u) + 1u;
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(samplesX, samplesZ);
    uint64 mismatched = 0;
    for (uint32 level = 0; level < levels.size(); ++level)
    {
        const PageStreaming::PageSampleRect reach =
            PageStreaming::Level0RectReach(levels, tiled.PageOverlayChanged, level);
        const PageStreaming::PageSampleRect region =
            PageStreaming::Union(PageStreaming::Level0RectReach(levels, before.Level0Rect(), level),
                                 PageStreaming::Level0RectReach(levels, after.Level0Rect(), level));
        for (uint32 z = region.MinZ; z <= region.MaxZ; ++z)
            for (uint32 x = region.MinX; x <= region.MaxX; ++x)
            {
                const float32 previous = before.Delta(level, x, z);
                const float32 current = after.Delta(level, x, z);
                if (reach.Contains(x, z) || std::memcmp(&previous, &current, sizeof(float32)) == 0)
                    continue;
                ++mismatched;
                EXPECT_LT(mismatched, 2u) << "level " << level << " sample (" << x << ", " << z << ") " << previous
                                          << " -> " << current << " outside the changed rect";
            }
    }
    EXPECT_EQ(mismatched, 0u);
}
} // namespace

// Dragging a modifier at the edge of the overlay's reach rebuilds the overlay over its new rect but
// composes only the dirty rect and the newly reached strip, keeping every other delta of the previous
// overlay (a marker written into the previous overlay away from the drag survives it), and apart
// from the marker the result equals a full rebuild. It reports only the dirty rect as changed (the
// dragged stamp's old and new bounds), not the grown reach.
TEST(TerrainRegionBake, DraggingAModifierAtTheReachsEdgeComposesOnlyWhatItMoved)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    CreateFlattenModifier(world, kTiledTileWorld, 500.0f, 20.0f, 300.0f);
    const auto edge = CreateFlattenModifier(world, 1500.0f, 500.0f, 35.0f, 20.0f);
    tiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_NE(tiled->PageOverlay, nullptr);
    // The marker: one level-0 sample at the flatten's centre, far from the dragged stamp.
    const PageStreaming::PageSampleRect before = tiled->PageOverlay->Level0Rect();
    const uint32 markX = before.MinX + 40u;
    const uint32 markZ = (before.MinZ + before.MaxZ) / 2u;
    constexpr float32 kMarker = 999.0f;
    tiled->PageOverlay->Rebake({markX, markZ, markX, markZ}, std::span<const float32>(&kMarker, 1));
    const auto previous = tiled->PageOverlay;

    world.GetComponentForWrite<Components::WorldTransform>(edge)->matrix[12] = 1520.0f;
    system.Update(world, 1.0f / 60.0f);
    const auto dragged = tiled->PageOverlay;
    ASSERT_NE(dragged, nullptr);
    const PageStreaming::PageSampleRect reach = dragged->Level0Rect();
    EXPECT_GT(reach.MaxX, before.MaxX) << "the drag grows the reach";
    EXPECT_EQ(dragged->Delta(0, markX, markZ), kMarker) << "a drag at the reach's edge recomposed the whole overlay";
    const PageStreaming::PageSampleRect& changed = tiled->PageOverlayChanged;
    EXPECT_LT(changed.Width(), reach.Width()) << "the drag reported the whole reach";
    EXPECT_LT(changed.Height(), reach.Height()) << "the drag reported the whole reach";
    ExpectSameDeltasOutsideTheChangedRect(*tiled, *previous, *dragged);

    // The same modifiers baked from scratch.
    tiled->PageOverlayCurrent = false;
    TerrainModifierSystem fresh;
    fresh.Update(world, 1.0f / 60.0f);
    ASSERT_NE(tiled->PageOverlay, nullptr);
    ASSERT_NE(tiled->PageOverlay, dragged);
    for (uint32 level = 0; level < 3; ++level)
        for (uint32 z = reach.MinZ >> level; z <= (reach.MaxZ >> level); z += 3)
            for (uint32 x = reach.MinX >> level; x <= (reach.MaxX >> level); x += 3)
            {
                // The marker and its coarser levels' tent footprint differ by design.
                const bool nearMarker = (x + 2u >= (markX >> level) && x <= (markX >> level) + 2u) &&
                                        (z + 2u >= (markZ >> level) && z <= (markZ >> level) + 2u);
                if (nearMarker)
                    continue;
                ASSERT_NEAR(dragged->Delta(level, x, z), tiled->PageOverlay->Delta(level, x, z), 1.0e-6f)
                    << "level " << level << " sample (" << x << ", " << z << ")";
            }
}

// A dab beside the overlay grows it, and the grown overlay reports only the dab's samples as changed
// (the rect the dab alone bakes), not the whole grown area: the pages rewrite the dab, not the stroke.
TEST(TerrainRegionBake, ADabThatGrowsTheHeightPageOverlayReportsOnlyTheDabsSamples)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentTiledTerrain(svc, 2);
    const auto aloneHandle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    auto* alone = svc.GetTiledTerrainData(aloneHandle);
    ASSERT_NE(tiled, nullptr);
    ASSERT_NE(alone, nullptr);
    tiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    CreateFlattenModifier(world, kTiledTileWorld, 500.0f, 35.0f, 20.0f);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_NE(tiled->PageOverlay, nullptr);
    const PageStreaming::PageSampleRect first = tiled->PageOverlay->Level0Rect();
    const uint64 version = tiled->PageOverlayVersion;

    CreateFlattenModifier(world, kTiledTileWorld + 40.0f, 500.0f, 35.0f, 20.0f);
    system.Update(world, 1.0f / 60.0f);
    ASSERT_NE(tiled->PageOverlay, nullptr);
    const PageStreaming::PageSampleRect grown = tiled->PageOverlay->Level0Rect();
    ASSERT_GT(grown.MaxX, first.MaxX) << "the second dab grows the overlay";
    EXPECT_EQ(tiled->PageOverlayChangedSince, version);

    // The second dab alone, on a terrain of its own.
    ECS::World aloneWorld;
    RegisterModifierLifecycleEvents(aloneWorld);
    CreateTiledTerrainEntity(aloneWorld, aloneHandle, 2);
    CreateFlattenModifier(aloneWorld, kTiledTileWorld + 40.0f, 500.0f, 35.0f, 20.0f);
    alone->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;
    TerrainModifierSystem aloneSystem;
    aloneSystem.Update(aloneWorld, 1.0f / 60.0f);
    ASSERT_NE(alone->PageOverlay, nullptr);
    const PageStreaming::PageSampleRect dab = alone->PageOverlay->Level0Rect();

    const PageStreaming::PageSampleRect& changed = tiled->PageOverlayChanged;
    EXPECT_EQ(changed.MinX, dab.MinX);
    EXPECT_EQ(changed.MinZ, dab.MinZ);
    EXPECT_EQ(changed.MaxX, dab.MaxX);
    EXPECT_EQ(changed.MaxZ, dab.MaxZ);
    EXPECT_LT(changed.Width(), grown.Width()) << "the grown overlay reported its whole area";
}

// Deleting a modifier at the overlay's edge reports a rect that covers the deleted modifier's old
// bounds, and its samples return to the base.
TEST(TerrainRegionBake, DeletingAModifierAtTheHeightPageOverlaysEdgeReportsItsOldBounds)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    tiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    CreateFlattenModifier(world, kTiledTileWorld, 500.0f, 35.0f, 20.0f);
    const auto edge = CreateFlattenModifier(world, kTiledTileWorld + 40.0f, 500.0f, 35.0f, 20.0f);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_NE(tiled->PageOverlay, nullptr);
    const PageStreaming::PageSampleRect old = tiled->PageOverlay->Level0Rect();
    // Each stamp (20 m radius) spans 59 level-0 samples: the first one starts at the overlay's MinX,
    // the edge one ends at its MaxX.
    constexpr uint32 kEdgeStampSamples = 59u;
    const uint32 centerX = old.MaxX - 20u;
    const uint32 centerZ = (old.MinZ + old.MaxZ) / 2u;
    EXPECT_NE(tiled->PageOverlay->Delta(0, centerX, centerZ), 0.0f);

    world.DestroyEntityImmediate(edge);
    world.SwapLifecycleEvents();
    system.Update(world, 1.0f / 60.0f);
    ASSERT_NE(tiled->PageOverlay, nullptr);
    const PageStreaming::PageSampleRect& changed = tiled->PageOverlayChanged;
    EXPECT_GE(changed.MaxX, old.MaxX) << "the reported rect misses the deleted modifier's old bounds";
    EXPECT_LE(changed.MinX, old.MaxX - (kEdgeStampSamples - 1u));
    EXPECT_LE(changed.MinZ, centerZ);
    EXPECT_GE(changed.MaxZ, centerZ);
    for (uint32 x = old.MinX + kEdgeStampSamples; x <= old.MaxX; ++x)
        EXPECT_EQ(tiled->PageOverlay->Delta(0, x, centerZ), 0.0f) << "sample " << x << " kept the deleted delta";
}

// Deleting the last height modifier releases the overlay and reports its whole area as changed: the
// pages return to the base under all of it.
TEST(TerrainRegionBake, DeletingTheLastHeightModifierReportsTheReleasedOverlaysWholeArea)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    tiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    const auto only = CreateFlattenModifier(world, kTiledTileWorld, 500.0f, 35.0f, 20.0f);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_NE(tiled->PageOverlay, nullptr);
    const PageStreaming::PageSampleRect old = tiled->PageOverlay->Level0Rect();
    const uint64 version = tiled->PageOverlayVersion;

    world.DestroyEntityImmediate(only);
    world.SwapLifecycleEvents();
    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(tiled->PageOverlay, nullptr);
    EXPECT_EQ(tiled->PageOverlayChangedSince, version);
    const PageStreaming::PageSampleRect& changed = tiled->PageOverlayChanged;
    EXPECT_EQ(changed.MinX, old.MinX);
    EXPECT_EQ(changed.MinZ, old.MinZ);
    EXPECT_EQ(changed.MaxX, old.MaxX);
    EXPECT_EQ(changed.MaxZ, old.MaxZ);
}

// An overlay rebuilt from scratch (its modifiers' data is no longer current) reports its whole area
// as changed: the pages hold no overlay it derives from.
TEST(TerrainRegionBake, AHeightPageOverlayRebuiltFromScratchReportsItsWholeArea)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    tiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    CreateFlattenModifier(world, kTiledTileWorld, 500.0f, 35.0f, 20.0f);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_NE(tiled->PageOverlay, nullptr);
    const PageStreaming::PageSampleRect first = tiled->PageOverlay->Level0Rect();

    CreateFlattenModifier(world, kTiledTileWorld + 40.0f, 500.0f, 35.0f, 20.0f);
    tiled->PageOverlayCurrent = false;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_NE(tiled->PageOverlay, nullptr);
    const PageStreaming::PageSampleRect grown = tiled->PageOverlay->Level0Rect();
    const PageStreaming::PageSampleRect& changed = tiled->PageOverlayChanged;
    EXPECT_EQ(changed.MinX, std::min(first.MinX, grown.MinX));
    EXPECT_EQ(changed.MinZ, std::min(first.MinZ, grown.MinZ));
    EXPECT_EQ(changed.MaxX, std::max(first.MaxX, grown.MaxX));
    EXPECT_EQ(changed.MaxZ, std::max(first.MaxZ, grown.MaxZ));
}

// Two terrains' data never publish the same overlay version: a scene load rebuilds a terrain's data
// while its pages stay resident, and the pages tell the rebuilt data's overlay from the one they
// hold by its version (TerrainHeightPages rewrites every page either overlay reaches).
TEST(TerrainRegionBake, NoTwoTerrainsDataPublishTheSameHeightPageOverlayVersion)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto first = CreateResidentTiledTerrain(svc, 2);
    const auto second = CreateResidentTiledTerrain(svc, 2);
    auto* firstTiled = svc.GetTiledTerrainData(first);
    auto* secondTiled = svc.GetTiledTerrainData(second);
    ASSERT_NE(firstTiled, nullptr);
    ASSERT_NE(secondTiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, first, 2);
    CreateTiledTerrainEntity(world, second, 2);
    CreateFlattenModifier(world, kTiledTileWorld, 300.0f, 20.0f);
    firstTiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;
    secondTiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_NE(firstTiled->PageOverlay, nullptr);
    ASSERT_NE(secondTiled->PageOverlay, nullptr);
    EXPECT_NE(firstTiled->PageOverlayVersion, secondTiled->PageOverlayVersion);
    EXPECT_EQ(secondTiled->PageOverlayChangedSince, 0u) << "its first overlay changes the unmodified base";
    EXPECT_NE(secondTiled->PageOverlayChangedSince, firstTiled->PageOverlayVersion);
}

// A terrain whose cooked store is off its tile lattice (PageOverlayOffGrid) builds no overlay once a
// height modifier reaches it: it keeps its texture, is refused (the driver logs OverlayGridRefusal)
// and stops waiting, so the modifier system does not rebake it every frame. With no modifier
// reaching it, it pages.
TEST(TerrainRegionBake, AHeightPageOverlayOffTheStoresGridIsRefused)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    tiled->PageOverlayByteCap = kDesktopHeightPages.OverlayByteCap;
    tiled->PageOverlayOffGrid = true;
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_TRUE(tiled->PageOverlayCurrent) << "no modifier reaches it: it pages on its own grid";
    EXPECT_FALSE(tiled->PageOverlayRefusedOffGrid);

    CreateFlattenModifier(world, kTiledTileWorld, 300.0f, 20.0f);
    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(tiled->PageOverlay, nullptr);
    EXPECT_FALSE(tiled->PageOverlayCurrent) << "a modifier reaches it: it keeps its height texture";
    EXPECT_TRUE(tiled->PageOverlayRefusedOffGrid);
    EXPECT_FALSE(tiled->AwaitsPageOverlay()) << "a refused terrain is not rebaked every frame";
}

// A modifier whose overlay is over the cap: nothing is built, the terrain keeps its texture
// (PageOverlayCurrent stays false, so TerrainHeightIsPaged says no) and the refused bytes are
// what the driver's OverlayRefusal names.
TEST(TerrainRegionBake, AHeightPageOverlayOverTheCapIsRefused)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const auto handle = CreateResidentTiledTerrain(svc, 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    CreateFlattenModifier(world, 0.0f, 0.0f, 20.0f, 4.0f * kTiledTileWorld);
    const uint32 samplesX = tiled->Config.TilesPerAxisX * (tiled->Config.TileConfig.HeightmapWidth - 1u) + 1u;
    const uint32 samplesZ = tiled->Config.TilesPerAxisZ * (tiled->Config.TileConfig.HeightmapHeight - 1u) + 1u;
    const uint64 wholeTerrain = PageStreaming::HeightPageOverlay::Bytes(
        PageStreaming::BuildPageStoreLevels(samplesX, samplesZ), {0, 0, samplesX - 1u, samplesZ - 1u});
    tiled->PageOverlayByteCap = wholeTerrain - 1u;
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(tiled->PageOverlay, nullptr);
    EXPECT_FALSE(tiled->PageOverlayCurrent) << "a refused terrain must not page without its modifiers";
    EXPECT_EQ(tiled->PageOverlayRefusedBytes, wholeTerrain) << "the modifier reaches every sample";
}

// ============================================================================
// A tiled terrain starts from its Terrain.baseSource (#2389)
// ============================================================================

namespace
{
// A 2 x 1 tiled terrain (1024 m tiles of 1025^2 at 1 sample/m) whose corner is off the
// world origin, so a base fill that ignored the corner would sample the wrong place.
constexpr float32 kBaseTerrainOriginX = -1024.0f;
constexpr float32 kBaseTerrainOriginZ = -512.0f;

TiledTerrainConfig MakeBaseTestTiledConfig(TiledTerrainBase base)
{
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 2.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    cfg.Base = std::move(base);
    return cfg;
}

// The oracle: the base FillHeightfieldBaseRegion gives an UNTILED terrain of the same
// footprint at 1 sample/m (2049 x 1025), the ground the issue expects a tiled terrain
// to show. Its sample (i, j) sits i, j metres from the corner.
Terrain::HeightfieldData MakeUntiledBaseOracle(const TiledTerrainConfig& cfg)
{
    const uint32 w = static_cast<uint32>(cfg.WorldSizeX) + 1;
    const uint32 h = static_cast<uint32>(cfg.WorldSizeZ) + 1;
    Terrain::HeightfieldData oracle(w, h);
    FillHeightfieldBaseRegion(oracle, cfg.Base.Source, cfg.Base.Heightmap.get(), 0, 0,
                              static_cast<int32>(w) - 1, static_cast<int32>(h) - 1);
    return oracle;
}

// Compare every `stride`-th sample of a tile's heightfield (coarse or full) with the
// untiled oracle at the same metre, plus the tile's last row and column (the seam a
// neighbour shares). The tile's samples sit on whole metres of the oracle's lattice
// (1 m at full detail, 8 m coarse). A sample past the footprint (a tile grid that
// overhangs it) is compared with the oracle's nearest edge sample: the heightmap's edge.
// Returns the number of samples compared.
uint32 ExpectTileMatchesUntiledBase(const TerrainTileData& tile, const TiledTerrainData& tiled,
                                    const Terrain::HeightfieldData& oracle, uint32 stride,
                                    const char* label)
{
    const uint32 w = tile.Heightfield.GetWidth();
    const uint32 h = tile.Heightfield.GetHeight();
    const uint32 metresPerSampleX = static_cast<uint32>(tiled.Config.TileWorldSize) / (w - 1);
    const uint32 metresPerSampleZ = static_cast<uint32>(tiled.Config.TileWorldSize) / (h - 1);
    const uint32 tileMetreX = static_cast<uint32>(tile.WorldOriginX - tiled.WorldOriginX);
    const uint32 tileMetreZ = static_cast<uint32>(tile.WorldOriginZ - tiled.WorldOriginZ);
    uint32 compared = 0;
    for (uint32 z = 0; z < h; ++z)
    {
        if (z % stride != 0 && z != h - 1)
            continue;
        for (uint32 x = 0; x < w; ++x)
        {
            if (x % stride != 0 && x != w - 1)
                continue;
            const float32 expected =
                oracle.GetSample(std::min(tileMetreX + x * metresPerSampleX, oracle.GetWidth() - 1),
                                 std::min(tileMetreZ + z * metresPerSampleZ, oracle.GetHeight() - 1));
            EXPECT_NEAR(tile.Heightfield.GetSample(x, z), expected, 1.0e-5f)
                << label << ": tile (" << tile.Coord.X << "," << tile.Coord.Z << ") sample (" << x
                << "," << z << ") is not the untiled base at that position";
            ++compared;
        }
    }
    return compared;
}

// A 33 x 33 decoded heightmap with relief in both axes (the same source the untiled
// heightmap-base tests use), seeded into the service's decode cache.
std::shared_ptr<const Terrain::HeightfieldData> SeedNoiseHeightmap(TerrainService& svc,
                                                                    const GUID& guid)
{
    Terrain::HeightfieldData source(33, 33);
    source.FillWithNoise(3.0f, 1.0f, 4, 99);
    svc.SeedDecodedHeightmapForTests(guid, source);
    return svc.ResolveHeightmapAsset(guid);
}
} // namespace

// THE ISSUE (#2389): past 1025 samples a side a terrain tiles, and every tile used to
// start from the procedural tile noise whatever Terrain.baseSource said. Each authored
// base is checked through both production producers of a resident tile — the load fill
// and the modifier bake (BakeTiledFull -> ComposeTileHeights) — against what an untiled
// terrain of the same footprint starts from. Missing heightmap: flat, as untiled.
//
// The flatten sits inside tile (1,0) only; the oracle comparison runs over tile (0,0),
// which it never reaches, and over the seam column tile (1,0) shares with it. The
// positive control proves the bake ran (the flatten is in tile (1,0)).
TEST(TerrainRegionBake, TiledTerrainStartsFromItsAuthoredBase)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);

    struct BaseCase
    {
        const char* Label;
        TiledTerrainBase Base;
    };
    const BaseCase cases[] = {
        {"heightmap",
         svc.ResolveTiledTerrainBase(Components::TerrainBaseSource::HeightmapAsset, heightmapGuid)},
        {"missing heightmap",
         svc.ResolveTiledTerrainBase(Components::TerrainBaseSource::HeightmapAsset, GUID::Null())},
        {"flat", svc.ResolveTiledTerrainBase(Components::TerrainBaseSource::Flat, heightmapGuid)},
    };

    for (const BaseCase& baseCase : cases)
    {
        const TiledTerrainConfig cfg = MakeBaseTestTiledConfig(baseCase.Base);
        const TiledTerrainHandle handle = svc.CreateTiledTerrain(cfg);
        auto* tiled = svc.GetTiledTerrainData(handle);
        ASSERT_NE(tiled, nullptr);
        tiled->WorldOriginX = kBaseTerrainOriginX;
        tiled->WorldOriginZ = kBaseTerrainOriginZ;
        for (int32 x = 0; x < 2; ++x)
            svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

        const Terrain::HeightfieldData oracle = MakeUntiledBaseOracle(cfg);
        std::string label = std::string(baseCase.Label) + ", loaded";
        EXPECT_GT(ExpectTileMatchesUntiledBase(*Tile(tiled, 0, 0), *tiled, oracle, 37, label.c_str()),
                  700u);
        ExpectTileMatchesUntiledBase(*Tile(tiled, 1, 0), *tiled, oracle, 37, label.c_str());

        // The modifier bake re-fills every Full tile from the base before the stack.
        ECS::World world;
        RegisterModifierLifecycleEvents(world);
        const auto terrainEntity = CreateTiledTerrainEntity(world, handle, 2);
        auto* terrain = world.GetComponentForWrite<Components::Terrain>(terrainEntity);
        terrain->BaseSource = baseCase.Base.Source;
        terrain->TerrainAssetGuid.Set(heightmapGuid);
        // The entity's transform is the terrain's centre; the corner is what extraction
        // derives from it, kept in step with tiled->WorldOrigin above.
        auto* terrainXf = world.GetComponentForWrite<Components::WorldTransform>(terrainEntity);
        terrainXf->matrix[12] = kBaseTerrainOriginX + kTiledTileWorld;
        terrainXf->matrix[14] = kBaseTerrainOriginZ + 0.5f * kTiledTileWorld;
        const float32 flattenX = kBaseTerrainOriginX + 1.5f * kTiledTileWorld;
        const float32 flattenZ = kBaseTerrainOriginZ + 0.5f * kTiledTileWorld;
        CreateFlattenModifier(world, flattenX, flattenZ, 20.0f);
        TerrainModifierSystem system;
        system.Update(world, 1.0f / 60.0f);

        label = std::string(baseCase.Label) + ", baked";
        ExpectTileMatchesUntiledBase(*Tile(tiled, 0, 0), *tiled, oracle, 37, label.c_str());
        const TerrainTileData& tile10 = *Tile(tiled, 1, 0);
        for (uint32 z = 0; z < tile10.Heightfield.GetHeight(); z += 37)
            EXPECT_NEAR(tile10.Heightfield.GetSample(0, z),
                        Tile(tiled, 0, 0)->Heightfield.GetSample(static_cast<uint32>(kTiledTileWorld), z),
                        1.0e-6f)
                << label << ": tiles (0,0) and (1,0) disagree on their shared edge at z " << z;

        // Positive control: the bake ran and applied the flatten inside tile (1,0).
        const float32 flattened =
            tile10.Heightfield.GetSample(static_cast<uint32>(0.5f * kTiledTileWorld),
                                         static_cast<uint32>(0.5f * kTiledTileWorld));
        EXPECT_NEAR(flattened, 20.0f / kTiledHeightScale, 1.0e-4f) << label;

        svc.DestroyTiledTerrain(handle);
    }
}

// The streamed producer: TileStreamingManager's coarse and full jobs fill each tile on a
// worker, and a tile no modifier reaches keeps that fill as its final ground (no bake ever
// re-fills it), so a modifier-free tiled terrain shows exactly what the jobs wrote. Both
// stages are compared: a tile is Coarse for at least one Update between its coarse
// integration and its full one, which integrates on a later Update.
TEST(TerrainRegionBake, StreamedTilesStartFromTheHeightmapBase)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);

    const TiledTerrainConfig cfg = MakeBaseTestTiledConfig(
        svc.ResolveTiledTerrainBase(Components::TerrainBaseSource::HeightmapAsset, heightmapGuid));
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    tiled->WorldOriginX = kBaseTerrainOriginX;
    tiled->WorldOriginZ = kBaseTerrainOriginZ;
    const Terrain::HeightfieldData oracle = MakeUntiledBaseOracle(cfg);

    JobSystem::WorkStealingThreadPool pool(4);
    TileStreamingManager streaming(&pool);
    const std::vector<Mathematics::Vector3> cameras{
        {kBaseTerrainOriginX + kTiledTileWorld, 50.0f, kBaseTerrainOriginZ + 0.5f * kTiledTileWorld}};

    std::array<bool, 2> sawCoarse{};
    std::array<bool, 2> sawFull{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!(sawFull[0] && sawFull[1]) && std::chrono::steady_clock::now() < deadline)
    {
        streaming.Update(handle, *tiled, svc, cameras, Mathematics::Vector3{0.0f, 0.0f, 1.0f},
                         4.0f * kTiledTileWorld, 1.0f / 60.0f);
        for (int32 x = 0; x < 2; ++x)
        {
            const auto it = tiled->Tiles.find(TileCoord{x, 0});
            if (it == tiled->Tiles.end() || !it->second)
                continue;
            const TerrainTileData& tile = *it->second;
            if (tile.LodState == TileLodState::Coarse && !sawCoarse[x])
            {
                sawCoarse[x] = true;
                ExpectTileMatchesUntiledBase(tile, *tiled, oracle, 1, "coarse job");
            }
            if (tile.LodState == TileLodState::Full && !sawFull[x])
            {
                sawFull[x] = true;
                ExpectTileMatchesUntiledBase(tile, *tiled, oracle, 37, "full job");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(sawCoarse[0] && sawCoarse[1]) << "a tile skipped its coarse stage";
    ASSERT_TRUE(sawFull[0] && sawFull[1]) << "the tiles did not stream in within the deadline";
}

// A Coarse tile arrives with its full-resolution stand-in (the upsampled heights and their normal)
// built on the streaming worker, current for the heightfield it integrated with, so the frame that
// patches it copies texels: computing it there costs about 16 ms per tile on the frame thread
// (#3409). The stand-in is byte-equal to one built from the integrated heightfield.
TEST(TerrainRegionBake, ACoarseArrivalCarriesItsStandInFromTheStreamingJob)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);
    const TiledTerrainConfig cfg = MakeBaseTestTiledConfig(
        svc.ResolveTiledTerrainBase(Components::TerrainBaseSource::HeightmapAsset, heightmapGuid));
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    tiled->WorldOriginX = kBaseTerrainOriginX;
    tiled->WorldOriginZ = kBaseTerrainOriginZ;

    JobSystem::WorkStealingThreadPool pool(4);
    TileStreamingManager streaming(&pool);
    const std::vector<Mathematics::Vector3> cameras{
        {kBaseTerrainOriginX + kTiledTileWorld, 50.0f, kBaseTerrainOriginZ + 0.5f * kTiledTileWorld}};
    const uint32 tileRes = tiled->Config.TileConfig.HeightmapWidth;
    const float32 tileWorld = tiled->Config.TileWorldSize;
    const float32 heightScale = tiled->Config.HeightScale;

    const TerrainTileData* coarseTile = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!coarseTile && std::chrono::steady_clock::now() < deadline)
    {
        streaming.Update(handle, *tiled, svc, cameras, Mathematics::Vector3{0.0f, 0.0f, 1.0f},
                         4.0f * kTiledTileWorld, 1.0f / 60.0f);
        for (const auto& [coord, tile] : tiled->Tiles)
            if (tile && tile->LodState == TileLodState::Coarse)
                coarseTile = tile.get();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_NE(coarseTile, nullptr) << "no tile reached its coarse stage within the deadline";
    ASSERT_NE(coarseTile->CoarseStandIn, nullptr) << "the coarse job built no stand-in";
    const CoarseTileStandIn& standIn = *coarseTile->CoarseStandIn;
    EXPECT_TRUE(standIn.Matches(tileRes, tileWorld, heightScale, coarseTile->HeightfieldVersion))
        << "the extraction would rebuild the stand-in on the frame thread";

    CoarseTileStandIn rebuilt;
    BuildCoarseTileStandIn(coarseTile->Heightfield, tileRes, tileWorld, heightScale, rebuilt);
    ASSERT_EQ(standIn.Heights.GetSampleCount(), rebuilt.Heights.GetSampleCount());
    EXPECT_EQ(0, std::memcmp(standIn.Heights.GetRawSamples(), rebuilt.Heights.GetRawSamples(),
                             rebuilt.Heights.GetSampleCount() * sizeof(float32)));
    EXPECT_EQ(standIn.Normalmap, rebuilt.Normalmap);
}

namespace
{
// One tiled terrain streamed by a manager shared with another, and the ground it must show.
struct StreamedTiledTerrain
{
    const char* Label = nullptr;
    TiledTerrainHandle Handle;
    TiledTerrainData* Tiled = nullptr;
    Terrain::HeightfieldData Oracle;
};

// A 2 x 1 tiled terrain at the base-test corner whose ground is `source`.
StreamedTiledTerrain ProvisionStreamedTerrain(TerrainService& svc, const char* label,
                                              Components::TerrainBaseSource source,
                                              const GUID& heightmapGuid)
{
    const TiledTerrainConfig cfg = MakeBaseTestTiledConfig(svc.ResolveTiledTerrainBase(source, heightmapGuid));
    StreamedTiledTerrain terrain;
    terrain.Label = label;
    terrain.Handle = svc.CreateTiledTerrain(cfg);
    terrain.Tiled = svc.GetTiledTerrainData(terrain.Handle);
    if (terrain.Tiled)
    {
        terrain.Tiled->WorldOriginX = kBaseTerrainOriginX;
        terrain.Tiled->WorldOriginZ = kBaseTerrainOriginZ;
    }
    terrain.Oracle = MakeUntiledBaseOracle(cfg);
    return terrain;
}

bool AllTilesFull(const TiledTerrainData& tiled)
{
    for (uint32 z = 0; z < tiled.Config.TilesPerAxisZ; ++z)
    {
        for (uint32 x = 0; x < tiled.Config.TilesPerAxisX; ++x)
        {
            const auto it = tiled.Tiles.find(TileCoord{static_cast<int32>(x), static_cast<int32>(z)});
            if (it == tiled.Tiles.end() || !it->second || it->second->LodState != TileLodState::Full)
                return false;
        }
    }
    return true;
}

bool AllTerrainsFull(std::span<const StreamedTiledTerrain> terrains)
{
    for (const StreamedTiledTerrain& terrain : terrains)
    {
        if (!AllTilesFull(*terrain.Tiled))
            return false;
    }
    return true;
}

bool AnyTileCoarse(const TiledTerrainData& tiled)
{
    for (const auto& [coord, tile] : tiled.Tiles)
    {
        if (tile && tile->LodState == TileLodState::Coarse)
            return true;
    }
    return false;
}

// One frame of the extraction system's call pattern: one Update per terrain on the shared
// manager, then every resident tile of that terrain is compared with its own ground.
void StreamOneFrame(TileStreamingManager& streaming, TerrainService& svc,
                    std::span<const StreamedTiledTerrain> terrains)
{
    const std::vector<Mathematics::Vector3> cameras{
        {kBaseTerrainOriginX + kTiledTileWorld, 50.0f, kBaseTerrainOriginZ + 0.5f * kTiledTileWorld}};
    for (const StreamedTiledTerrain& terrain : terrains)
    {
        streaming.Update(terrain.Handle, *terrain.Tiled, svc, cameras,
                         Mathematics::Vector3{0.0f, 0.0f, 1.0f}, 4.0f * kTiledTileWorld, 1.0f / 60.0f);
        for (const auto& [coord, tile] : terrain.Tiled->Tiles)
        {
            if (tile && tile->LodState == TileLodState::Coarse)
                ExpectTileMatchesUntiledBase(*tile, *terrain.Tiled, terrain.Oracle, 16, terrain.Label);
            else if (tile && tile->LodState == TileLodState::Full)
                ExpectTileMatchesUntiledBase(*tile, *terrain.Tiled, terrain.Oracle, 128, terrain.Label);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

// Streams every terrain until all are Full, then checks each Full tile carries the full job's
// normal map. Stops at the first frame that showed a terrain the wrong ground.
void StreamUntilFullAndExpectOwnGround(TileStreamingManager& streaming, TerrainService& svc,
                                       std::span<const StreamedTiledTerrain> terrains)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!AllTerrainsFull(terrains) && std::chrono::steady_clock::now() < deadline &&
           !::testing::Test::HasFailure())
        StreamOneFrame(streaming, svc, terrains);
    ASSERT_FALSE(::testing::Test::HasFailure()) << "a terrain showed another terrain's ground";

    for (const StreamedTiledTerrain& terrain : terrains)
    {
        ASSERT_TRUE(AllTilesFull(*terrain.Tiled))
            << terrain.Label << ": the tiles did not stream in within the deadline";
        for (const auto& [coord, tile] : terrain.Tiled->Tiles)
        {
            const uint32 w = tile->Heightfield.GetWidth();
            const uint32 h = tile->Heightfield.GetHeight();
            EXPECT_EQ(tile->NormalmapWidth, w) << terrain.Label;
            EXPECT_EQ(tile->NormalmapHeight, h) << terrain.Label;
            EXPECT_EQ(tile->Normalmap.size(), static_cast<size_t>(w) * h * 4)
                << terrain.Label << ": tile (" << coord.X << "," << coord.Z << ") has no full normal map";
        }
    }
}
} // namespace

// #3012: a scene with two tiled terrains (a near field inside an outer terrain) crashed the
// editor seconds after load: two full jobs wrote one tile result, one freeing the normal map
// the other was filling. The extraction system drives ONE streaming manager for every tiled
// terrain, once per terrain per frame; this is that call pattern, with terrains of different
// ground. At every Update each terrain's resident tiles hold its own ground (a result
// streamed for the other terrain never lands in it), and both reach Full detail with the
// full job's normal map.
TEST(TerrainRegionBake, OneStreamingManagerStreamsTwoTiledTerrains)
{
    using Components::TerrainBaseSource;
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);

    const std::array<StreamedTiledTerrain, 2> terrains{
        ProvisionStreamedTerrain(svc, "heightmap terrain", TerrainBaseSource::HeightmapAsset, heightmapGuid),
        ProvisionStreamedTerrain(svc, "flat terrain", TerrainBaseSource::Flat, heightmapGuid)};
    ASSERT_NE(terrains[0].Tiled, nullptr);
    ASSERT_NE(terrains[1].Tiled, nullptr);

    JobSystem::WorkStealingThreadPool pool(4);
    TileStreamingManager streaming(&pool);
    StreamUntilFullAndExpectOwnGround(streaming, svc, terrains);
}

namespace
{
// Streams `terrains` until terrains[0] has a Coarse tile: the frame its full job was dispatched,
// so that job is still running. Returns false if it never got there.
bool StreamUntilAFullJobIsInFlight(TileStreamingManager& streaming, TerrainService& svc,
                                   std::span<const StreamedTiledTerrain> terrains)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!AnyTileCoarse(*terrains[0].Tiled) && std::chrono::steady_clock::now() < deadline &&
           !::testing::Test::HasFailure())
        StreamOneFrame(streaming, svc, terrains);
    return AnyTileCoarse(*terrains[0].Tiled);
}

// Re-provisions terrains[0] while its full job runs: the slot takes a flat terrain (other ground
// at the same tile coordinates), optionally after Forget, as the extraction system does. Then
// both terrains must stream to Full on their own ground.
void ReuseTheSlotWhileItsJobsRun(bool forgetFirst)
{
    using Components::TerrainBaseSource;
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);

    std::array<StreamedTiledTerrain, 2> terrains{
        ProvisionStreamedTerrain(svc, "retired terrain", TerrainBaseSource::HeightmapAsset, heightmapGuid),
        ProvisionStreamedTerrain(svc, "flat terrain", TerrainBaseSource::Flat, heightmapGuid)};
    ASSERT_NE(terrains[0].Tiled, nullptr);
    ASSERT_NE(terrains[1].Tiled, nullptr);

    JobSystem::WorkStealingThreadPool pool(4);
    TileStreamingManager streaming(&pool);
    ASSERT_TRUE(StreamUntilAFullJobIsInFlight(streaming, svc, terrains))
        << "the terrain never reached its full stage";

    const uint32 slot = terrains[0].Handle.Index;
    if (forgetFirst)
        streaming.Forget(slot);
    svc.DestroyTiledTerrain(terrains[0].Handle);
    terrains[0] = ProvisionStreamedTerrain(svc, "the slot's next terrain", TerrainBaseSource::Flat, heightmapGuid);
    ASSERT_NE(terrains[0].Tiled, nullptr);
    ASSERT_EQ(terrains[0].Handle.Index, slot) << "the slot was not reused, so the test proves nothing";

    StreamUntilFullAndExpectOwnGround(streaming, svc, terrains);
}

// Occupies the pool's only worker until `gate` is released, and returns once it is running, so
// the next jobs submitted stay queued.
void HoldTheOnlyWorker(JobSystem::WorkStealingThreadPool& pool, std::shared_future<void> gate)
{
    auto running = std::make_shared<std::atomic<bool>>(false);
    pool.Submit([gate, running]() {
        running->store(true);
        gate.wait();
    });
    while (!running->load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
} // namespace

// Forgetting a terrain whose jobs are still running (its re-provision or teardown, through
// ForgetTiledTerrain): each job owns the result it writes, so it finishes into memory nothing
// else holds; none of its output reaches the terrain that next takes the slot; and the other
// terrain streams on undisturbed.
TEST(TerrainRegionBake, AForgottenTerrainsRunningJobsNeverReachTheSlotsNextTerrain)
{
    ReuseTheSlotWhileItsJobsRun(true);
}

// The same reuse with no Forget (a teardown the extraction system has not seen yet): the new
// generation in the slot alone starts it from fresh streaming state, so the old terrain's
// running jobs still never reach it.
TEST(TerrainRegionBake, ANewTerrainInARecycledSlotNeverSeesTheOldTerrainsJobs)
{
    ReuseTheSlotWhileItsJobsRun(false);
}

// Forget drops every tile the terrain was streaming, including the ones whose jobs still run.
TEST(TerrainRegionBake, ForgetDropsEveryTileTheTerrainWasStreaming)
{
    using Components::TerrainBaseSource;
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);

    const std::array<StreamedTiledTerrain, 1> terrains{
        ProvisionStreamedTerrain(svc, "forgotten terrain", TerrainBaseSource::HeightmapAsset, heightmapGuid)};
    ASSERT_NE(terrains[0].Tiled, nullptr);

    JobSystem::WorkStealingThreadPool pool(4);
    TileStreamingManager streaming(&pool);
    ASSERT_TRUE(StreamUntilAFullJobIsInFlight(streaming, svc, terrains))
        << "the terrain never reached its full stage";

    streaming.Forget(terrains[0].Handle.Index);
    const TileStreamingManager::Stats stats = streaming.GetStats();
    EXPECT_EQ(stats.TilesActive + stats.TilesLoading + stats.TilesPending, 0u)
        << "Forget left the terrain's streaming state behind";
}

// One terrain, the #3012 double writer without a second terrain: a tile's coarse result is
// delivered, the tile unloads before it integrates, and the tile is requested again with a
// new job that has not run yet. The old result must not integrate into the new request (it
// would also dispatch the full job onto the Result the new coarse job is about to write).
// One worker held by a gate keeps the new jobs queued, and an integration budget of 0 lets
// exactly one result integrate per Update, so the order is deterministic.
TEST(TerrainRegionBake, AStaleCoarseResultNeverIntegratesIntoARerequestedTile)
{
    using Components::TerrainBaseSource;
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);
    const StreamedTiledTerrain terrain =
        ProvisionStreamedTerrain(svc, "terrain", TerrainBaseSource::HeightmapAsset, heightmapGuid);
    ASSERT_NE(terrain.Tiled, nullptr);

    JobSystem::WorkStealingThreadPool pool(1);
    StreamingConfig config;
    config.IntegrationBudgetMs = 0.0f;
    config.IntegrationBudgetMsTeleport = 0.0f;
    config.TeleportThreshold = 1.0e9f; // the far camera below is a move, not a teleport
    TileStreamingManager streaming(&pool, config);

    const float32 radius = 2.0f * kTiledTileWorld;
    const std::vector<Mathematics::Vector3> nearCamera{
        {kBaseTerrainOriginX + kTiledTileWorld, 50.0f, kBaseTerrainOriginZ + 0.5f * kTiledTileWorld}};
    const std::vector<Mathematics::Vector3> farCamera{
        {kBaseTerrainOriginX + 9.0f * kTiledTileWorld, 50.0f, kBaseTerrainOriginZ + 0.5f * kTiledTileWorld}};
    const Mathematics::Vector3 forward{0.0f, 0.0f, 1.0f};
    constexpr float32 kDeltaTime = 1.0f / 60.0f;

    // Frame 1, near: both tiles request their coarse jobs; once released, both deliver.
    std::promise<void> firstGate;
    HoldTheOnlyWorker(pool, firstGate.get_future().share());
    EXPECT_TRUE(streaming.Update(terrain.Handle, *terrain.Tiled, svc, nearCamera, forward, radius, kDeltaTime).empty())
        << "frame 1 integrated before any job ran";
    firstGate.set_value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (pool.GetPendingTasksApprox() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(pool.GetPendingTasksApprox(), 0u) << "the coarse jobs did not finish";

    // Frame 2, far: one result integrates (its full job queues behind the gate), then both
    // tiles unload; the other coarse result stays delivered and unintegrated.
    std::promise<void> secondGate;
    HoldTheOnlyWorker(pool, secondGate.get_future().share());
    EXPECT_EQ(streaming.Update(terrain.Handle, *terrain.Tiled, svc, farCamera, forward, radius, kDeltaTime).size(), 1u)
        << "frame 2 should integrate exactly one coarse result (integration budget 0)";

    // Frame 3, near: both tiles are requested again with new jobs that cannot run yet.
    const std::vector<TileCoord>& integrated =
        streaming.Update(terrain.Handle, *terrain.Tiled, svc, nearCamera, forward, radius, kDeltaTime);
    EXPECT_TRUE(integrated.empty())
        << "frame 3 integrated " << integrated.size()
        << " stale result(s) into a re-requested tile whose own job has not run";
    secondGate.set_value();
}

// The tile grid rounds up to whole tiles, so a footprint that does not divide into them
// leaves the last tile column and row past its edge: 1500 x 1100 m on 1024 m tiles puts
// 548 m of tile column 1 past the +X edge and 948 m of tile row 1 past the +Z edge (the
// lakeside study is 576 m on 1024 m tiles at 2 samples/m). The heightmap covers only the
// footprint, so past it every sample takes the heightmap's edge, the footprint position
// clamped to it. Unclamped, SampleBilinear reads past the heightmap's last column and row.
TEST(TerrainRegionBake, TheTileGridOverhangTakesTheHeightmapsEdge)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);

    TiledTerrainConfig cfg = MakeBaseTestTiledConfig(
        svc.ResolveTiledTerrainBase(Components::TerrainBaseSource::HeightmapAsset, heightmapGuid));
    cfg.WorldSizeX = 1500.0f;
    cfg.WorldSizeZ = 1100.0f;
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    ASSERT_EQ(tiled->Config.TileWorldSize, kTiledTileWorld);
    ASSERT_EQ(tiled->Config.TilesPerAxisX, 2u);
    ASSERT_EQ(tiled->Config.TilesPerAxisZ, 2u);
    tiled->WorldOriginX = kBaseTerrainOriginX;
    tiled->WorldOriginZ = kBaseTerrainOriginZ;

    // The oracle spans the footprint only (1501 x 1101). Tile column 1 (1024 m to 2048 m
    // in X) reaches 548 m past it and tile row 1 (1024 m to 2048 m in Z) 948 m past it, so
    // most samples of tiles (1, 0), (0, 1) and (1, 1) are overhang, including the last
    // column of tile column 1 and the last row of tile row 1, which
    // ExpectTileMatchesUntiledBase always compares.
    const Terrain::HeightfieldData oracle = MakeUntiledBaseOracle(cfg);
    for (int32 tz = 0; tz < 2; ++tz)
    {
        for (int32 tx = 0; tx < 2; ++tx)
        {
            const TerrainTileData* tile = svc.LoadTile(handle, TileCoord{tx, tz});
            ASSERT_NE(tile, nullptr);
            ExpectTileMatchesUntiledBase(*tile, *tiled, oracle, 37, "tile grid overhang");
        }
    }
    svc.DestroyTiledTerrain(handle);
}

// The extraction system keeps a tiled terrain's base until IsTiledTerrainBaseCurrent says
// the authored base has moved on, then re-provisions the terrain from a fresh
// ResolveTiledTerrainBase. It asks every frame, so the answer comes from lookups, and it
// must change exactly when the ground a tile starts from would: a new source, another
// heightmap asset, or a hot reload of the heightmap (an eviction bumps its content version).
TEST(TerrainRegionBake, ATiledBaseGoesStaleOnlyWhenTheAuthoredBaseChanges)
{
    using Components::TerrainBaseSource;
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    const GUID otherHeightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);
    ASSERT_NE(SeedNoiseHeightmap(svc, otherHeightmapGuid), nullptr);

    const TiledTerrainBase base =
        svc.ResolveTiledTerrainBase(TerrainBaseSource::HeightmapAsset, heightmapGuid);
    ASSERT_NE(base.Heightmap, nullptr);
    EXPECT_EQ(base.Heightmap, svc.ResolveHeightmapAsset(heightmapGuid))
        << "the base does not share the decode cache's heightmap";
    EXPECT_TRUE(svc.IsTiledTerrainBaseCurrent(base, TerrainBaseSource::HeightmapAsset, heightmapGuid))
        << "an unchanged base reads as stale, so the terrain would re-provision every frame";
    EXPECT_FALSE(svc.IsTiledTerrainBaseCurrent(base, TerrainBaseSource::Flat, heightmapGuid))
        << "a base source edit went unseen";
    EXPECT_FALSE(svc.IsTiledTerrainBaseCurrent(base, TerrainBaseSource::ProceduralNoise, heightmapGuid))
        << "a base source edit went unseen";
    EXPECT_FALSE(svc.IsTiledTerrainBaseCurrent(base, TerrainBaseSource::HeightmapAsset, otherHeightmapGuid))
        << "a heightmap asset swap went unseen";

    // A hot reload: the eviction makes the base stale, and the base resolved after it is
    // current and holds the new decode.
    ASSERT_TRUE(svc.EvictDecodedHeightmap(heightmapGuid));
    EXPECT_FALSE(svc.IsTiledTerrainBaseCurrent(base, TerrainBaseSource::HeightmapAsset, heightmapGuid))
        << "a hot reload of the heightmap went unseen";
    svc.SeedDecodedHeightmapForTests(heightmapGuid, Terrain::HeightfieldData(33, 33, 0.25f));
    const TiledTerrainBase reloaded =
        svc.ResolveTiledTerrainBase(TerrainBaseSource::HeightmapAsset, heightmapGuid);
    EXPECT_TRUE(svc.IsTiledTerrainBaseCurrent(reloaded, TerrainBaseSource::HeightmapAsset, heightmapGuid))
        << "the base resolved after a reload reads as stale, so the terrain would re-provision every frame";
    ASSERT_NE(reloaded.Heightmap, nullptr);
    EXPECT_FLOAT_EQ(reloaded.Heightmap->GetSample(16, 16), 0.25f);

    // A noise or flat base reads no heightmap, so neither an asset edit nor a reload
    // under it makes it stale.
    for (const TerrainBaseSource source : {TerrainBaseSource::ProceduralNoise, TerrainBaseSource::Flat})
    {
        const TiledTerrainBase other = svc.ResolveTiledTerrainBase(source, heightmapGuid);
        EXPECT_EQ(other.Heightmap, nullptr);
        EXPECT_TRUE(svc.IsTiledTerrainBaseCurrent(other, source, otherHeightmapGuid))
            << "a heightmap asset edit made a base that reads no heightmap stale";
        ASSERT_TRUE(svc.EvictDecodedHeightmap(heightmapGuid));
        EXPECT_TRUE(svc.IsTiledTerrainBaseCurrent(other, source, heightmapGuid))
            << "a heightmap reload made a base that reads no heightmap stale";
    }
}

// The extraction system's half of the fix. No suite runs TerrainExtractionSystem::Update's
// tiled path (EditorTests' TerrainMaterialLibraryEditorScene runs Update against a device, on an
// untiled scene). That path provisions every tiled terrain through BuildTiledTerrainConfig and
// decides every re-provision with CompareTiledTerrainConfig and StepTiledReprovision, and the
// three tests below hold those to the rule.
namespace
{
// The component a 2 x 1 tiled terrain (2048 x 1024 m at 1 sample/m) is authored with.
Components::Terrain MakeTiledTerrainComponent(Components::TerrainBaseSource source, const GUID& heightmapGuid)
{
    Components::Terrain terrain;
    terrain.SizeX = 2.0f * kTiledTileWorld;
    terrain.SizeZ = kTiledTileWorld;
    terrain.HeightScale = kTiledHeightScale;
    terrain.SamplesPerMeter = 1.0f;
    terrain.StreamingRadius = 1500.0f;
    terrain.BaseSource = source;
    terrain.TerrainAssetGuid.Set(heightmapGuid);
    return terrain;
}
} // namespace

// THE ISSUE (#2389) at the place it is decided: the config extraction provisions a tiled
// terrain with carries the component's base. Without it every tile of every tiled terrain
// starts from the tile noise again, whatever baseSource says, and every producer test above
// stays green because each builds its config by hand. Checked as a config (each source) and
// end to end for the heightmap: the terrain that config provisions loads its tiles from it.
TEST(TerrainRegionBake, ExtractionProvisionsATiledTerrainWithItsAuthoredBase)
{
    using Components::TerrainBaseSource;
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    const auto heightmap = SeedNoiseHeightmap(svc, heightmapGuid);
    ASSERT_NE(heightmap, nullptr);

    for (const TerrainBaseSource source :
         {TerrainBaseSource::HeightmapAsset, TerrainBaseSource::Flat, TerrainBaseSource::ProceduralNoise})
    {
        const Components::Terrain terrain = MakeTiledTerrainComponent(source, heightmapGuid);
        ASSERT_TRUE(Terrain::TerrainNeedsTiling(terrain.SizeX, terrain.SizeZ, terrain.SamplesPerMeter));
        const TiledTerrainConfig cfg = svc.BuildTiledTerrainConfig(terrain);
        EXPECT_EQ(cfg.WorldSizeX, terrain.SizeX);
        EXPECT_EQ(cfg.WorldSizeZ, terrain.SizeZ);
        EXPECT_EQ(cfg.HeightScale, terrain.HeightScale);
        EXPECT_EQ(cfg.SamplesPerMeter, terrain.SamplesPerMeter);
        EXPECT_EQ(cfg.StreamingRadius, terrain.StreamingRadius);
        EXPECT_EQ(cfg.Base.Source, source) << "the provisioned tiled terrain does not carry the authored base source";
        if (source == TerrainBaseSource::HeightmapAsset)
        {
            EXPECT_EQ(cfg.Base.Heightmap, heightmap)
                << "the provisioned tiled terrain does not hold the authored heightmap";
            EXPECT_EQ(cfg.Base.HeightmapGuid, heightmapGuid);
        }
        else
        {
            EXPECT_EQ(cfg.Base.Heightmap, nullptr);
        }
        // Provisioned and unchanged: nothing to re-provision.
        const TiledTerrainEdit edit = svc.CompareTiledTerrainConfig(cfg, terrain);
        EXPECT_FALSE(edit.ResolutionChanged || edit.SizeChanged || edit.BaseChanged)
            << "a freshly provisioned terrain already differs from its component";
    }

    const Components::Terrain terrain = MakeTiledTerrainComponent(TerrainBaseSource::HeightmapAsset, heightmapGuid);
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(svc.BuildTiledTerrainConfig(terrain));
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    ASSERT_EQ(tiled->Config.TilesPerAxisX, 2u);
    tiled->WorldOriginX = kBaseTerrainOriginX;
    tiled->WorldOriginZ = kBaseTerrainOriginZ;
    // The oracle comes from a base resolved by hand, not from the config under test.
    const Terrain::HeightfieldData oracle = MakeUntiledBaseOracle(MakeBaseTestTiledConfig(
        svc.ResolveTiledTerrainBase(TerrainBaseSource::HeightmapAsset, heightmapGuid)));
    for (int32 x = 0; x < 2; ++x)
    {
        const TerrainTileData* tile = svc.LoadTile(handle, TileCoord{x, 0});
        ASSERT_NE(tile, nullptr);
        ExpectTileMatchesUntiledBase(*tile, *tiled, oracle, 37, "provisioned from the component");
    }
    svc.DestroyTiledTerrain(handle);
}

// The re-provision rule extraction applies every tick. A base edit (source, asset, or a
// content event on the heightmap) re-provisions on the tick it is seen, with no settle
// debounce, also in the middle of a density drag; an unchanged component never does; a
// density edit still waits for kReprovisionSettleFrames unchanged ticks. Without the base
// arm, a base edit leaves every tile on the old base until something else re-provisions.
TEST(TerrainRegionBake, ABaseEditReprovisionsATiledTerrainAtOnce)
{
    using Components::TerrainBaseSource;
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    const GUID otherHeightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);
    ASSERT_NE(SeedNoiseHeightmap(svc, otherHeightmapGuid), nullptr);

    const Components::Terrain authored = MakeTiledTerrainComponent(TerrainBaseSource::HeightmapAsset, heightmapGuid);
    TiledTerrainConfig live = svc.BuildTiledTerrainConfig(authored);
    std::unordered_map<uint32, TerrainReprovisionDebounce> debounces;
    constexpr uint32 kSlot = 3;
    const auto tick = [&](const Components::Terrain& terrain) {
        return StepTiledReprovision(debounces, kSlot, svc.CompareTiledTerrainConfig(live, terrain),
                                    terrain.SamplesPerMeter, terrain.SizeX, terrain.SizeZ,
                                    kReprovisionSettleFrames);
    };

    for (uint32 i = 0; i < 2 * kReprovisionSettleFrames + 2; ++i)
        EXPECT_FALSE(tick(authored)) << "an unchanged terrain re-provisions at tick " << i;
    EXPECT_EQ(debounces.count(kSlot), 0u) << "an unchanged terrain holds debounce state";

    struct BaseEdit
    {
        const char* Label;
        Components::Terrain Terrain;
    };
    BaseEdit edits[] = {
        {"source to Flat", authored},
        {"source to ProceduralNoise", authored},
        {"another heightmap asset", authored},
    };
    edits[0].Terrain.BaseSource = TerrainBaseSource::Flat;
    edits[1].Terrain.BaseSource = TerrainBaseSource::ProceduralNoise;
    edits[2].Terrain.TerrainAssetGuid.Set(otherHeightmapGuid);
    for (const BaseEdit& edit : edits)
        EXPECT_TRUE(tick(edit.Terrain)) << edit.Label << ": the base edit did not re-provision on its first tick";

    // A content event on the heightmap (any eviction, not only a hot reload) re-provisions;
    // the terrain provisioned after it is current again.
    ASSERT_TRUE(svc.EvictDecodedHeightmap(heightmapGuid));
    EXPECT_TRUE(tick(authored)) << "a heightmap eviction did not re-provision";
    svc.SeedDecodedHeightmapForTests(heightmapGuid, Terrain::HeightfieldData(33, 33, 0.25f));
    live = svc.BuildTiledTerrainConfig(authored);
    EXPECT_FALSE(tick(authored)) << "the terrain provisioned after the eviction re-provisions again";

    // A density edit waits for the settle debounce.
    Components::Terrain denser = authored;
    denser.SamplesPerMeter = 2.0f;
    for (uint32 i = 0; i < kReprovisionSettleFrames; ++i)
        EXPECT_FALSE(tick(denser)) << "a density edit re-provisioned before it settled, tick " << i;
    EXPECT_TRUE(tick(denser)) << "a settled density edit did not re-provision";

    // A base edit in the middle of a density drag does not wait for the drag.
    debounces.clear();
    Components::Terrain dragging = denser;
    EXPECT_FALSE(tick(dragging));
    dragging.SamplesPerMeter = 1.5f;
    dragging.BaseSource = TerrainBaseSource::Flat;
    EXPECT_TRUE(tick(dragging)) << "a base edit waited for a density drag to settle";

    EXPECT_FALSE(tick(authored));
    EXPECT_EQ(debounces.count(kSlot), 0u) << "the debounce state outlived the edit";
}

// The resident-window atlas's out-of-window coarse field: a texel no streamed tile covers
// takes the coarse base field, and BuildCoarseHeightField maps texels over the tile grid's
// extent. The grid overhangs a 1500 x 1100 m footprint (2 x 2 tiles of 1024 m), so the base
// field must span the grid too, or a heightmap terrain's out-of-window relief lands at the
// wrong place. The oracle is the same coarse field built from the resident tiles, which
// LoadTile fills from the base: where a tile has not streamed, the field must show what it
// will show once it has.
TEST(TerrainRegionBake, TheAtlasCoarseBaseFieldLinesUpWithTheTiles)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);

    TiledTerrainConfig cfg = MakeBaseTestTiledConfig(
        svc.ResolveTiledTerrainBase(Components::TerrainBaseSource::HeightmapAsset, heightmapGuid));
    cfg.WorldSizeX = 1500.0f;
    cfg.WorldSizeZ = 1100.0f;
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    ASSERT_EQ(tiled->Config.TilesPerAxisX, 2u);
    ASSERT_EQ(tiled->Config.TilesPerAxisZ, 2u);
    tiled->WorldOriginX = kBaseTerrainOriginX;
    tiled->WorldOriginZ = kBaseTerrainOriginZ;
    for (int32 tz = 0; tz < 2; ++tz)
        for (int32 tx = 0; tx < 2; ++tx)
            ASSERT_NE(svc.LoadTile(handle, TileCoord{tx, tz}), nullptr);

    // 65 texels a side: every texel sits on a whole metre of the tiles' 1 m lattice.
    constexpr uint32 kDim = 65;
    constexpr float32 kFallback = 0.5f;
    std::vector<float32> fromTiles;
    BuildCoarseHeightField(
        kDim, 2, 2, kFallback,
        [tiled](int32 tx, int32 tz) -> CoarseTileSource {
            const auto& hf = tiled->Tiles.at(TileCoord{tx, tz})->Heightfield;
            return {hf.GetRawSamples(), hf.GetWidth(), hf.GetHeight()};
        },
        fromTiles);

    Terrain::HeightfieldData baseField(kDim, kDim, kFallback);
    FillAtlasCoarseBaseField(baseField, *tiled);
    std::vector<float32> fromBase;
    BuildCoarseHeightField(
        kDim, 2, 2, kFallback, [](int32, int32) -> CoarseTileSource { return {}; }, fromBase,
        baseField.GetRawSamples());

    ASSERT_EQ(fromTiles.size(), static_cast<size_t>(kDim) * kDim);
    ASSERT_EQ(fromBase.size(), fromTiles.size());
    uint32 mismatched = 0;
    float32 lo = 1e30f, hi = -1e30f;
    for (size_t i = 0; i < fromTiles.size(); ++i)
    {
        if (std::abs(fromBase[i] - fromTiles[i]) > 1.0e-5f)
            ++mismatched;
        lo = std::min(lo, fromTiles[i]);
        hi = std::max(hi, fromTiles[i]);
    }
    EXPECT_EQ(mismatched, 0u) << "of " << fromTiles.size()
                              << " coarse texels, these do not show the base the resident tiles start from";
    EXPECT_GT(hi - lo, 0.1f) << "the tiles carry no relief, so the comparison proves nothing";
    svc.DestroyTiledTerrain(handle);
}

// LIFETIME. The retained modifier stack borrows from the gather's three arenas, and
// the deferred-splat flush runs AHEAD of the change gate — so a flush that gathered
// anywhere else would clear those arenas and then return through a closed gate that
// never refilled the stack, leaving it pointing at freed spline data for the next
// compose to read. A spline-shaped volume is what makes that reachable: its resolved
// entry holds a pointer into the transformed-spline arena, where a circle or a
// rectangle holds only values.
//
// Under ASan (vs2026-x64-local-asan) this is the use-after-free repro; in an
// ordinary build the parity assertion still catches a stack that resolved elsewhere.
TEST(TerrainRegionBake, AFlushBetweenGatesLeavesTheComposedStackUsable)
{
    ScopedTerrainService scoped;
    if (SplineECS::SplineService::IsInitialized())
        SplineECS::SplineService::Shutdown();
    SplineECS::SplineService::Initialize();
    auto& svc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const auto handle = CreateResidentTiledTerrain(svc, /*tilesX*/ 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);

    // A spline-shaped volume, so the resolved stack points into the spline arena.
    const SplineECS::SplineHandle splineHandle =
        splineSvc.CreateSpline(Spline::SplineType::CatmullRom, /*closed*/ false);
    auto* splineData = splineSvc.GetSplineData(splineHandle);
    ASSERT_NE(splineData, nullptr);
    for (int i = 0; i < 4; ++i)
        splineData->AddPoint(
            Mathematics::Vector3(400.0f + 200.0f * static_cast<float32>(i), 0.0f, 500.0f), 40.0f);
    splineSvc.RebuildCache(splineHandle);

    const auto modEntity = world.CreateEntity();
    Components::SplineComponent splineComp{};
    splineComp.SplineDataIndex = splineHandle.Index();
    splineComp.SplineDataGeneration = splineHandle.Generation();
    world.AddComponentImmediate<Components::SplineComponent>(modEntity, splineComp);
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::SplinePath;
    vol.Falloff = 8.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(modEntity, vol);
    Components::TerrainFlattenEffect fx{};
    fx.UseVolumeHeight = false;
    fx.TargetHeight = 100.0f; // past HeightScale 64 — grows the global range
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(modEntity, fx);
    world.AddComponentImmediate<Components::WorldTransform>(modEntity,
                                                            Components::WorldTransform{});

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // full bake: gather fills the stack and arenas

    // Shift the range so the splat renormalize DEFERS — that is what arms the flush.
    auto* flatten = world.GetComponentForWrite<Components::TerrainFlattenEffect>(modEntity);
    ASSERT_NE(flatten, nullptr);
    flatten->TargetHeight = 240.0f;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_TRUE(tiled->SplatResplatPending) << "the deferred flush never armed";

    // Quiescent frames: the change gate is CLOSED and the flush runs anyway. This is
    // the window in which the stack and its arenas can fall out of step.
    for (int i = 0; i < 512 && tiled->SplatResplatPending; ++i)
        system.Update(world, 1.0f / 60.0f);
    EXPECT_FALSE(tiled->SplatResplatPending) << "the deferral never settled";

    // Read the stack after that window. Pre-fix this dereferences freed spline data.
    constexpr uint32 kCountX = 200u;
    constexpr uint32 kCountZ = 80u;
    constexpr int64 kFirstX = 700;
    constexpr int64 kFirstZ = 460;
    std::vector<float32> composed;
    ASSERT_TRUE(system.ComposeTiledGroundBlock(*tiled, kTiledHeightScale, /*terrainOriginY*/ 0.0f,
                                               kFirstX, kFirstZ, kCountX, kCountZ, composed));

    const int64 interior = static_cast<int64>(tiled->Config.TileConfig.HeightmapWidth) - 1;
    for (uint32 j = 0; j < kCountZ; ++j)
        for (uint32 i = 0; i < kCountX; ++i)
        {
            const int64 gx = kFirstX + i;
            const int64 gz = kFirstZ + j;
            const float32 baked =
                Tile(tiled, static_cast<int32>(gx / interior), static_cast<int32>(gz / interior))
                    ->Heightfield.GetSample(static_cast<uint32>(gx % interior),
                                            static_cast<uint32>(gz % interior));
            ASSERT_EQ(composed[static_cast<std::size_t>(j) * kCountX + i], baked)
                << "composed ground diverged from the baked tiles after a gate-closed flush, at ("
                << gx << "," << gz << ")";
        }

    if (SplineECS::SplineService::IsInitialized())
        SplineECS::SplineService::Shutdown();
}

// COST PROBE (DISABLED — run with --gtest_also_run_disabled_tests, Release only).
// Times one lone tile's full bake: world-space base noise plus the modifier stack
// over 1025^2 = 1,050,625 samples. That is exactly the composition a
// camera-independent ground source runs, so the per-sample figure it prints sizes
// any corridor-shaped query against the water fill's own 300,000-corner window cap.
TEST(TerrainRegionBake, DISABLED_ComposedTileBakeCost)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 2.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    svc.LoadTile(handle, TileCoord{1, 0})->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);
    // Three overlapping flattens: a stack, not a single-effect best case.
    CreateFlattenModifier(world, kTiledTileWorld * 1.25f, 300.0f, 20.0f);
    CreateFlattenModifier(world, kTiledTileWorld * 1.50f, 500.0f, 24.0f);
    CreateFlattenModifier(world, kTiledTileWorld * 1.75f, 700.0f, 28.0f);

    TerrainModifierSystem system;
    const auto t0 = std::chrono::steady_clock::now();
    system.Update(world, 1.0f / 60.0f); // full bake of the one resident tile
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    const uint64 samples = Tile(tiled, 1, 0)->Heightfield.GetSampleCount();
    ASSERT_GT(samples, 0u);
    std::printf("[ COST ] one composed tile: %llu samples in %.2f ms (%.1f ns/sample); "
                "a 300,000-corner fill window projects to %.2f ms\n",
                static_cast<unsigned long long>(samples), ms,
                ms * 1.0e6 / static_cast<double>(samples),
                ms * 300000.0 / static_cast<double>(samples));
}

// PARITY: a region-scoped tiled re-bake produces heightfields and splatmaps
// bit-identical to a fresh full bake of the final modifier state, on every
// tile. Also asserts the region bake left the far tile untouched (discriminates
// against the pre-fix full re-bake).
TEST(TerrainRegionBake, TiledRegionBakeMatchesFullBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    // Terrain A: full bake, then a modifier move (staying in tile 0) → region bake.
    const auto handleA = CreateResidentTiledTerrain(svc, 2);
    auto* tiledA = svc.GetTiledTerrainData(handleA);
    ASSERT_NE(tiledA, nullptr);

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTiledTerrainEntity(worldA, handleA, 2);
    auto modA = CreateFlattenModifier(worldA, 300.0f, 400.0f, 20.0f);

    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // full
    const uint64 vFar = Tile(tiledA, 1, 0)->HeightfieldVersion;

    auto* xf = worldA.GetComponentForWrite<Components::WorldTransform>(modA);
    ASSERT_NE(xf, nullptr);
    xf->matrix[12] = 250.0f;
    xf->matrix[14] = 330.0f;
    systemA.Update(worldA, 1.0f / 60.0f); // region
    EXPECT_EQ(Tile(tiledA, 1, 0)->HeightfieldVersion, vFar)
        << "region bake must not re-bake the untouched tile";

    // Terrain B: fresh full bake of the FINAL modifier state.
    const auto handleB = CreateResidentTiledTerrain(svc, 2);
    auto* tiledB = svc.GetTiledTerrainData(handleB);
    ASSERT_NE(tiledB, nullptr);

    ECS::World worldB;
    CreateTiledTerrainEntity(worldB, handleB, 2);
    CreateFlattenModifier(worldB, 250.0f, 330.0f, 20.0f);

    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f); // full

    // Every tile: heightfield + splatmap bit-identical between A and B.
    for (int32 z = 0; z < static_cast<int32>(tiledA->Config.TilesPerAxisZ); ++z)
        for (int32 x = 0; x < static_cast<int32>(tiledA->Config.TilesPerAxisX); ++x)
        {
            auto* ta = Tile(tiledA, x, z);
            auto* tb = Tile(tiledB, x, z);
            ASSERT_EQ(ta->Heightfield.GetSampleCount(), tb->Heightfield.GetSampleCount());
            EXPECT_EQ(0, std::memcmp(ta->Heightfield.GetRawSamples(),
                                     tb->Heightfield.GetRawSamples(),
                                     ta->Heightfield.GetSampleCount() * sizeof(float32)))
                << "heightfield mismatch at tile (" << x << "," << z << ")";
            ASSERT_EQ(ta->Splatmap.size(), tb->Splatmap.size());
            ASSERT_FALSE(ta->Splatmap.empty());
            EXPECT_EQ(0, std::memcmp(ta->Splatmap.data(), tb->Splatmap.data(), ta->Splatmap.size()))
                << "splatmap mismatch at tile (" << x << "," << z << ")";
        }
}

// PARITY under the DEFERRED range-shift renormalize (edit-realtime). A raise edit
// that lifts the global height max above the committed range would, pre-fix,
// renormalize EVERY resident tile's splat every dab (O(all tiles) — the dominant
// brush hitch). It is now deferred: the touched tile region-splats against the
// still-committed range and the terrain flags a pending flush; once editing goes
// quiescent (kSplatResplatSettleFrames) the one full renormalize runs against the
// FINAL range. This oracle proves the settled state is byte-identical to a fresh
// full bake — the same guarantee TiledRegionBakeMatchesFullBake makes for the
// stable-range path — and that the deferral always settles (no stuck pending).
TEST(TerrainRegionBake, TiledDeferredRangeShiftSplatSettlesToFullBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    // Terrain A: full bake at a raised target, then a further raise. TargetHeight >
    // HeightScale (64) escapes the [0,1] procedural-noise band, so the global max
    // grows past the committed range -> deferred re-splat -> settle.
    const auto handleA = CreateResidentTiledTerrain(svc, /*tilesX*/ 3);
    auto* tiledA = svc.GetTiledTerrainData(handleA);
    ASSERT_NE(tiledA, nullptr);

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTiledTerrainEntity(worldA, handleA, 3);
    // Inside tile (1,0), which spans world x in [1024,2048].
    auto modA = CreateFlattenModifier(worldA, 1536.0f, 512.0f, 100.0f); // norm 1.56
    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // full bake — commits the range

    auto* fmA = worldA.GetComponentForWrite<Components::TerrainFlattenEffect>(modA);
    ASSERT_NE(fmA, nullptr);
    fmA->TargetHeight = 200.0f;             // norm 3.125 — grows the global max
    systemA.Update(worldA, 1.0f / 60.0f);   // region height bake + DEFERRED splat
    EXPECT_TRUE(tiledA->SplatResplatPending) << "a range shift while editing must defer";

    // Quiescent frames: no modifier change -> the deferred renormalize settles. The
    // spread flush (Fix 2) renormalizes at a bounded per-frame texel budget, so it can
    // span several settle frames; loop until quiescent rather than a fixed count.
    for (int i = 0; i < 512 && tiledA->SplatResplatPending; ++i)
        systemA.Update(worldA, 1.0f / 60.0f);
    EXPECT_FALSE(tiledA->SplatResplatPending) << "editing settled -> deferral must flush";

    // Terrain B: fresh full bake of the FINAL modifier state.
    const auto handleB = CreateResidentTiledTerrain(svc, 3);
    auto* tiledB = svc.GetTiledTerrainData(handleB);
    ASSERT_NE(tiledB, nullptr);
    ECS::World worldB;
    RegisterModifierLifecycleEvents(worldB);
    CreateTiledTerrainEntity(worldB, handleB, 3);
    CreateFlattenModifier(worldB, 1536.0f, 512.0f, 200.0f);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f); // full bake

    for (int32 z = 0; z < static_cast<int32>(tiledA->Config.TilesPerAxisZ); ++z)
        for (int32 x = 0; x < static_cast<int32>(tiledA->Config.TilesPerAxisX); ++x)
        {
            auto* ta = Tile(tiledA, x, z);
            auto* tb = Tile(tiledB, x, z);
            ASSERT_EQ(ta->Splatmap.size(), tb->Splatmap.size());
            ASSERT_FALSE(ta->Splatmap.empty());
            EXPECT_EQ(0, std::memcmp(ta->Splatmap.data(), tb->Splatmap.data(), ta->Splatmap.size()))
                << "settled splat != full bake at tile (" << x << "," << z << ")";
        }
}

// QUIESCENCE-UNDER-CHURN guard: the deferred re-splat must reach settle even while
// tiles keep streaming in, and the range-extending stream-in flag must not create a
// stuck-pending loop. A range-shifting raise defers the whole-terrain renormalize
// and starts a settle countdown; then, under heavy atlas eviction (a 400-tile
// / 36-slot terrain), a tile re-streams almost every frame, waking the streamed-tile
// bake path. The settle countdown is re-armed only by an actual height edit (the
// range recompute is gated on heightDirty, so a stream-in-only wake takes the
// rangeStable branch), and BakeTiledStreamedTiles' range-extending flag deliberately
// does NOT reset the countdown — so continuous churn cannot starve the flush. After
// settle, further in-range stream-ins re-flag nothing and the terrain stays quiet.
TEST(TerrainRegionBake, TiledDeferredResplatSettlesUnderStreamInChurn)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const auto handleA = CreateResidentTiledTerrain(svc, /*tilesX*/ 3);
    auto* tiledA = svc.GetTiledTerrainData(handleA);
    ASSERT_NE(tiledA, nullptr);

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTiledTerrainEntity(worldA, handleA, 3);
    // Modifier inside tile (1,0); TargetHeight escapes the [0,1] noise band so the
    // global max grows past the committed range -> deferred re-splat.
    auto modA = CreateFlattenModifier(worldA, 1536.0f, 512.0f, 100.0f);
    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // full bake — commits the range

    auto* fmA = worldA.GetComponentForWrite<Components::TerrainFlattenEffect>(modA);
    ASSERT_NE(fmA, nullptr);
    fmA->TargetHeight = 300.0f;           // grows the global max -> range shift
    systemA.Update(worldA, 1.0f / 60.0f); // region height bake + range-shift re-splat
    if (std::getenv("GE_TERRAIN_DEFER_RESPLAT") == nullptr)
    {
        ASSERT_TRUE(tiledA->SplatResplatPending) << "a range shift while editing must defer";
    }

    // Sustained stream-in churn with NO further authored edit: each frame flip a
    // resident tile back to "needs bake" (what atlas re-residency does) so the
    // streamed-tile-wake path runs. This must NOT keep the settle from firing.
    // The spread flush (Fix 2) can span several settle frames; keep churning while it
    // drains and loop until quiescent (bounded), proving the churn never starves it.
    for (int32 i = 0; i < 512 && tiledA->SplatResplatPending; ++i)
    {
        Tile(tiledA, i % 3, 0)->ModifiersApplied = false; // re-stream churn
        systemA.Update(worldA, 1.0f / 60.0f);
    }
    EXPECT_FALSE(tiledA->SplatResplatPending)
        << "sustained stream-in churn must not starve the deferred settle";

    // Oracle: a fresh full bake of the FINAL modifier state.
    const auto handleB = CreateResidentTiledTerrain(svc, 3);
    auto* tiledB = svc.GetTiledTerrainData(handleB);
    ASSERT_NE(tiledB, nullptr);
    ECS::World worldB;
    RegisterModifierLifecycleEvents(worldB);
    CreateTiledTerrainEntity(worldB, handleB, 3);
    CreateFlattenModifier(worldB, 1536.0f, 512.0f, 300.0f);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    for (int32 z = 0; z < static_cast<int32>(tiledA->Config.TilesPerAxisZ); ++z)
        for (int32 x = 0; x < static_cast<int32>(tiledA->Config.TilesPerAxisX); ++x)
        {
            auto* ta = Tile(tiledA, x, z);
            auto* tb = Tile(tiledB, x, z);
            ASSERT_EQ(ta->Splatmap.size(), tb->Splatmap.size());
            ASSERT_FALSE(ta->Splatmap.empty());
            EXPECT_EQ(0, std::memcmp(ta->Splatmap.data(), tb->Splatmap.data(), ta->Splatmap.size()))
                << "settled splat != full bake at tile (" << x << "," << z << ") under churn";
        }
}

// SEAM CONTINUITY + proportionality: a region edit straddling the tile 0/1 seam
// re-bakes both tiles' shared edge to identical world samples, and leaves the
// far tile (2) untouched. World-space noise makes the seam match; the far-tile
// version check discriminates against the pre-fix full re-bake.
TEST(TerrainRegionBake, TiledSeamContinuityAcrossRegionEdit)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const auto handle = CreateResidentTiledTerrain(svc, /*tilesX*/ 3);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    ASSERT_EQ(tiled->Config.TilesPerAxisX, 3u);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    // Straddle the seam between tile 0 ([0,1024]) and tile 1 ([1024,2048]).
    auto mod = CreateFlattenModifier(world, 1024.0f, 400.0f, 15.0f);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // full bake
    const uint64 vFar = Tile(tiled, 2, 0)->HeightfieldVersion;

    // Move it (still straddling the same seam) → region bake touching tiles 0,1.
    auto* xf = world.GetComponentForWrite<Components::WorldTransform>(mod);
    ASSERT_NE(xf, nullptr);
    xf->matrix[12] = 1024.0f;
    xf->matrix[14] = 430.0f;
    system.Update(world, 1.0f / 60.0f);

    EXPECT_EQ(Tile(tiled, 2, 0)->HeightfieldVersion, vFar)
        << "a seam edit between tiles 0 and 1 must not re-bake tile 2";

    // Shared edge: tile0 x=w-1 (world 1024) == tile1 x=0 (world 1024) for all z.
    auto* t0 = Tile(tiled, 0, 0);
    auto* t1 = Tile(tiled, 1, 0);
    const uint32 w = t0->Heightfield.GetWidth();
    const uint32 h = t0->Heightfield.GetHeight();
    ASSERT_EQ(w, t1->Heightfield.GetWidth());
    for (uint32 z = 0; z < h; ++z)
        EXPECT_FLOAT_EQ(t0->Heightfield.GetSample(w - 1, z), t1->Heightfield.GetSample(0, z))
            << "seam sample mismatch at z=" << z;
}

// SETTLE-SPIKE BOUND (Fix 2): the deferred whole-terrain splat renormalize is spread
// across settle frames at a bounded per-frame texel budget, so a large tiled terrain's
// stroke-release settle never regenerates every resident tile's splat in a single frame
// (the ~1.4s spike). Drives a range-shifting raise on a multi-tile terrain and asserts
// (a) no settle frame renormalizes more than the budget + one texel-row of slop,
// (b) when the resident splat area exceeds the budget the settle spans >1 frame (it
// actually spread), and (c) the total texels renormalized equals the resident splat area
// exactly (each texel once — the byte-identity precondition, with full-bake parity
// already locked by TiledDeferredRangeShiftSplatSettlesToFullBake).
TEST(TerrainRegionBake, SpreadSettleBoundsPerFrameRenormalizeTexels)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    constexpr uint32 kTiles = 4;
    const auto handle = CreateResidentTiledTerrain(svc, kTiles);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, kTiles);
    // Inside tile (0,0); TargetHeight escapes the [0,1] noise band -> global max grows.
    auto mod = CreateFlattenModifier(world, 512.0f, 512.0f, 100.0f);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // full bake commits the range

    auto* fm = world.GetComponentForWrite<Components::TerrainFlattenEffect>(mod);
    ASSERT_NE(fm, nullptr);
    fm->TargetHeight = 300.0f;          // grows global max -> range shift -> deferred
    system.Update(world, 1.0f / 60.0f); // region height bake + deferred splat
    ASSERT_TRUE(tiled->SplatResplatPending);

    std::size_t residentArea = 0;
    std::size_t maxTileWidth = 0;
    for (const auto& [coord, tp] : tiled->Tiles)
        if (tp && tp->LodState == TileLodState::Full && !tp->Splatmap.empty())
        {
            const std::size_t tw = tp->Heightfield.GetWidth();
            residentArea += tw * tp->Heightfield.GetHeight();
            maxTileWidth = std::max(maxTileWidth, tw);
        }
    const std::size_t budget = TerrainModifierSystem::GetSplatRenormalizeTexelBudgetForTests();
    const std::size_t perFrameBound = budget + maxTileWidth; // one texel-row of round-up slop

    // The band-spread assertions only hold with the spread on (default). The
    // GE_TERRAIN_SPREAD_SETTLE=0 kill switch restores the one-shot whole-tile flush (which
    // does not go through the row counter), so under it we only assert the settle completes
    // — its byte-identity is covered by TiledDeferredRangeShiftSplatSettlesToFullBake.
    const char* spreadEnv = std::getenv("GE_TERRAIN_SPREAD_SETTLE");
    const bool spread = spreadEnv == nullptr || spreadEnv[0] != '0';

    uint64 prev = TerrainModifierSystem::GetSplatRenormalizeTexelCountForTests();
    const uint64 base = prev;
    int spreadFrames = 0;
    for (int i = 0; i < 4096 && tiled->SplatResplatPending; ++i)
    {
        system.Update(world, 1.0f / 60.0f);
        const uint64 now = TerrainModifierSystem::GetSplatRenormalizeTexelCountForTests();
        const uint64 delta = now - prev;
        prev = now;
        if (delta > 0)
        {
            ++spreadFrames;
            if (spread)
            {
                EXPECT_LE(delta, static_cast<uint64>(perFrameBound))
                    << "settle frame renormalized more than the per-frame budget";
            }
        }
    }
    EXPECT_FALSE(tiled->SplatResplatPending) << "settle must complete";
    if (spread)
    {
        EXPECT_EQ(prev - base, static_cast<uint64>(residentArea))
            << "spread must renormalize each resident splat texel exactly once";
        if (residentArea > budget)
        {
            EXPECT_GE(spreadFrames, 2) << "a terrain larger than the budget must spread across frames";
        }
    }
}

// PER-DAB CPU WORK BOUND: a sculpt dab into ONE tile of a multi-tile terrain does
// O(touched tiles) work, not O(resident tiles). Each dab rescans only the tile it edits
// for its height range (RefreshTileHeightRange), so per-dab CPU cost stays flat no matter
// how many tiles are resident or how long the stroke runs. (The GPU-upload half of the
// per-dab cost — the unified region vs whole-tile height/normal/splat upload, Fix 1 — is
// exercised in the editor A/B; this bounds the modifier system's per-dab CPU contribution.)
TEST(TerrainRegionBake, StrokeDabWorkStaysTileScopedNotResidentScoped)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    constexpr uint32 kTiles = 4; // a single-tile dab must not scan all 4 resident tiles
    const auto handle = CreateResidentTiledTerrain(svc, kTiles);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, kTiles);
    auto mod = CreateFlattenModifier(world, 300.0f, 400.0f, 20.0f); // fully inside tile (0,0)
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // full bake (scans all tiles once — expected)

    for (int d = 0; d < 8; ++d)
    {
        const uint64 before = TerrainModifierSystem::GetTileHeightRescanCountForTests();
        // Re-fetch for write each dab so the move stamps the ECS change gate (a single
        // GetComponentForWrite stamps once; later direct writes would go unseen). Each
        // dab is a distinct position, all inside tile (0,0) ([0,1024]).
        auto* xf = world.GetComponentForWrite<Components::WorldTransform>(mod);
        ASSERT_NE(xf, nullptr);
        xf->matrix[12] = 320.0f + static_cast<float32>(d) * 8.0f;
        xf->matrix[14] = 400.0f + static_cast<float32>(d) * 6.0f;
        system.Update(world, 1.0f / 60.0f);
        const uint64 dabScans = TerrainModifierSystem::GetTileHeightRescanCountForTests() - before;
        EXPECT_EQ(dabScans, 1u)
            << "dab " << d << " scanned " << dabScans << " tiles; a single-tile dab must scan 1";
    }
}

namespace
{
// Runs the modifier system until `tile`'s arrival bake is in (ModifiersApplied), at most a few
// frames past the row budget's count, and returns the frames it took.
uint32 UpdateUntilArrivalBaked(TerrainModifierSystem& system, ECS::World& world, const TerrainTileData& tile)
{
    const uint32 budgetFrames =
        (tile.Heightfield.GetHeight() + TerrainModifierSystem::kArrivalSplatRowsPerFrame - 1u) /
        TerrainModifierSystem::kArrivalSplatRowsPerFrame;
    uint32 frames = 0;
    while (!tile.ModifiersApplied && frames < budgetFrames + 4u)
    {
        system.Update(world, 1.0f / 60.0f);
        ++frames;
    }
    return frames;
}

// Stream a NEW Full tile in via the real service path (CreateEmptyTile +
// SetTileHeightfield with a full-resolution world-space-noise heightfield),
// mirroring what TileStreamingManager::IntegrateCompletedTiles does on a Full
// integrate. Leaves LodState=Full and ModifiersApplied=false, so the next
// modifier bake must scope its work to this tile alone.
void StreamInFullTile(TerrainService& svc, TiledTerrainHandle handle, TileCoord coord)
{
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    svc.CreateEmptyTile(handle, coord);
    const auto& tileCfg = tiled->Config.TileConfig;
    const float32 originX = tiled->WorldOriginX + coord.X * tiled->Config.TileWorldSize;
    const float32 originZ = tiled->WorldOriginZ + coord.Z * tiled->Config.TileWorldSize;
    Terrain::HeightfieldData hf;
    hf.Resize(tileCfg.HeightmapWidth, tileCfg.HeightmapHeight, 0.0f);
    hf.FillWithNoiseWorldSpace(kTileNoiseFrequency, kTileNoiseAmplitude,
                               originX, originZ,
                               tiled->Config.TileWorldSize, tiled->Config.TileWorldSize,
                               kTileNoiseOctaves, kTileNoiseSeed);
    float32 minH = 1e30f, maxH = -1e30f;
    const float32* s = hf.GetRawSamples();
    for (std::size_t i = 0; i < hf.GetSampleCount(); ++i)
    {
        minH = std::min(minH, s[i]);
        maxH = std::max(maxH, s[i]);
    }
    svc.SetTileHeightfield(handle, coord, std::move(hf), minH, maxH);
}

// The COARSE arrival: a heightfield at a lower resolution than the tile config, which is
// what SetTileHeightfield keys LodState::Coarse off.
//
// Mirrors the streaming job's shape, INCLUDING the ResetSplatmap that sizes the coarse splat
// (TileStreamingManager's coarse dispatch). Without that the tile arrives with an empty
// splatmap, and a test that disables the coarse bake then dies on the sizing assertion
// instead of on the material assertions it is actually about — the red arm would be
// measuring the helper, not the code under test.
void StreamInCoarseTile(TerrainService& svc, TiledTerrainHandle handle, TileCoord coord,
                        uint32 coarseRes)
{
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    svc.CreateEmptyTile(handle, coord);
    const float32 originX = tiled->WorldOriginX + coord.X * tiled->Config.TileWorldSize;
    const float32 originZ = tiled->WorldOriginZ + coord.Z * tiled->Config.TileWorldSize;
    Terrain::HeightfieldData hf;
    hf.Resize(coarseRes, coarseRes, 0.0f);
    hf.FillWithNoiseWorldSpace(kTileNoiseFrequency, kTileNoiseAmplitude, originX, originZ,
                               tiled->Config.TileWorldSize, tiled->Config.TileWorldSize,
                               kTileNoiseOctaves, kTileNoiseSeed);
    float32 minH = 1e30f, maxH = -1e30f;
    const float32* s = hf.GetRawSamples();
    for (std::size_t i = 0; i < hf.GetSampleCount(); ++i)
    {
        minH = std::min(minH, s[i]);
        maxH = std::max(maxH, s[i]);
    }
    svc.SetTileHeightfield(handle, coord, std::move(hf), minH, maxH);

    // The job sizes the splat before handing the tile over; do the same, so the tile arrives
    // exactly as production delivers it.
    auto* tile = tiled->Tiles[coord].get();
    ASSERT_NE(tile, nullptr);
    ResetSplatmap(tile->Heightfield, tile->Splatmap, tile->SplatmapWidth, tile->SplatmapHeight);
}
} // namespace

// STREAM-CHURN CONTRACT (E6-review#3): streaming a tile in with a modifier
// present must bake ONLY the new tile — zero HeightfieldVersion bumps on the
// already-resident tiles (each version bump is a re-upload and normal regen
// trigger, so zero other bumps == zero other regens / quiescence timers preserved).
// Pre-fix this routed through a full re-bake of every resident tile (tile
// residency was folded into the terrain-state hash), so the untouched-tile
// assertions fail on old code.
// COARSE tiles get the rules too, or the streaming edge is a flat band that POPS.
//
// Material comes only from authored rows now, and every full-detail splat path gates on
// LodState == Full. A coarse tile left unbaked therefore renders the unbaked base -- which
// the surface resolves to channel 0 -- across the whole distant ring, then snaps to the
// ruled surface the moment it upgrades. That is a visible distance artifact, not a
// correctness detail, so the bake runs on the tile's own coarse grid.
//
// DISCRIMINATOR: the assertion is that the coarse splat carries the ROW'S channel, not
// merely that it is non-zero -- an unbaked coarse tile is all-zero, and a tile baked with
// the wrong range or no rows would be too.
// SPLAT DETERMINISM ACROSS A STREAM CYCLE, through the rules path.
//
// Replaces two deleted tests whose oracle was the procedural classifier's output
// (TiledTerrain.AdjacentTiles_SplatmapBoundaryConsistency and
// StreamedTileSplatIsBitIdenticalAcrossStreamCycles). The drift CLASS they guarded is real
// and outlived their mechanism: the same tile, same heights, same committed range must
// resolve to the same bytes however many times it streams through. What changed is who
// decides those bytes -- authored rows rather than a hardcoded derivation -- so the
// assertion is made against a terrain carrying a rules volume.
//
// The positive control is load-bearing here: with no rows, both cycles produce all-zero and
// byte-identity would hold for the trivial reason.
TEST(TerrainRegionBake, StreamedTileSplatIsBitIdenticalAcrossStreamCyclesUnderRules)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    for (int32 x = 0; x < 2; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    CreateGlobalRulesModifier(world);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    // First arrival. The terrain is not modifier-FREE — it carries the global rules volume,
    // which is what gives the splat any content to be deterministic about.
    StreamInFullTile(svc, handle, TileCoord{2, 0});
    for (int32 i = 0; i < 8; ++i)
        system.Update(world, 1.0f / 60.0f);
    const std::vector<uint8> first = Tile(tiled, 2, 0)->Splatmap;
    ASSERT_FALSE(first.empty());
    ASSERT_GT(WeightedTexelCount(first), 0u)
        << "the rules row placed nothing - a byte-identity check would be zeros-to-zeros";

    // Stream out, then back in with the same deterministic heights.
    svc.UnloadTile(handle, TileCoord{2, 0});
    StreamInFullTile(svc, handle, TileCoord{2, 0});
    for (int32 i = 0; i < 8; ++i)
        system.Update(world, 1.0f / 60.0f);
    const std::vector<uint8>& second = Tile(tiled, 2, 0)->Splatmap;

    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(0, std::memcmp(first.data(), second.data(), first.size()))
        << "the tile's splat drifted across a stream-out/in cycle: same heights, same committed "
           "range, same rows, different bytes";
}

TEST(TerrainRegionBake, CoarseTileGetsTheSurfaceRulesBeforeItUpgrades)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    // One resident Full tile so the first bake commits a real splat range (the coarse pass
    // defers while the range is the [0,0] sentinel, which would collapse the rules' domain).
    svc.LoadTile(handle, TileCoord{0, 0})->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    CreateGlobalRulesModifier(world); // a HeightNormalized row targeting channel 1
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    ASSERT_TRUE(tiled->SplatBakeRangeValid) << "no committed range; the coarse pass would defer";

    // A coarse arrival: lower resolution than the tile config, which is what makes it Coarse.
    constexpr uint32 kCoarseRes = 17;
    StreamInCoarseTile(svc, handle, TileCoord{1, 0}, kCoarseRes);
    auto* coarse = Tile(tiled, 1, 0);
    ASSERT_NE(coarse, nullptr);
    ASSERT_EQ(coarse->LodState, TileLodState::Coarse);
    ASSERT_FALSE(coarse->CoarseSplatBaked);

    system.Update(world, 1.0f / 60.0f);

    EXPECT_TRUE(coarse->CoarseSplatBaked);
    ASSERT_EQ(coarse->SplatmapWidth, kCoarseRes) << "coarse splat not sized to the coarse grid";
    ASSERT_FALSE(coarse->Splatmap.empty());

    const std::size_t weighted = WeightedTexelCount(coarse->Splatmap);
    EXPECT_GT(weighted, 0u)
        << "the coarse tile carries NO material - it renders as the unbaked base across the "
           "streaming edge and pops on upgrade";

    std::size_t rowChannelTexels = 0;
    for (std::size_t i = 0; i + 3 < coarse->Splatmap.size(); i += 4)
        if (coarse->Splatmap[i + 1] > 0) // the rules row's MaterialSlot
            ++rowChannelTexels;
    EXPECT_GT(rowChannelTexels, 0u)
        << "coarse splat has weight but none in the row's channel - it was not the rules "
           "that wrote it";

    // The coarse pass must NOT consume the full bake: this tile still upgrades later.
    EXPECT_FALSE(coarse->ModifiersApplied)
        << "the coarse pass marked the tile fully applied - its full bake would be skipped";

    // Idempotent: a second tick must not re-bake a coarse tile that already has its rules.
    const uint64 revisionBefore = tiled->Revision;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(tiled->Revision, revisionBefore) << "the coarse pass re-baked on a quiet tick";
}

TEST(TerrainRegionBake, TiledStreamInWithModifierBakesOnlyThatTile)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    // 3-tile terrain; tiles 0 and 1 start resident (Full), tile 2 streams in.
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    ASSERT_EQ(tiled->Config.TilesPerAxisX, 3u);
    for (int32 x = 0; x < 2; ++x)
    {
        auto* t = svc.LoadTile(handle, TileCoord{x, 0});
        ASSERT_NE(t, nullptr);
        t->LodState = TileLodState::Full;
    }

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    // Modifier INSIDE tile 2 (x in [2048,3072]) so the streamed tile actually
    // needs a bake — the scoped path must touch that tile and no other.
    CreateFlattenModifier(world, 2560.0f, 512.0f, 20.0f);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // full bake: tiles 0,1 → ModifiersApplied

    const uint64 v0 = Tile(tiled, 0, 0)->HeightfieldVersion;
    const uint64 v1 = Tile(tiled, 1, 0)->HeightfieldVersion;
    ASSERT_GT(v0, 0u);
    ASSERT_GT(v1, 0u);
    ASSERT_TRUE(Tile(tiled, 0, 0)->ModifiersApplied);
    ASSERT_TRUE(Tile(tiled, 1, 0)->ModifiersApplied);

    // Stream in tile 2 (real service path). SetTileHeightfield bumps its version
    // once (for the upload) and clears ModifiersApplied.
    StreamInFullTile(svc, handle, TileCoord{2, 0});
    auto* t2 = Tile(tiled, 2, 0);
    ASSERT_EQ(t2->LodState, TileLodState::Full);
    ASSERT_FALSE(t2->ModifiersApplied);
    const uint64 v2AfterStream = t2->HeightfieldVersion;

    UpdateUntilArrivalBaked(system, world, *t2); // must bake ONLY tile 2

    EXPECT_EQ(Tile(tiled, 0, 0)->HeightfieldVersion, v0)
        << "resident tile 0 must NOT re-bake / re-cook on a stream-in";
    EXPECT_EQ(Tile(tiled, 1, 0)->HeightfieldVersion, v1)
        << "resident tile 1 must NOT re-bake / re-cook on a stream-in";
    EXPECT_GT(Tile(tiled, 2, 0)->HeightfieldVersion, v2AfterStream)
        << "the streamed tile hosting the modifier must have it baked in";
    EXPECT_TRUE(Tile(tiled, 2, 0)->ModifiersApplied);

    // Quiescence: with everything applied, further ticks bake nothing anywhere.
    const uint64 v2Applied = Tile(tiled, 2, 0)->HeightfieldVersion;
    for (int i = 0; i < 3; ++i)
        system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(Tile(tiled, 0, 0)->HeightfieldVersion, v0);
    EXPECT_EQ(Tile(tiled, 1, 0)->HeightfieldVersion, v1);
    EXPECT_EQ(Tile(tiled, 2, 0)->HeightfieldVersion, v2Applied);
}

// A splat-only arrival keeps the heights it streamed in. The scene's global surface rules reach
// every tile, so every arrival bakes, but the rules write no height: the base the streaming job
// filled is already the tile's final heightfield. Re-filling it rewrote the same samples and
// re-dirtied the tile (height upload, normal regen, quadtree sync) on the frame thread for
// nothing; on the web fly-by that re-fill alone was 49 ms per arriving tile.
//
// DISCRIMINATOR: one streamed sample is moved off the base before the bake. A re-fill writes the
// base value back over it; the kept heights still hold it. The splat must still bake (positive
// control: the rules row lands), or a skipped bake would pass for the trivial reason.
TEST(TerrainRegionBake, AnArrivalOnlySurfaceRulesReachKeepsItsStreamedHeights)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    for (int32 x = 0; x < 2; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    CreateGlobalRulesModifier(world);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    StreamInFullTile(svc, handle, TileCoord{2, 0});
    auto* arrived = Tile(tiled, 2, 0);
    ASSERT_FALSE(arrived->ModifiersApplied);
    const float32 base = arrived->Heightfield.GetSample(5, 5);
    const float32 marker = base + 0.125f;
    arrived->Heightfield.SetSample(5, 5, marker);
    const uint64 versionBefore = arrived->HeightfieldVersion;

    UpdateUntilArrivalBaked(system, world, *arrived);

    ASSERT_TRUE(arrived->ModifiersApplied);
    ASSERT_GT(WeightedTexelCount(arrived->Splatmap), 0u) << "the rules row placed nothing: the bake did not run";
    EXPECT_EQ(arrived->Heightfield.GetSample(5, 5), marker)
        << "the arrival's base was re-filled though only the surface rules reach it";
    EXPECT_EQ(arrived->HeightfieldVersion, versionBefore)
        << "a splat-only bake re-dirtied the arrival's heights (re-upload, normal regen)";
    EXPECT_TRUE(arrived->SplatmapDirty) << "the baked splat must still upload";
}

// An arriving tile's splat runs in row bands across the job pool, and the bands reproduce the
// serial bake byte for byte. The arrival bake was one serial call per tile on the frame thread:
// 72 ms per tile on the single-thread web build, and the threaded build's workers sat idle for it.
TEST(TerrainRegionBake, AnArrivalsSplatRunsInRowBandsAndMatchesTheSerialBake)
{
    auto arrive = [](JobSystem::WorkStealingThreadPool* pool, std::vector<uint8>& outSplat,
                     uint32& outBands) {
        ScopedTerrainService scoped;
        auto& svc = TerrainService::Get();
        TiledTerrainConfig cfg{};
        cfg.WorldSizeX = 3.0f * kTiledTileWorld;
        cfg.WorldSizeZ = kTiledTileWorld;
        cfg.HeightScale = kTiledHeightScale;
        cfg.SamplesPerMeter = 1.0f;
        const auto handle = svc.CreateTiledTerrain(cfg);
        auto* tiled = svc.GetTiledTerrainData(handle);
        ASSERT_NE(tiled, nullptr);
        for (int32 x = 0; x < 2; ++x)
            svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

        ECS::World world;
        if (pool)
            world.SetJobSystem(pool);
        RegisterModifierLifecycleEvents(world);
        CreateTiledTerrainEntity(world, handle, 3);
        CreateGlobalRulesModifier(world);
        TerrainModifierSystem system;
        system.Update(world, 1.0f / 60.0f);

        StreamInFullTile(svc, handle, TileCoord{2, 0});
        system.Update(world, 1.0f / 60.0f);
        outBands = system.GetLastBakeBandCountForTests();
        UpdateUntilArrivalBaked(system, world, *Tile(tiled, 2, 0));
        ASSERT_TRUE(Tile(tiled, 2, 0)->ModifiersApplied);
        outSplat = Tile(tiled, 2, 0)->Splatmap;
    };

    std::vector<uint8> serial, parallel;
    uint32 serialBands = 0, parallelBands = 0;
    arrive(nullptr, serial, serialBands);
    {
        JobSystem::WorkStealingThreadPool pool(4);
        JobSystem::JobCounter counter;
        std::promise<void> started;
        auto workerStarted = started.get_future();
        pool.Run([&]() {
            started.set_value();
            arrive(&pool, parallel, parallelBands);
        }, counter);
        // The bake runs on a worker, as the scheduler runs it; the main thread only joins.
        workerStarted.wait();
        pool.Wait(counter);
    }

    EXPECT_EQ(serialBands, 0u);
    // A frame's row budget in kBakeBandRows bands.
    const auto bandRows = static_cast<uint32>(kBakeBandRows);
    EXPECT_EQ(parallelBands, (TerrainModifierSystem::kArrivalSplatRowsPerFrame + bandRows - 1u) / bandRows)
        << "the arrival's splat did not fan out across the pool";
    ASSERT_GT(WeightedTexelCount(serial), 0u) << "the rules row placed nothing: zeros would match zeros";
    ASSERT_EQ(serial.size(), parallel.size());
    EXPECT_EQ(0, std::memcmp(serial.data(), parallel.data(), serial.size()))
        << "the banded arrival splat differs from the serial one";
}

// The arrivals' surface-rules splat spends at most kArrivalSplatRowsPerFrame rows a frame on one
// terrain, however many tiles arrive together: a whole tile in one frame costs 72 ms on the
// single-thread web build and about 5 ms a tile joined into the frame on a threaded one (#3408).
// A tile stays unapplied until its last row is in.
TEST(TerrainRegionBake, ArrivalsSplatSpendsAtMostTheTerrainsRowBudgetEachFrame)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 4.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    for (int32 x = 0; x < 2; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 4);
    CreateGlobalRulesModifier(world);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    StreamInFullTile(svc, handle, TileCoord{2, 0});
    StreamInFullTile(svc, handle, TileCoord{3, 0});
    const std::array<const TerrainTileData*, 2> arrivals{Tile(tiled, 2, 0), Tile(tiled, 3, 0)};
    const uint32 rows = arrivals[0]->Heightfield.GetHeight();
    const uint32 budget = TerrainModifierSystem::kArrivalSplatRowsPerFrame;
    ASSERT_GT(rows, budget) << "a tile fits one frame's budget: nothing to spread";

    // The rows a tile has composited: its cursor while it bakes, all of them once applied.
    const auto bakedRows = [rows](const TerrainTileData& t) {
        return t.ModifiersApplied ? rows : t.ArrivalSplatNextRow;
    };
    uint32 frames = 0;
    uint32 baked = 0;
    while (!(arrivals[0]->ModifiersApplied && arrivals[1]->ModifiersApplied) && frames < 2u * rows)
    {
        system.Update(world, 1.0f / 60.0f);
        ++frames;
        const uint32 nowBaked = bakedRows(*arrivals[0]) + bakedRows(*arrivals[1]);
        EXPECT_LE(nowBaked - baked, budget) << "frame " << frames << " spent more than the terrain's budget";
        baked = nowBaked;
    }
    EXPECT_TRUE(arrivals[0]->ModifiersApplied && arrivals[1]->ModifiersApplied);
    EXPECT_EQ(frames, (2u * rows + budget - 1u) / budget);
    EXPECT_GT(WeightedTexelCount(arrivals[0]->Splatmap), 0u) << "the rules row placed nothing";
    EXPECT_GT(WeightedTexelCount(arrivals[1]->Splatmap), 0u) << "the rules row placed nothing";
}

// A Coarse tile turning Full keeps showing its coarse splat while its arrival bake runs: the
// uploaders draw ArrivalSplatStandIn, so the partly baked splat never reaches the screen. The
// bake's end drops the stand-in and marks the baked splat for upload.
TEST(TerrainRegionBake, ATileTurningFullShowsItsCoarseSplatUntilItsArrivalBakeEnds)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    for (int32 x = 0; x < 2; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    CreateGlobalRulesModifier(world);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const TileCoord coord{2, 0};
    svc.CreateEmptyTile(handle, coord);
    constexpr uint32 kCoarse = 33;
    Terrain::HeightfieldData coarse;
    coarse.Resize(kCoarse, kCoarse, 0.0f);
    svc.SetTileHeightfield(handle, coord, std::move(coarse), 0.0f, 0.0f);
    std::vector<uint8> coarseSplat(static_cast<size_t>(kCoarse) * kCoarse * 4u, 0u);
    for (std::size_t i = 0; i + 3 < coarseSplat.size(); i += 4)
        coarseSplat[i + 1] = 255u;
    const std::vector<uint8> shown = coarseSplat;
    svc.SetTileMaps(handle, coord, std::move(coarseSplat), kCoarse, kCoarse, {}, 0, 0);
    ASSERT_EQ(Tile(tiled, 2, 0)->LodState, TileLodState::Coarse);

    StreamInFullTile(svc, handle, coord);
    const TerrainTileData& arrived = *Tile(tiled, 2, 0);
    ASSERT_EQ(arrived.LodState, TileLodState::Full);
    EXPECT_EQ(arrived.ArrivalSplatStandIn, shown) << "the coarse splat is not what the arrival shows";
    EXPECT_EQ(arrived.ArrivalSplatStandInDim, kCoarse);

    system.Update(world, 1.0f / 60.0f);
    ASSERT_FALSE(arrived.ModifiersApplied) << "one frame baked the whole tile: no stand-in phase to check";
    EXPECT_EQ(arrived.ArrivalSplatStandIn, shown) << "the stand-in left before the bake ended";

    UpdateUntilArrivalBaked(system, world, arrived);
    ASSERT_TRUE(arrived.ModifiersApplied);
    EXPECT_TRUE(arrived.ArrivalSplatStandIn.empty());
    EXPECT_TRUE(arrived.SplatmapDirty) << "the baked splat must upload";
}

// Two tiles a height modifier reaches arrive together; the second waits frames for the row budget.
// Each one's heights are composed once, on its first bake frame: composing a tile again on every
// frame it waits re-uploads its heights, regenerates its normal and syncs the quadtree each time,
// for the same samples.
TEST(TerrainRegionBake, AnArrivalWaitingForTheRowBudgetComposesItsHeightsOnce)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 4.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    for (int32 x = 0; x < 2; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 4);
    CreateGlobalRulesModifier(world);
    // A flatten across the boundary of tiles 2 and 3 (x 2048 to 4096), clear of tiles 0 and 1.
    CreateFlattenModifier(world, 3072.0f, 512.0f, 20.0f, 700.0f);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    StreamInFullTile(svc, handle, TileCoord{2, 0});
    StreamInFullTile(svc, handle, TileCoord{3, 0});
    const TerrainTileData& first = *Tile(tiled, 2, 0);
    const TerrainTileData& second = *Tile(tiled, 3, 0);
    const uint64 firstStreamed = first.HeightfieldVersion;
    const uint64 secondStreamed = second.HeightfieldVersion;
    ASSERT_GT(first.Heightfield.GetHeight(), TerrainModifierSystem::kArrivalSplatRowsPerFrame)
        << "a tile fits one frame's budget: neither arrival waits";

    for (uint32 frame = 0; frame < 64u && !(first.ModifiersApplied && second.ModifiersApplied); ++frame)
        system.Update(world, 1.0f / 60.0f);
    ASSERT_TRUE(first.ModifiersApplied && second.ModifiersApplied);
    EXPECT_EQ(first.HeightfieldVersion, firstStreamed + 1u) << "tile 2's heights were composed more than once";
    EXPECT_EQ(second.HeightfieldVersion, secondStreamed + 1u) << "tile 3's heights were composed more than once";
}

namespace
{
// The highest cached top among a terrain's resident Full tiles (1 when none is higher).
float32 ResidentFullMaxHeight(const TiledTerrainData& tiled)
{
    float32 maxH = 1.0f;
    for (const auto& [coord, tilePtr] : tiled.Tiles)
        if (tilePtr && tilePtr->LodState == TileLodState::Full)
            maxH = std::max(maxH, tilePtr->CachedMaxH);
    return maxH;
}

struct ArrivalAcrossDeparture
{
    std::vector<uint8> Splat;
    float32 ResidentMaxBefore = 0.0f;
    float32 ResidentMaxAfter = 0.0f;
    bool ResplatPending = false;
};

// Tile 2 arrives beside resident tiles 0 and 1; a flatten raises tile 1, the top of the resident
// range. With `departMidBake`, tile 1 departs after the arrival's first bake frame, which shrinks
// the resident range while the arrival's later rows are still to bake.
ArrivalAcrossDeparture BakeArrivalAcrossADeparture(bool departMidBake)
{
    ArrivalAcrossDeparture result;
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    for (int32 x = 0; x < 2; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    CreateGlobalRulesModifier(world);
    CreateFlattenModifier(world, 1536.0f, 512.0f, 200.0f, 300.0f);
    TerrainModifierSystem system;
    for (uint32 frame = 0; frame < 4u; ++frame)
        system.Update(world, 1.0f / 60.0f);
    result.ResidentMaxBefore = ResidentFullMaxHeight(*tiled);

    StreamInFullTile(svc, handle, TileCoord{2, 0});
    system.Update(world, 1.0f / 60.0f);
    if (departMidBake)
        svc.UnloadTile(handle, TileCoord{1, 0});
    UpdateUntilArrivalBaked(system, world, *Tile(tiled, 2, 0));
    for (uint32 frame = 0; frame < 64u; ++frame)
        system.Update(world, 1.0f / 60.0f);
    result.ResidentMaxAfter = ResidentFullMaxHeight(*tiled);
    result.ResplatPending = tiled->SplatResplatPending;
    result.Splat = Tile(tiled, 2, 0)->Splatmap;
    return result;
}
} // namespace

// A resident tile departs while an arrival's splat bake is spread over frames, and the resident
// height range shrinks. Every row of the arrival normalizes against the one range its bake began
// with: a range that changes at a frame boundary splits the tile into two bands of different
// ground, and a shrinking range schedules no resplat to heal it.
TEST(TerrainRegionBake, AnArrivalsRowsShareOneHeightRangeWhenATileDepartsMidBake)
{
    const ArrivalAcrossDeparture stay = BakeArrivalAcrossADeparture(false);
    const ArrivalAcrossDeparture depart = BakeArrivalAcrossADeparture(true);
    ASSERT_GT(depart.ResidentMaxBefore, depart.ResidentMaxAfter) << "the departure did not shrink the range";
    EXPECT_FALSE(depart.ResplatPending) << "a resplat would heal the split: the departure must not schedule one";
    ASSERT_GT(WeightedTexelCount(stay.Splat), 0u) << "the rules row placed nothing: zeros would match zeros";
    ASSERT_EQ(stay.Splat.size(), depart.Splat.size());
    EXPECT_EQ(0, std::memcmp(stay.Splat.data(), depart.Splat.data(), stay.Splat.size()))
        << "the arrival's rows were normalized against two height ranges";
}

namespace
{
struct ArrivalAcrossWidening
{
    std::vector<uint8> Splat;
    bool WidenedMidBake = false;
};

// Tile 0 arrives beside resident tiles 1 and 2; tile 3 arrives under a flatten that raises it
// above the resident range, which schedules the renormalize. With `widenMidBake`, tile 3 arrives
// after tile 0's first bake frame, and the renormalize rewrites tile 0 while its bake still runs;
// otherwise tile 3 arrives and settles first.
ArrivalAcrossWidening BakeArrivalAcrossAWidening(bool widenMidBake)
{
    ArrivalAcrossWidening result;
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 4.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    for (int32 x = 1; x < 3; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 4);
    CreateGlobalRulesModifier(world);
    CreateFlattenModifier(world, 3584.0f, 512.0f, 200.0f, 300.0f);
    TerrainModifierSystem system;
    for (uint32 frame = 0; frame < 4u; ++frame)
        system.Update(world, 1.0f / 60.0f);

    constexpr uint32 kSettleFrames = 96;
    if (!widenMidBake)
    {
        StreamInFullTile(svc, handle, TileCoord{3, 0});
        UpdateUntilArrivalBaked(system, world, *Tile(tiled, 3, 0));
        for (uint32 frame = 0; frame < kSettleFrames; ++frame)
            system.Update(world, 1.0f / 60.0f);
    }
    StreamInFullTile(svc, handle, TileCoord{0, 0});
    system.Update(world, 1.0f / 60.0f);
    if (widenMidBake)
    {
        StreamInFullTile(svc, handle, TileCoord{3, 0});
        system.Update(world, 1.0f / 60.0f);
        result.WidenedMidBake = tiled->SplatResplatPending && !Tile(tiled, 0, 0)->ModifiersApplied;
    }
    UpdateUntilArrivalBaked(system, world, *Tile(tiled, 0, 0));
    UpdateUntilArrivalBaked(system, world, *Tile(tiled, 3, 0));
    for (uint32 frame = 0; frame < kSettleFrames; ++frame)
        system.Update(world, 1.0f / 60.0f);
    result.Splat = Tile(tiled, 0, 0)->Splatmap;
    return result;
}
} // namespace

// An arrival's splat bake is spread over frames, and another arrival widens the height range,
// so the renormalize rewrites the first arrival while its bake still runs. The bake's remaining
// rows move onto the renormalize's range: rows left on the range the bake began with would split
// the tile into two bands of different ground, and no later resplat heals them.
TEST(TerrainRegionBake, AnArrivalsRowsShareOneHeightRangeWhenARenormalizeWidensItMidBake)
{
    const ArrivalAcrossWidening settledFirst = BakeArrivalAcrossAWidening(false);
    const ArrivalAcrossWidening widened = BakeArrivalAcrossAWidening(true);
    ASSERT_TRUE(widened.WidenedMidBake) << "the second arrival did not widen the range during the first one's bake";
    ASSERT_GT(WeightedTexelCount(settledFirst.Splat), 0u) << "the rules row placed nothing: zeros would match zeros";
    ASSERT_EQ(settledFirst.Splat.size(), widened.Splat.size());
    EXPECT_EQ(0, std::memcmp(settledFirst.Splat.data(), widened.Splat.data(), settledFirst.Splat.size()))
        << "the arrival's rows were normalized against two height ranges";
}

// While a tile's arrival bake runs, its colour map keeps the one it showed while Coarse, as the
// uploaders keep its coarse splat: composing it from the partly baked splat would show the unbaked
// rows' ground on screen until the bake ends.
TEST(TerrainRegionBake, TheColorMapWaitsForAnArrivalsSplatBakeToEnd)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    for (int32 x = 0; x < 2; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    CreateGlobalRulesModifier(world);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    // The streaming path: a Coarse tile with its splat, then the Full heights and the job's
    // unbaked full-resolution splat.
    const TileCoord coord{2, 0};
    svc.CreateEmptyTile(handle, coord);
    constexpr uint32 kCoarse = 33;
    Terrain::HeightfieldData coarse;
    coarse.Resize(kCoarse, kCoarse, 0.0f);
    svc.SetTileHeightfield(handle, coord, std::move(coarse), 0.0f, 0.0f);
    svc.SetTileMaps(handle, coord, std::vector<uint8>(static_cast<size_t>(kCoarse) * kCoarse * 4u, 0u), kCoarse,
                    kCoarse, {}, 0, 0);
    system.Update(world, 1.0f / 60.0f);
    TerrainTileData& arrived = *Tile(tiled, 2, 0);
    ASSERT_FALSE(arrived.SplatColorDirtyFull) << "the colour map never composed the coarse tile";

    StreamInFullTile(svc, handle, coord);
    const uint32 res = arrived.Heightfield.GetWidth();
    svc.SetTileMaps(handle, coord, std::vector<uint8>(static_cast<size_t>(res) * res * 4u, 0u), res, res, {}, 0, 0);
    ASSERT_FALSE(arrived.ArrivalSplatStandIn.empty());

    system.Update(world, 1.0f / 60.0f);
    ASSERT_FALSE(arrived.ModifiersApplied) << "one frame baked the whole tile: no bake to wait for";
    EXPECT_TRUE(arrived.SplatColorDirtyFull) << "the colour map was composed from a partly baked splat";

    UpdateUntilArrivalBaked(system, world, arrived);
    ASSERT_TRUE(arrived.ModifiersApplied);
    system.Update(world, 1.0f / 60.0f);
    EXPECT_FALSE(arrived.SplatColorDirtyFull) << "the colour map did not compose the baked splat";
    EXPECT_EQ(arrived.ColorMap.Width, res);
}

// A coarse stand-in serves only the heights, extent and height scale it was built for: the
// extraction rebuilds it when any of them moved, so a stale one never reaches the screen.
TEST(TerrainRegionBake, ACoarseStandInGoesStaleWhenItsTileHeightsChange)
{
    constexpr uint32 kCoarse = 33;
    constexpr uint32 kTileRes = 129;
    Terrain::HeightfieldData coarse;
    coarse.Resize(kCoarse, kCoarse, 0.0f);
    coarse.FillWithNoiseWorldSpace(kTileNoiseFrequency, kTileNoiseAmplitude, 0.0f, 0.0f, kTiledTileWorld,
                                   kTiledTileWorld, kTileNoiseOctaves, kTileNoiseSeed);
    CoarseTileStandIn standIn;
    BuildCoarseTileStandIn(coarse, kTileRes, kTiledTileWorld, kTiledHeightScale, standIn);
    standIn.HeightfieldVersion = 7u;

    EXPECT_TRUE(standIn.Matches(kTileRes, kTiledTileWorld, kTiledHeightScale, 7u));
    EXPECT_FALSE(standIn.Matches(kTileRes, kTiledTileWorld, kTiledHeightScale, 8u)) << "new heights";
    EXPECT_FALSE(standIn.Matches(kTileRes, kTiledTileWorld, 2.0f * kTiledHeightScale, 7u)) << "new height scale";
    EXPECT_FALSE(standIn.Matches(kTileRes, 2.0f * kTiledTileWorld, kTiledHeightScale, 7u)) << "new extent";
    EXPECT_FALSE(standIn.Matches(2u * kTileRes - 1u, kTiledTileWorld, kTiledHeightScale, 7u)) << "new resolution";
}

// ZERO-WORK count oracle (coordinator REQUIRED 2): a stream-in in a scene where
// NO modifier touches the tile must cost ZERO bake work — no whole-tile noise
// re-fill (rescan count flat), no version bump beyond the streaming write, no
// re-upload. Discriminates against the first-pass code, which re-baked every
// streamed tile even with no modifiers (the async worker's output duplicated on
// the schedule thread).
TEST(TerrainRegionBake, TiledStreamInModifierFreeDoesZeroBakeWork)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    for (int32 x = 0; x < 2; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    // NO modifier at all.

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // modifier-free: mark applied, no bake
    ASSERT_TRUE(Tile(tiled, 0, 0)->ModifiersApplied);
    ASSERT_TRUE(Tile(tiled, 1, 0)->ModifiersApplied);

    const uint64 v0 = Tile(tiled, 0, 0)->HeightfieldVersion;
    const uint64 v1 = Tile(tiled, 1, 0)->HeightfieldVersion;
    const uint64 rescanBefore = TerrainModifierSystem::GetTileHeightRescanCountForTests();

    StreamInFullTile(svc, handle, TileCoord{2, 0});
    const uint64 v2AfterStream = Tile(tiled, 2, 0)->HeightfieldVersion;

    system.Update(world, 1.0f / 60.0f); // must do ZERO streamed-bake work

    EXPECT_EQ(TerrainModifierSystem::GetTileHeightRescanCountForTests(), rescanBefore)
        << "modifier-free stream-in must not rescan any tile's heights";
    EXPECT_EQ(Tile(tiled, 0, 0)->HeightfieldVersion, v0);
    EXPECT_EQ(Tile(tiled, 1, 0)->HeightfieldVersion, v1);
    EXPECT_EQ(Tile(tiled, 2, 0)->HeightfieldVersion, v2AfterStream)
        << "a modifier-free streamed tile keeps its streamed base — no re-fill / bump";
    EXPECT_TRUE(Tile(tiled, 2, 0)->ModifiersApplied);
}

// SPLAT-RANGE DRIFT, height-edit path (F2): a height edit re-derives the splat
// normalization range from ComputeResidentGlobalHeightRange, which the bakes
// ASSIGN into CachedGlobal*. That aggregate is floored to the procedural noise
// band [0, kTileNoiseAmplitude], so an edit keeps the range residency-invariant
// (== the streaming seed) instead of collapsing it to the resident tiles'
// observed extremes. Pre-fix (aggregate seeded at the resident min/max) the first
// height edit anywhere re-armed drift for the WHOLE terrain: the floored min here
// is 0, not the > 0 floor the resident noise tiles actually reach.
TEST(TerrainRegionBake, HeightEditBakeFloorsSplatRangeToNoiseBand)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    for (int32 x = 0; x < 3; ++x)
    {
        auto* t = svc.LoadTile(handle, TileCoord{x, 0});
        ASSERT_NE(t, nullptr);
        t->LodState = TileLodState::Full;
    }

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    CreateFlattenModifier(world, 512.0f, 512.0f, 20.0f); // a height edit -> full bake

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    // The resident noise tiles reach a floor strictly above 0, but the range is
    // pinned to the band floor 0 and at least its ceiling — residency-invariant.
    EXPECT_FLOAT_EQ(tiled->CachedGlobalMinH, 0.0f)
        << "the splat range floor must be the noise band 0, not the observed tile min";
    EXPECT_GE(tiled->CachedGlobalMaxH, kTileNoiseAmplitude)
        << "the range must still cover the full noise band";
}

// PARITY: the streamed-tile scoped bake produces a heightfield + splatmap for
// the new tile bit-identical to a fresh full bake that had all three tiles
// resident from the start. Uses a modifier INSIDE tile 2 so the scoped path does
// the full parity work. (Only the streamed tile is compared: the already-resident
// tiles' splats are deliberately NOT renormalized on a stream-in — that residual
// global-range drift is reconciled on the next authored height edit.)
TEST(TerrainRegionBake, TiledStreamInBakeMatchesFullBakeForNewTile)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    auto makeThreeTile = [&]() {
        TiledTerrainConfig cfg{};
        cfg.WorldSizeX = 3.0f * kTiledTileWorld;
        cfg.WorldSizeZ = kTiledTileWorld;
        cfg.HeightScale = kTiledHeightScale;
        cfg.SamplesPerMeter = 1.0f;
        return svc.CreateTiledTerrain(cfg);
    };

    // Terrain A: tiles 0,1 resident + full bake, then stream in tile 2 → scoped bake.
    const auto handleA = makeThreeTile();
    auto* tiledA = svc.GetTiledTerrainData(handleA);
    for (int32 x = 0; x < 2; ++x)
        svc.LoadTile(handleA, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTiledTerrainEntity(worldA, handleA, 3);
    CreateFlattenModifier(worldA, 2560.0f, 512.0f, 20.0f); // inside tile 2
    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f);
    StreamInFullTile(svc, handleA, TileCoord{2, 0});
    systemA.Update(worldA, 1.0f / 60.0f);

    // Terrain B: all three tiles resident from the start → one full bake.
    const auto handleB = makeThreeTile();
    auto* tiledB = svc.GetTiledTerrainData(handleB);
    for (int32 x = 0; x < 3; ++x)
        svc.LoadTile(handleB, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World worldB;
    CreateTiledTerrainEntity(worldB, handleB, 3);
    CreateFlattenModifier(worldB, 2560.0f, 512.0f, 20.0f);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    auto* ta = Tile(tiledA, 2, 0);
    auto* tb = Tile(tiledB, 2, 0);
    ASSERT_EQ(ta->Heightfield.GetSampleCount(), tb->Heightfield.GetSampleCount());
    EXPECT_EQ(0, std::memcmp(ta->Heightfield.GetRawSamples(), tb->Heightfield.GetRawSamples(),
                             ta->Heightfield.GetSampleCount() * sizeof(float32)))
        << "streamed tile heightfield must match a full bake";
    ASSERT_FALSE(ta->Splatmap.empty());
    ASSERT_EQ(ta->Splatmap.size(), tb->Splatmap.size());
    EXPECT_EQ(0, std::memcmp(ta->Splatmap.data(), tb->Splatmap.data(), ta->Splatmap.size()))
        << "streamed tile splatmap must match a full bake";
}

// STREAM-IN THAT EXTENDS THE RANGE (persistent-seam repro, hole a): a tile that
// streams in carrying heights above the committed splat range is itself normalized
// against the live resident aggregate, but every already-resident tile stays at the
// committed range -> a normalization seam at the new tile's border. Pre-fix nothing
// reconciled this until the next authored height edit, so the seam persisted. The
// fix flags the deferred renormalize on a range-extending stream-in, so the settle
// brings every resident tile onto the one final range (== a fresh full bake).
TEST(TerrainRegionBake, TiledStreamInExtendingRangeRenormalizesResidentsToFullBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    auto makeThreeTile = [&]() {
        TiledTerrainConfig cfg{};
        cfg.WorldSizeX = 3.0f * kTiledTileWorld;
        cfg.WorldSizeZ = kTiledTileWorld;
        cfg.HeightScale = kTiledHeightScale;
        cfg.SamplesPerMeter = 1.0f;
        return svc.CreateTiledTerrain(cfg);
    };

    // Terrain A: tiles 0,1 resident + full bake (commits [0, band]); the modifier is
    // INSIDE tile 2, which streams in and raises far past the band.
    const auto handleA = makeThreeTile();
    auto* tiledA = svc.GetTiledTerrainData(handleA);
    for (int32 x = 0; x < 2; ++x)
        svc.LoadTile(handleA, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTiledTerrainEntity(worldA, handleA, 3);
    CreateFlattenModifier(worldA, 2560.0f, 512.0f, 300.0f); // inside tile 2, above band
    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f);       // full bake of tiles 0,1
    StreamInFullTile(svc, handleA, TileCoord{2, 0});
    systemA.Update(worldA, 1.0f / 60.0f);       // scoped bake of streamed tile 2
    // Let the deferred renormalize settle (quiescent frames — no more edits). The spread
    // flush (Fix 2) can span several frames; loop until quiescent (bounded) rather than a
    // fixed count.
    for (int32 i = 0; i < 512 && tiledA->SplatResplatPending; ++i)
        systemA.Update(worldA, 1.0f / 60.0f);
    if (std::getenv("GE_TERRAIN_DEFER_RESPLAT") == nullptr)
    {
        EXPECT_FALSE(tiledA->SplatResplatPending) << "range-extending stream-in must settle";
    }

    // Terrain B: all three tiles resident from the start -> one full bake.
    const auto handleB = makeThreeTile();
    auto* tiledB = svc.GetTiledTerrainData(handleB);
    for (int32 x = 0; x < 3; ++x)
        svc.LoadTile(handleB, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World worldB;
    RegisterModifierLifecycleEvents(worldB);
    CreateTiledTerrainEntity(worldB, handleB, 3);
    CreateFlattenModifier(worldB, 2560.0f, 512.0f, 300.0f);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    // EVERY tile — not just the streamed one — must match the full bake.
    for (int32 x = 0; x < 3; ++x)
    {
        auto* ta = Tile(tiledA, x, 0);
        auto* tb = Tile(tiledB, x, 0);
        ASSERT_EQ(ta->Splatmap.size(), tb->Splatmap.size());
        ASSERT_FALSE(ta->Splatmap.empty());
        EXPECT_EQ(0, std::memcmp(ta->Splatmap.data(), tb->Splatmap.data(), ta->Splatmap.size()))
            << "resident tile " << x << " not renormalized to the streamed tile's range";
    }
}

// STALE-RANGE STREAM-IN heal (the permanent snow/grass tile-boundary seam, round-6 #3):
// an authored raise pushes the committed splat range PAST the noise band, then a tile
// whose streaming job was submitted BEFORE the edit arrives afterward carrying a splat the
// background job baked against the pre-edit range snapshot. The arrival does NOT extend the
// committed range and no modifier overlaps it, so the existing extend-only heal never fires
// (BakeTiledStreamedTiles line ~2459) — the tile keeps a stale-range splat and its shared
// border shows a hard, position-independent shading seam against its resplatted neighbour.
// The bake must renormalize the arrival onto the committed range, byte-identical to a full
// bake. This is the inverse of TiledStreamInExtendingRangeRenormalizesResidentsToFullBake:
// there the arrival WIDENS the range (heals); here it does not (must still heal).
TEST(TerrainRegionBake, TiledStaleRangeStreamInRenormalizesToCommittedRange)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    auto makeThreeTile = [&]() {
        TiledTerrainConfig cfg{};
        cfg.WorldSizeX = 3.0f * kTiledTileWorld;
        cfg.WorldSizeZ = kTiledTileWorld;
        cfg.HeightScale = kTiledHeightScale;
        cfg.SamplesPerMeter = 1.0f;
        return svc.CreateTiledTerrain(cfg);
    };

    // Terrain A: tiles 0,1 resident; a raise in tile 0 pushes the committed range above the
    // band. Tile 2 then arrives with a STALE band-range splat (its job captured CachedGlobal
    // before the edit) and no modifier overlaps it.
    const auto handleA = makeThreeTile();
    auto* tiledA = svc.GetTiledTerrainData(handleA);
    for (int32 x = 0; x < 2; ++x)
        svc.LoadTile(handleA, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTiledTerrainEntity(worldA, handleA, 3);
    CreateFlattenModifier(worldA, 512.0f, 512.0f, 300.0f); // inside tile 0, above band
    // A global HeightNormalized row is what makes the committed range OBSERVABLE in the splat:
    // it is the domain the row normalizes against, so a stale range moves real bytes.
    CreateGlobalRulesModifier(worldA);
    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // full bake commits the widened range
    ASSERT_TRUE(tiledA->SplatBakeRangeValid);
    ASSERT_GT(tiledA->SplatBakeMaxH, kTileNoiseAmplitude) << "edit must widen range off-band";

    // Model the async streaming job: tile 2 arrives carrying splat content baked before the
    // edit, i.e. against the pre-edit noise band rather than the widened committed range. A
    // recognizable non-zero fill stands in for that stale bake — what makes the tile stale is
    // that its weights are not the ones the committed range yields, and the system must
    // overwrite them wholesale rather than leave them.
    StreamInFullTile(svc, handleA, TileCoord{2, 0});
    {
        auto* t2 = Tile(tiledA, 2, 0);
        ResetSplatmap(t2->Heightfield, t2->Splatmap, t2->SplatmapWidth, t2->SplatmapHeight);
        for (std::size_t i = 0; i + 3 < t2->Splatmap.size(); i += 4)
            t2->Splatmap[i + 2] = 200; // a channel the rules row does not target
        t2->SplatmapDirty = true;
    }

    UpdateUntilArrivalBaked(systemA, worldA, *Tile(tiledA, 2, 0)); // must renormalize tile 2
    for (int32 i = 0; i < 512 && tiledA->SplatResplatPending; ++i)
        systemA.Update(worldA, 1.0f / 60.0f);

    // Terrain B: all three tiles resident from the start -> one full bake (the oracle).
    const auto handleB = makeThreeTile();
    auto* tiledB = svc.GetTiledTerrainData(handleB);
    for (int32 x = 0; x < 3; ++x)
        svc.LoadTile(handleB, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World worldB;
    RegisterModifierLifecycleEvents(worldB);
    CreateTiledTerrainEntity(worldB, handleB, 3);
    CreateFlattenModifier(worldB, 512.0f, 512.0f, 300.0f);
    CreateGlobalRulesModifier(worldB);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    // The streamed tile 2 must be renormalized onto the committed range (== the full bake),
    // not left on its stale band-range splat.
    auto* ta2 = Tile(tiledA, 2, 0);
    auto* tb2 = Tile(tiledB, 2, 0);
    ASSERT_EQ(ta2->Splatmap.size(), tb2->Splatmap.size());
    ASSERT_FALSE(ta2->Splatmap.empty());
    EXPECT_GT(WeightedTexelCount(tb2->Splatmap), 0u)
        << "the oracle placed no material - the comparisons below would be zeros-to-zeros";
    EXPECT_EQ(0, std::memcmp(ta2->Splatmap.data(), tb2->Splatmap.data(), ta2->Splatmap.size()))
        << "stale-range stream-in tile not renormalized to the committed range "
           "(permanent tile-boundary shading seam)";

    // And the shared tile1|tile2 border must be continuous — the visible seam. Both border
    // columns are pure base noise (the modifier is in tile 0), so equal heights must yield
    // equal splat once both tiles share the committed range.
    auto* ta1 = Tile(tiledA, 1, 0);
    const uint32 w = ta1->SplatmapWidth;
    const uint32 h = ta1->SplatmapHeight;
    ASSERT_EQ(w, ta2->SplatmapWidth);
    ASSERT_EQ(h, ta2->SplatmapHeight);
    for (uint32 z = 0; z < h; ++z)
    {
        const uint8* rightCol = &ta1->Splatmap[(static_cast<size_t>(z) * w + (w - 1)) * 4];
        const uint8* leftCol = &ta2->Splatmap[(static_cast<size_t>(z) * w + 0) * 4];
        EXPECT_EQ(0, std::memcmp(rightCol, leftCol, 4))
            << "tile1|tile2 shared border splat differs at row " << z << " (seam)";
    }
}

// CREATION-TIME first-bake range (the freshly-created-terrain tile-boundary seam, round-7
// finding A -- NO edit): a tiled terrain's first modifier-system bake takes the streamed-tile-
// wake region path (BakeTiledRegion), where the committed splat range is not valid yet, so
// SplatBakeMin/MaxH are the uninitialized [0,0] sentinel. That range is the domain every
// HeightNormalized rule condition normalizes against, and the heightRange clamp floors a
// degenerate one at 0.001 -- so each tile would resolve identical shared-border heights to
// different rule weights, a permanent tile-boundary seam on a terrain nobody edited. The first
// bake must instead commit the deterministic noise band [0, kTileNoiseAmplitude] and evaluate
// every tile's rules against it.
//
// The terrain carries a global HeightNormalized rules row so the borders hold real weights:
// against the all-zero unbaked base this comparison would otherwise be zeros-to-zeros.
TEST(TerrainRegionBake, TiledCreationFirstBakeNormalizesSplatToNoiseBand)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    // 3-tile modifier-FREE terrain; every tile Full-resident from creation, each carrying base
    // noise with its OWN (distinct) observed height range -- exactly what makes a per-tile
    // local-range bake seam and a shared-band bake continuous.
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    for (int32 x = 0; x < 3; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    ASSERT_FALSE(tiled->SplatBakeRangeValid) << "a fresh terrain has no committed splat range";

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    CreateGlobalRulesModifier(world);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // first bake -> BakeTiledRegion first-bake

    // The committed range must be the deterministic noise band, never the [0,0] sentinel that
    // collapses the rules' normalized-height domain onto the 0.001 clamp.
    EXPECT_TRUE(tiled->SplatBakeRangeValid);
    EXPECT_FLOAT_EQ(tiled->SplatBakeMinH, 0.0f);
    EXPECT_FLOAT_EQ(tiled->SplatBakeMaxH, kTileNoiseAmplitude)
        << "first bake committed a non-band range -> tiles normalized against local ranges (seam)";

    // Every shared border is byte-identical: continuous world-space-noise heights under one
    // shared range give identical rule weights. A per-tile range makes each tile use its own
    // [min,max], so the same border height resolves to a different weight -> a seam.
    auto* t0 = Tile(tiled, 0, 0);
    auto* t1 = Tile(tiled, 1, 0);
    auto* t2 = Tile(tiled, 2, 0);
    EXPECT_GT(WeightedTexelCount(t0->Splatmap), 0u)
        << "the rules row placed nothing - the border comparison below would be zeros-to-zeros";
    const TerrainTileData* leftTiles[2] = {t0, t1};
    const TerrainTileData* rightTiles[2] = {t1, t2};
    for (int32 pair = 0; pair < 2; ++pair)
    {
        const TerrainTileData* a = leftTiles[pair];
        const TerrainTileData* b = rightTiles[pair];
        const uint32 w = a->SplatmapWidth;
        const uint32 h = a->SplatmapHeight;
        ASSERT_FALSE(a->Splatmap.empty());
        ASSERT_EQ(w, b->SplatmapWidth);
        ASSERT_EQ(h, b->SplatmapHeight);
        for (uint32 z = 0; z < h; ++z)
        {
            const uint8* rightCol = &a->Splatmap[(static_cast<size_t>(z) * w + (w - 1)) * 4];
            const uint8* leftCol = &b->Splatmap[(static_cast<size_t>(z) * w + 0) * 4];
            EXPECT_EQ(0, std::memcmp(rightCol, leftCol, 4))
                << "creation splat seam: tile pair " << pair << " border differs at row " << z;
        }
    }
}

// GPU EVAL-SKIP first-bake range (the planar GPU-bake analogue of the #602 creation seam): the
// GPU eval-skip path records the height regions and returns from BakeTiledFull/BakeTiledRegion
// BEFORE the CPU path's !SplatBakeRangeValid recompute, so on the FIRST GPU bake the committed
// range is still the uninitialized [0,0] sentinel -- the same local-fallback normalization family.
// Pre-fix the recorder sourced the batch's splat range straight from that sentinel and forced
// SplatEligible false, so the GPU splat preview was dead for the whole first stroke (and any
// loosening of the guard would feed [0,0] -- a degenerate divide -- to the kernel). The recorder
// must instead commit the deterministic noise band [0, kTileNoiseAmplitude] and mark the batch
// splat-eligible, so the GPU splat runs live from the first stroke against the band, never [0,0].
// Fails before (SplatBakeRangeValid false, SplatEligible false, batch range [0,0]), passes after.
TEST(TerrainRegionBake, GpuFirstBakeCommitsSplatBandNotSentinel)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    for (int32 x = 0; x < 3; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    ASSERT_FALSE(tiled->SplatBakeRangeValid) << "a fresh terrain has no committed splat range";

    // Arm the GPU eval-skip path deterministically: force the gate on and mark the terrain
    // whole-resident-eligible (extraction sets AtlasGpuBakeEligible at runtime; the modifier
    // system reads it). A flatten (a slice-1 GPU-bakeable height type) is the edit that makes
    // the first bake record tiles into the batch.
    SetGpuHeightBakeEnabledForTests(1);
    tiled->AtlasGpuBakeEligible = true;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);
    CreateFlattenModifier(world, 1536.0f, 512.0f, 20.0f); // in tile 1, target 20 -> in-band

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    SetGpuHeightBakeEnabledForTests(-1); // restore the env gate for the rest of the suite

    // The bake must have taken the GPU eval-skip path (recorded tiles for GPU dispatch).
    ASSERT_FALSE(tiled->GpuBakeBatch.Tiles.empty())
        << "GPU eval-skip recorded no tile -> the batch recording path was not exercised";

    // The committed range must be the deterministic noise band, never the [0,0] sentinel.
    EXPECT_TRUE(tiled->SplatBakeRangeValid);
    EXPECT_FLOAT_EQ(tiled->SplatBakeMinH, 0.0f);
    EXPECT_FLOAT_EQ(tiled->SplatBakeMaxH, kTileNoiseAmplitude);

    // The batch routes that band to the GPU splat kernel and marks the pass eligible (no paint
    // modifiers), so the GPU splat runs live rather than being masked off by an invalid range.
    EXPECT_TRUE(tiled->GpuBakeBatch.SplatEligible)
        << "GPU splat left ineligible on the first stroke -> the [0,0] sentinel disabled it";
    EXPECT_FLOAT_EQ(tiled->GpuBakeBatch.SplatMinH, 0.0f);
    EXPECT_FLOAT_EQ(tiled->GpuBakeBatch.SplatMaxH, kTileNoiseAmplitude)
        << "batch splat range is the [0,0] sentinel, not the committed noise band";
}

// A Shape::Global volume must force the CPU bake. The GPU height kernel's
// ComputeWeight knows SHAPE_CIRCLE and SHAPE_RECTANGLE only, and there is no
// SHAPE_GLOBAL to pack one as — so a global packed as either would bake its
// authored half-extents as a footprint and produce a DIFFERENT heightfield than
// the CPU path, silently, on whichever machines have the GPU bake armed.
//
// The refusal is observable exactly where the eligible case above is: an empty
// GpuBakeBatch means the modifier pack declined and the bake stayed on the CPU.
// Same fixture as GpuFirstBakeCommitsSplatBandNotSentinel, which is the positive
// control for it — that test proves this setup DOES record tiles when every
// modifier is packable.
TEST(TerrainRegionBake, GlobalVolumeForcesTheCpuBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    for (int32 x = 0; x < 3; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    SetGpuHeightBakeEnabledForTests(1);
    tiled->AtlasGpuBakeEligible = true;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);

    // A flatten modifier the kernel CAN bake, so the refusal below is provably
    // caused by the global volume rather than by an empty modifier set.
    CreateFlattenModifier(world, 1536.0f, 512.0f, 20.0f);

    auto globalVolume = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Global;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(globalVolume, vol);
    world.AddComponentImmediate<Components::WorldTransform>(globalVolume,
                                                            Components::WorldTransform{});
    Components::TerrainHeightOffsetEffect offset{};
    offset.Offset = 5.0f;
    offset.Blend = Components::TerrainModifierBlend::Add;
    world.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(globalVolume, offset);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    SetGpuHeightBakeEnabledForTests(-1); // restore the env gate for the rest of the suite

    EXPECT_TRUE(tiled->GpuBakeBatch.Tiles.empty())
        << "a Shape::Global volume was packed for the GPU height bake — the kernel has no "
           "SHAPE_GLOBAL, so it would bake the volume's authored rect extents as a footprint";
}

// A non-noise base must force the CPU bake. The GPU height kernel composes the tile
// noise (BaseFreq/BaseAmp/BaseOctaves/BaseSeed) as every tile's base and has no
// heightmap input, so a heightmap-based terrain baked there would render the noise
// again — on exactly the machines that arm the GPU bake (an atlas-sized terrain).
//
// Two arms over one fixture: the noise base records tiles for the GPU (the positive
// control: this setup does reach the GPU path), the heightmap base records none and its
// CPU bake fills the untouched tile from the heightmap.
TEST(TerrainRegionBake, AHeightmapBaseKeepsTheTiledBakeOnTheCpu)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID heightmapGuid = GUID::Generate();
    ASSERT_NE(SeedNoiseHeightmap(svc, heightmapGuid), nullptr);

    struct GpuArm
    {
        TiledTerrainBase Base;
        bool ExpectGpuTiles;
    };
    const GpuArm arms[] = {
        {svc.ResolveTiledTerrainBase(Components::TerrainBaseSource::ProceduralNoise, heightmapGuid), true},
        {svc.ResolveTiledTerrainBase(Components::TerrainBaseSource::HeightmapAsset, heightmapGuid), false},
    };
    for (const GpuArm& arm : arms)
    {
        const TiledTerrainConfig cfg = MakeBaseTestTiledConfig(arm.Base);
        const TiledTerrainHandle handle = svc.CreateTiledTerrain(cfg);
        auto* tiled = svc.GetTiledTerrainData(handle);
        ASSERT_NE(tiled, nullptr);
        tiled->WorldOriginX = kBaseTerrainOriginX;
        tiled->WorldOriginZ = kBaseTerrainOriginZ;
        for (int32 x = 0; x < 2; ++x)
            svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

        SetGpuHeightBakeEnabledForTests(1);
        tiled->AtlasGpuBakeEligible = true;

        ECS::World world;
        RegisterModifierLifecycleEvents(world);
        CreateTiledTerrainEntity(world, handle, 2);
        CreateFlattenModifier(world, kBaseTerrainOriginX + 1.5f * kTiledTileWorld,
                              kBaseTerrainOriginZ + 0.5f * kTiledTileWorld, 20.0f);
        TerrainModifierSystem system;
        system.Update(world, 1.0f / 60.0f);

        SetGpuHeightBakeEnabledForTests(-1); // restore the env gate for the rest of the suite

        EXPECT_EQ(!tiled->GpuBakeBatch.Tiles.empty(), arm.ExpectGpuTiles)
            << (arm.ExpectGpuTiles ? "the noise-base arm never reached the GPU bake, so the "
                                     "heightmap arm's empty batch proves nothing"
                                   : "a heightmap-based terrain was handed to the GPU height "
                                     "kernel, whose only base is the tile noise");
        if (!arm.ExpectGpuTiles)
            ExpectTileMatchesUntiledBase(*Tile(tiled, 0, 0), *tiled, MakeUntiledBaseOracle(cfg),
                                         37, "CPU bake of a heightmap base");
        svc.DestroyTiledTerrain(handle);
    }
}

// A SURFACE RULES effect must not refuse the GPU HEIGHT bake. Rules write the splatmap and never
// the heightfield, but the pack used to `continue` past only PaintLayer and treat every other
// effect kind as an unimplemented HEIGHT effect — so authoring one rules modifier anywhere on a
// terrain dropped the WHOLE terrain's height bake to the CPU, a cliff with no cause in the maths.
//
// Same fixture as GpuFirstBakeCommitsSplatBandNotSentinel (its positive control: that test proves
// this setup records tiles when every modifier is packable) and the inverse of
// GlobalVolumeForcesTheCpuBake (its negative control: a genuinely unpackable modifier empties the
// batch). A non-empty batch here therefore means the pack accepted the rules-carrying volume.
TEST(TerrainRegionBake, ARulesEffectDoesNotRefuseTheGpuHeightBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 3.0f * kTiledTileWorld;
    cfg.WorldSizeZ = kTiledTileWorld;
    cfg.HeightScale = kTiledHeightScale;
    cfg.SamplesPerMeter = 1.0f;
    const auto handle = svc.CreateTiledTerrain(cfg);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    for (int32 x = 0; x < 3; ++x)
        svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

    SetGpuHeightBakeEnabledForTests(1);
    tiled->AtlasGpuBakeEligible = true;

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 3);

    // A rectangle volume carrying BOTH a height effect the kernel can bake and a rules effect it
    // now composites into the splat. Pre-fix the rules effect alone emptied the batch.
    auto volume = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Rectangle;
    vol.RectHalfX = 200.0f;
    vol.RectHalfZ = 200.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(volume, vol);
    Components::WorldTransform xf{};
    xf.matrix[12] = 1536.0f;
    xf.matrix[14] = 512.0f;
    world.AddComponentImmediate<Components::WorldTransform>(volume, xf);

    Components::TerrainFlattenEffect flatten{};
    flatten.TargetHeight = 20.0f;
    // An ABSOLUTE target: UseVolumeHeight defaults true, and the kernel has no reference-height
    // lookup, so leaving it on would refuse the bake for a reason that has nothing to do with rules.
    flatten.UseVolumeHeight = false;
    flatten.Blend = Components::TerrainModifierBlend::Add;
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(volume, flatten);

    Components::TerrainSurfaceRulesEffect rules{};
    rules.RuleCount = 1;
    rules.Rules[0].MaterialSlot = 1;
    rules.Rules[0].Strength = 1.0f;
    world.AddComponentImmediate<Components::TerrainSurfaceRulesEffect>(volume, rules);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    SetGpuHeightBakeEnabledForTests(-1); // restore the env gate for the rest of the suite

    EXPECT_FALSE(tiled->GpuBakeBatch.Tiles.empty())
        << "a rules effect refused the GPU HEIGHT bake — it contributes no height at all, so the "
           "whole terrain fell back to the CPU for a modifier that never touches a heightfield";
    // And the rules reached the splat kernel rather than being silently dropped on the way.
    EXPECT_TRUE(tiled->GpuBakeBatch.SplatEligible);
    ASSERT_EQ(tiled->GpuBakeBatch.SurfaceRules.size(), 1u);
    EXPECT_EQ(tiled->GpuBakeBatch.SurfaceRules[0].MaterialSlot, 1u);
    // Exactly one packed HEIGHT row: the flatten. The rules effect packs no height record.
    EXPECT_EQ(tiled->GpuBakeBatch.Modifiers.size(), 1u)
        << "the rules effect packed a height modifier row it has no business packing";
}

// A POOLED flatten (Blend = Average) must force the CPU bake. Pooling is a
// cross-modifier accumulate-then-apply and the kernel evaluates one modifier
// record per texel with no scratch to accumulate into — and its blend switch
// falls THROUGH to Add for any id it does not implement, so an Average row would
// not be ignored, it would bake a route as an additive raise on whichever
// machines have the GPU bake armed.
//
// Same fixture and the same observable as GlobalVolumeForcesTheCpuBake: an empty
// GpuBakeBatch means the modifier pack declined. ARulesEffectDoesNotRefuseTheGpu
// HeightBake above is the positive control — it proves this setup DOES record
// tiles when every modifier is packable.
//
// The Set arm runs first as this test's own control, so a refusal here cannot be
// blamed on the volume, the target or the fixture.
TEST(TerrainRegionBake, APooledFlattenForcesTheCpuBake)
{
    const auto bakeWithBlend = [](Components::TerrainModifierBlend blend) {
        ScopedTerrainService scoped;
        auto& svc = TerrainService::Get();

        TiledTerrainConfig cfg{};
        cfg.WorldSizeX = 3.0f * kTiledTileWorld;
        cfg.WorldSizeZ = kTiledTileWorld;
        cfg.HeightScale = kTiledHeightScale;
        cfg.SamplesPerMeter = 1.0f;
        const auto handle = svc.CreateTiledTerrain(cfg);
        auto* tiled = svc.GetTiledTerrainData(handle);
        if (!tiled)
            return false;
        for (int32 x = 0; x < 3; ++x)
            svc.LoadTile(handle, TileCoord{x, 0})->LodState = TileLodState::Full;

        SetGpuHeightBakeEnabledForTests(1);
        tiled->AtlasGpuBakeEligible = true;

        ECS::World world;
        RegisterModifierLifecycleEvents(world);
        CreateTiledTerrainEntity(world, handle, 3);

        auto volume = world.CreateEntity();
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::Rectangle;
        vol.RectHalfX = 200.0f;
        vol.RectHalfZ = 200.0f;
        world.AddComponentImmediate<Components::TerrainModifierVolume>(volume, vol);
        Components::WorldTransform xf{};
        xf.matrix[12] = 1536.0f;
        xf.matrix[14] = 512.0f;
        world.AddComponentImmediate<Components::WorldTransform>(volume, xf);

        Components::TerrainFlattenEffect flatten{};
        flatten.TargetHeight = 20.0f;
        // An ABSOLUTE target: the kernel has no reference-height lookup, so
        // leaving UseVolumeHeight on would refuse the bake for a reason that has
        // nothing to do with the blend under test.
        flatten.UseVolumeHeight = false;
        flatten.Blend = blend;
        world.AddComponentImmediate<Components::TerrainFlattenEffect>(volume, flatten);

        TerrainModifierSystem system;
        system.Update(world, 1.0f / 60.0f);

        SetGpuHeightBakeEnabledForTests(-1); // restore the env gate for the rest of the suite
        return !tiled->GpuBakeBatch.Tiles.empty();
    };

    EXPECT_TRUE(bakeWithBlend(Components::TerrainModifierBlend::Set))
        << "positive control: the same absolute flatten with a Set blend must record GPU bake "
           "tiles, or the refusal below proves nothing";
    EXPECT_FALSE(bakeWithBlend(Components::TerrainModifierBlend::Average))
        << "a pooled flatten was packed for the GPU height bake — the kernel's blend switch falls "
           "through to Add, so it would bake the route as an additive raise instead of averaging";
}

// MID-PENDING STREAM-IN guard (the exact stroke -> range-shift -> stream-in-mid-
// pending -> settle interleaving): a raise stroke defers the whole-terrain
// renormalize; while it is pending a fresh tile streams in; the settle must then
// renormalize EVERY resident tile — including the one that arrived mid-pending —
// onto the one final range, byte-identical to a full bake. Guards that a tile
// arriving during the pending window is covered by the settle (not left behind).
TEST(TerrainRegionBake, TiledStreamInDuringPendingStrokeSettlesToFullBake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    auto makeFourTile = [&]() {
        TiledTerrainConfig cfg{};
        cfg.WorldSizeX = 4.0f * kTiledTileWorld;
        cfg.WorldSizeZ = kTiledTileWorld;
        cfg.HeightScale = kTiledHeightScale;
        cfg.SamplesPerMeter = 1.0f;
        return svc.CreateTiledTerrain(cfg);
    };

    // Terrain A: tiles 0,1,2 resident + full bake; a raise in tile 1 shifts the
    // range (defers); then tile 3 streams in mid-pending; then settle.
    const auto handleA = makeFourTile();
    auto* tiledA = svc.GetTiledTerrainData(handleA);
    for (int32 x = 0; x < 3; ++x)
        svc.LoadTile(handleA, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World worldA;
    RegisterModifierLifecycleEvents(worldA);
    CreateTiledTerrainEntity(worldA, handleA, 4);
    auto modA = CreateFlattenModifier(worldA, 1536.0f, 512.0f, 100.0f); // tile 1
    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // full bake commits the range

    auto* fmA = worldA.GetComponentForWrite<Components::TerrainFlattenEffect>(modA);
    ASSERT_NE(fmA, nullptr);
    fmA->TargetHeight = 300.0f;           // grows the global max -> range shift
    systemA.Update(worldA, 1.0f / 60.0f); // deferred re-splat
    if (std::getenv("GE_TERRAIN_DEFER_RESPLAT") == nullptr)
    {
        ASSERT_TRUE(tiledA->SplatResplatPending);
    }

    StreamInFullTile(svc, handleA, TileCoord{3, 0}); // arrives mid-pending
    // Bake the arrival, then settle. The spread flush (Fix 2) can span several frames; one
    // guaranteed Update bakes the stream-in tile before the settle countdown completes,
    // then loop until quiescent (bounded).
    systemA.Update(worldA, 1.0f / 60.0f);
    for (int32 i = 0; i < 512 && tiledA->SplatResplatPending; ++i)
        systemA.Update(worldA, 1.0f / 60.0f);
    EXPECT_FALSE(tiledA->SplatResplatPending);

    // Terrain B: all four tiles resident from the start -> one full bake.
    const auto handleB = makeFourTile();
    auto* tiledB = svc.GetTiledTerrainData(handleB);
    for (int32 x = 0; x < 4; ++x)
        svc.LoadTile(handleB, TileCoord{x, 0})->LodState = TileLodState::Full;

    ECS::World worldB;
    RegisterModifierLifecycleEvents(worldB);
    CreateTiledTerrainEntity(worldB, handleB, 4);
    CreateFlattenModifier(worldB, 1536.0f, 512.0f, 300.0f);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    for (int32 x = 0; x < 4; ++x)
    {
        auto* ta = Tile(tiledA, x, 0);
        auto* tb = Tile(tiledB, x, 0);
        ASSERT_EQ(ta->Splatmap.size(), tb->Splatmap.size());
        ASSERT_FALSE(ta->Splatmap.empty());
        EXPECT_EQ(0, std::memcmp(ta->Splatmap.data(), tb->Splatmap.data(), ta->Splatmap.size()))
            << "tile " << x << " not settled to the full-bake range after mid-pending stream-in";
    }
}

// COARSE-TILE RENDERING: bilinear-upsampling a coarse tile's heightfield to the
// unified region resolution yields a non-zero surface (not a flat hole) that
// approximates the Full bake within a documented tolerance. Endpoints are exact,
// so shared tile edges stay crack-free.
TEST(TerrainRegionBake, CoarseUpsampleApproximatesFullAndPreservesEndpoints)
{
    constexpr uint32 kCoarseDim = 129; // production coarse resolution
    constexpr uint32 kFullDim = 513;   // ~4:1 upsample (production is up to 8:1)
    constexpr float32 kTileWorld = 512.0f;

    auto fill = [&](uint32 dim) {
        Terrain::HeightfieldData hf;
        hf.Resize(dim, dim, 0.0f);
        hf.FillWithNoiseWorldSpace(kTileNoiseFrequency, kTileNoiseAmplitude,
                                   0.0f, 0.0f, kTileWorld, kTileWorld,
                                   kTileNoiseOctaves, kTileNoiseSeed);
        return hf;
    };
    const Terrain::HeightfieldData full = fill(kFullDim);
    const Terrain::HeightfieldData coarse = fill(kCoarseDim);

    std::vector<float32> up;
    UpsampleHeightfieldBilinear(coarse.GetRawSamples(), kCoarseDim, up, kFullDim);
    ASSERT_EQ(up.size(), static_cast<std::size_t>(kFullDim) * kFullDim);

    float32 sumAbs = 0.0f, maxErr = 0.0f, hMin = 1e30f, hMax = -1e30f;
    for (uint32 z = 0; z < kFullDim; ++z)
        for (uint32 x = 0; x < kFullDim; ++x)
        {
            const float32 f = full.GetSample(x, z);
            const float32 u = up[static_cast<std::size_t>(z) * kFullDim + x];
            hMin = std::min(hMin, u);
            hMax = std::max(hMax, u);
            maxErr = std::max(maxErr, std::abs(u - f));
            sumAbs += std::abs(u);
        }
    EXPECT_GT(sumAbs, 0.0f) << "coarse upsample must be non-zero (not a flat/black hole)";

    const float32 range = hMax - hMin;
    ASSERT_GT(range, 0.0f);
    // Documented tolerance: the coarse cell undersamples the top noise octave, so
    // error is bounded by (not equal to) the per-cell variation. 30% of the field
    // range is a comfortable ceiling for a transient, distance-only approximation.
    EXPECT_LT(maxErr, 0.30f * range) << "coarse upsample diverges too far from Full";

    // Endpoints exact → shared tile edges are bit-identical across slots.
    EXPECT_FLOAT_EQ(up[0], coarse.GetSample(0, 0));
    EXPECT_FLOAT_EQ(up[kFullDim - 1], coarse.GetSample(kCoarseDim - 1, 0));
    EXPECT_FLOAT_EQ(up[static_cast<std::size_t>(kFullDim - 1) * kFullDim],
                    coarse.GetSample(0, kCoarseDim - 1));
    EXPECT_FLOAT_EQ(up[static_cast<std::size_t>(kFullDim) * kFullDim - 1],
                    coarse.GetSample(kCoarseDim - 1, kCoarseDim - 1));
}

// COARSE SHARED-EDGE PARITY (the C8 shared-edge lesson): two adjacent coarse
// tiles upsampled independently must agree bit-for-bit along their shared edge,
// so the unified texture has no seam while tiles stream at coarse LOD.
TEST(TerrainRegionBake, CoarseUpsampleSharedEdgeMatchesAcrossTiles)
{
    constexpr uint32 kCoarseDim = 129;
    constexpr uint32 kRegionDim = 257;
    constexpr float32 kTileWorld = 256.0f;

    auto coarseAt = [&](float32 originX) {
        Terrain::HeightfieldData hf;
        hf.Resize(kCoarseDim, kCoarseDim, 0.0f);
        hf.FillWithNoiseWorldSpace(kTileNoiseFrequency, kTileNoiseAmplitude,
                                   originX, 0.0f, kTileWorld, kTileWorld,
                                   kTileNoiseOctaves, kTileNoiseSeed);
        return hf;
    };
    const Terrain::HeightfieldData a = coarseAt(0.0f);
    const Terrain::HeightfieldData b = coarseAt(kTileWorld); // adjacent along +X

    std::vector<float32> upA, upB;
    UpsampleHeightfieldBilinear(a.GetRawSamples(), kCoarseDim, upA, kRegionDim);
    UpsampleHeightfieldBilinear(b.GetRawSamples(), kCoarseDim, upB, kRegionDim);

    for (uint32 z = 0; z < kRegionDim; ++z)
    {
        const float32 aRight = upA[static_cast<std::size_t>(z) * kRegionDim + (kRegionDim - 1)];
        const float32 bLeft = upB[static_cast<std::size_t>(z) * kRegionDim + 0];
        EXPECT_FLOAT_EQ(aRight, bLeft) << "coarse shared edge diverges at row z=" << z;
    }
}

// COARSE-ADJACENT-TO-FULL STOMP (coordinator REQUIRED 1 — the common frontier
// case the coarse<->coarse oracle could not see): a coarse tile's patch must NOT
// overwrite the boundary texel shared with a resident-Full neighbor. That texel
// carries the Full side's exact + modifier-applied height; a coarse write there
// (approximate, modifier-free) leaves a persistent seam trench. The interior
// clamp excludes it. Pre-fix (full-region patch) the shared column is stomped.
TEST(TerrainRegionBake, CoarsePatchDoesNotStompFullNeighborSharedEdge)
{
    constexpr uint32 kCoarseDim = 33;
    constexpr uint32 kRegionDim = 129;              // = tile resolution
    constexpr uint32 kInterior = kRegionDim - 1;    // shared-edge stride
    constexpr uint32 kUnifiedW = 2 * kInterior + 1; // two tiles overlap the middle column
    constexpr float32 kTileWorld = 256.0f;

    // A Full sentinel a coarse upsample will never produce.
    constexpr float32 kFullSentinel = -999.0f;
    std::vector<float32> unified(static_cast<std::size_t>(kUnifiedW) * kRegionDim, kFullSentinel);

    // Tile 0 (coarse) at world origin 0.
    Terrain::HeightfieldData coarse0;
    coarse0.Resize(kCoarseDim, kCoarseDim, 0.0f);
    coarse0.FillWithNoiseWorldSpace(kTileNoiseFrequency, kTileNoiseAmplitude, 0.0f, 0.0f,
                                    kTileWorld, kTileWorld, kTileNoiseOctaves, kTileNoiseSeed);
    std::vector<float32> up0;
    UpsampleHeightfieldBilinear(coarse0.GetRawSamples(), kCoarseDim, up0, kRegionDim);

    // Tile 0's +X neighbor (tile 1) is resident Full → the clamp drops tile 0's
    // rightmost (shared) column.
    CoarsePatchNeighbors nb;
    nb.RightFull = true;
    const CoarsePatchRect pr = ComputeCoarsePatchRect(kRegionDim, nb);
    ASSERT_EQ(pr.X0, 0u);
    ASSERT_EQ(pr.Z0, 0u);
    ASSERT_EQ(pr.Width, kRegionDim - 1u) << "a +X-Full neighbor must drop the shared column";
    ASSERT_EQ(pr.Height, kRegionDim);

    // Patch tile 0 into the unified buffer at dstX=0 using the clamped rect
    // (mirrors the extraction copy).
    std::vector<float32> patch;
    ExtractSubBlock(up0.data(), kRegionDim, pr.X0, pr.Z0, pr.Width, pr.Height, patch);
    for (uint32 z = 0; z < pr.Height; ++z)
        for (uint32 x = 0; x < pr.Width; ++x)
            unified[static_cast<std::size_t>(pr.Z0 + z) * kUnifiedW + (pr.X0 + x)] =
                patch[static_cast<std::size_t>(z) * pr.Width + x];

    // The shared column (texel kInterior) belongs to Full tile 1 — untouched.
    for (uint32 z = 0; z < kRegionDim; ++z)
        EXPECT_FLOAT_EQ(unified[static_cast<std::size_t>(z) * kUnifiedW + kInterior], kFullSentinel)
            << "coarse patch stomped the Full neighbor's shared column at z=" << z;

    // Discrimination: WITHOUT the clamp (no Full neighbors → full region) the patch
    // spans the shared column, and the coarse value there is not the Full sentinel.
    const CoarsePatchRect fullRect = ComputeCoarsePatchRect(kRegionDim, CoarsePatchNeighbors{});
    ASSERT_EQ(fullRect.Width, kRegionDim);
    EXPECT_NE(up0[kInterior], kFullSentinel)
        << "the coarse value at the shared column differs from Full — proving the stomp is real";
}

// Paint-only region bake proportionality (review Fix 2): a paint-only (splat)
// edit on a tiled terrain must NOT bump any tile's HeightfieldVersion, must NOT
// bump tiled.Revision — heights are untouched, so a re-upload and normal regen would
// be pure waste. Pre-fix (unconditional rescan + version bumps) fails this.
TEST(TerrainRegionBake, TiledPaintOnlyEditDoesNotBumpHeightOrRevision)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const auto handle = CreateResidentTiledTerrain(svc, /*tilesX*/ 2);
    auto* tiled = svc.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTiledTerrainEntity(world, handle, 2);

    // A splat-only paint modifier well inside tile (0,0) ([0,1024]).
    auto paintEntity = world.CreateEntity();
    {
        Components::TerrainModifierVolume paintVol{};
        paintVol.Shape = Components::TerrainVolumeShape::Circle;
        paintVol.Radius = 60.0f;
        paintVol.Falloff = 15.0f;
        world.AddComponentImmediate<Components::TerrainModifierVolume>(paintEntity, paintVol);

        Components::TerrainPaintLayerEffect paint{};
        paint.LayerIndex = 1;
        paint.Strength = 0.8f;
        world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(paintEntity, paint);
        Components::WorldTransform xf{};
        xf.matrix[12] = 300.0f;
        xf.matrix[14] = 400.0f;
        world.AddComponentImmediate<Components::WorldTransform>(paintEntity, xf);
    }

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // initial full bake

    const uint64 v00 = Tile(tiled, 0, 0)->HeightfieldVersion;
    const uint64 v10 = Tile(tiled, 1, 0)->HeightfieldVersion;
    const uint64 revisionAfterFull = tiled->Revision;
    ASSERT_GT(v00, 0u);

    // Paint drag (move the splat stamp, staying inside tile 0) → region bake,
    // splat-only.
    auto* xf = world.GetComponentForWrite<Components::WorldTransform>(paintEntity);
    ASSERT_NE(xf, nullptr);
    xf->matrix[12] = 250.0f;
    xf->matrix[14] = 330.0f;
    system.Update(world, 1.0f / 60.0f);

    EXPECT_EQ(Tile(tiled, 0, 0)->HeightfieldVersion, v00)
        << "paint-only edit must not bump the edited tile's HeightfieldVersion";
    EXPECT_EQ(Tile(tiled, 1, 0)->HeightfieldVersion, v10);
    EXPECT_EQ(tiled->Revision, revisionAfterFull)
        << "paint-only edit must not bump tiled.Revision";
    // The splat still updated (the paint actually ran).
    EXPECT_TRUE(Tile(tiled, 0, 0)->SplatmapDirty);
}

// ---------------------------------------------------------------------------
// Normal regen (quality-sweep slice 1, scope 4): a tile's normalmap is baked ONCE at cook time from
// BASE noise on the job thread and NEVER regenerated after a modifier bake, so the tiled path shipped
// stale base-noise normals (the dark-shard shading in the #505 investigation) and the coarse path
// shipped ZEROED (flat-up) normals. The fix regenerates the normal from the tile's POST-modifier heights
// at upload (both Full and coarse paths). These lock the regen SEMANTICS the extraction now relies on.
// ---------------------------------------------------------------------------

// DISCRIMINATOR: shipping the cook-time base-noise normal (the bug) keeps the base normals; the fix
// ships normals regenerated from the post-modifier heights. A modifier edit must change them, else the
// regen is a no-op and the dark-shard persists.
TEST(TerrainNormalRegen, PostModifierHeightsYieldDifferentNormalsThanStaleBase)
{
    Terrain::HeightfieldData hf(65, 65);
    hf.FillWithNoise(4.0f, 1.0f, 5, 7); // the cook-time base noise (what the stale normalmap reflects)

    std::vector<uint8> baseNormals;
    uint32 w = 0, h = 0;
    GenerateNormalmapFromHeightfield(hf, 128.0f, 128.0f, 32.0f, baseNormals, w, h);

    // A modifier raises a central region -> the post-modifier heightfield the extraction regenerates from.
    for (int32 z = 25; z <= 40; ++z)
        for (int32 x = 25; x <= 40; ++x)
            hf.SetSample(static_cast<uint32>(x), static_cast<uint32>(z),
                         hf.GetSample(static_cast<uint32>(x), static_cast<uint32>(z)) + 1.0f);

    std::vector<uint8> postNormals;
    uint32 pw = 0, ph = 0;
    GenerateNormalmapFromHeightfield(hf, 128.0f, 128.0f, 32.0f, postNormals, pw, ph);
    ASSERT_EQ(baseNormals.size(), postNormals.size());

    uint32 diffs = 0;
    for (size_t i = 0; i < baseNormals.size(); ++i)
        if (baseNormals[i] != postNormals[i])
            ++diffs;
    EXPECT_GT(diffs, 0u)
        << "post-modifier normals identical to the base-noise normals -> the regen picked up nothing "
           "(the stale-normal dark-shard bug)";
}

// DISCRIMINATOR: the shipped coarse path uploaded ALL-ZERO (flat-up) normals; the fix regenerates real
// gradient normals from the upsampled coarse heights. A sloped coarse field must yield non-zero components.
TEST(TerrainNormalRegen, CoarsePathProducesRealGradientNotZeros)
{
    Terrain::HeightfieldData coarse(33, 33);
    for (uint32 z = 0; z < 33; ++z)
        for (uint32 x = 0; x < 33; ++x)
            coarse.SetSample(x, z, 0.7f * (static_cast<float32>(x) / 32.0f)); // +X ramp -> real gradient

    std::vector<uint8> normals;
    uint32 w = 0, h = 0;
    GenerateNormalmapFromHeightfield(coarse, 64.0f, 64.0f, 40.0f, normals, w, h);
    ASSERT_EQ(normals.size(), static_cast<size_t>(33) * 33 * 4);

    bool anyNonZero = false;
    for (uint8 b : normals)
        if (b != 0)
        {
            anyNonZero = true;
            break;
        }
    EXPECT_TRUE(anyNonZero)
        << "coarse normals all zero -> the flat-up stand-in, not the regenerated gradient";
}

// IEEE-754 binary16 -> float32 (normal components are in [-1,1], so subnormals appear for a near-flat
// normal but never inf/NaN). Mirrors the FloatToHalf the normal generator writes.
float32 DecodeHalf(uint16 half)
{
    const uint32 sign = static_cast<uint32>(half & 0x8000u) << 16;
    const uint32 exp = (half >> 10) & 0x1Fu;
    uint32 mant = half & 0x3FFu;
    uint32 bits;
    if (exp == 0u)
    {
        if (mant == 0u)
            bits = sign; // +/-0
        else
        {
            int32 e = -1;
            do { mant <<= 1; ++e; } while ((mant & 0x400u) == 0u);
            mant &= 0x3FFu;
            bits = sign | (static_cast<uint32>(127 - 15 - e) << 23) | (mant << 13);
        }
    }
    else
    {
        bits = sign | (static_cast<uint32>(exp + (127 - 15)) << 23) | (mant << 13);
    }
    float32 out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// CONTENT ASSERT (finding-1 diagnosis, SPM=2 tile-shaped normal seam): a tile that reaches Full carries a
// DETAILED (tilted) normal because its normal is generated over the TILE world footprint, while the
// whole-terrain coarse-field fallback a slot-LESS tile resolves through is generated over the whole
// TERRAIN footprint — so its per-texel slope is ~ (terrainSize / tileSize) smaller and the normal
// collapses toward flat-up. This is the mechanism behind a flat trapezoid in the terrain: the flat tile is
// slot-less and reads the (flat by construction) coarse normal, NOT a stale slot normal — every atlas
// slot-writing path co-uploads a freshly regenerated full normal with the height (patchSlot /
// patchSlotRegion in TerrainExtractionSystem), so a resident Full tile cannot carry a coarse normal. This
// oracle locks both halves of that invariant: the Full-tile normal is markedly non-flat (a regression
// that generated it at the wrong world scale would flat-shade EVERY resident tile), and the coarse
// fallback is, by construction, the flat one. The 20x ratio is her config (10240 m terrain / 512 m tile).
TEST(TerrainNormalRegen, AtlasFullTileNormalIsDetailedWhileCoarseFallbackIsFlat)
{
    constexpr uint32 dim = 65;
    constexpr float32 kTileWorld = 512.0f;       // one tile
    constexpr float32 kTerrainWorld = 10240.0f;  // 20x20 tiles (her config)
    constexpr float32 kOracleHeightScale = 256.0f;

    // Real high-frequency relief across the tile (what a Full tile's cooked heightfield holds).
    Terrain::HeightfieldData hf(dim, dim);
    for (uint32 z = 0; z < dim; ++z)
        for (uint32 x = 0; x < dim; ++x)
        {
            const float32 u = static_cast<float32>(x) / static_cast<float32>(dim - 1);
            const float32 v = static_cast<float32>(z) / static_cast<float32>(dim - 1);
            hf.SetSample(x, z, 0.5f + 0.25f * std::sin(u * 12.0f) * std::cos(v * 9.0f));
        }

    std::vector<uint8> tileNormals, coarseNormals;
    uint32 w = 0, h = 0;
    GenerateNormalmapFromHeightfield(hf, kTileWorld, kTileWorld, kOracleHeightScale, tileNormals, w, h);
    GenerateNormalmapFromHeightfield(hf, kTerrainWorld, kTerrainWorld, kOracleHeightScale, coarseNormals, w, h);
    ASSERT_EQ(tileNormals.size(), coarseNormals.size());
    ASSERT_EQ(tileNormals.size(), static_cast<size_t>(dim) * dim * 4u);

    auto meanTangential = [](const std::vector<uint8>& normals) -> double {
        const uint16* halves = reinterpret_cast<const uint16*>(normals.data());
        const size_t texels = normals.size() / 4u;
        double acc = 0.0;
        for (size_t i = 0; i < texels; ++i)
        {
            const double nx = DecodeHalf(halves[i * 2 + 0]);
            const double nz = DecodeHalf(halves[i * 2 + 1]);
            acc += std::sqrt(nx * nx + nz * nz);
        }
        return texels ? acc / static_cast<double>(texels) : 0.0;
    };

    const double tileTangential = meanTangential(tileNormals);
    const double coarseTangential = meanTangential(coarseNormals);

    EXPECT_GT(tileTangential, 0.05)
        << "Full-tile normal is (near) flat -> a resident tile would flat-shade like the coarse fallback";
    EXPECT_LT(coarseTangential, tileTangential * 0.25)
        << "coarse fallback normal is not markedly flatter than the tile normal -> the flat-trapezoid "
           "diagnosis (a slot-less tile reads the whole-terrain coarse normal) would not hold";
}

// DISCRIMINATOR (R2, atlas normal seam): the atlas keeps each tile's normal in a DISJOINT slot, so the
// two copies of a shared tile boundary must be bit-equal or lighting/specular seams grid every tile
// edge on sloped terrain. Passing each tile its Full axis neighbour makes both slots compute the shared
// boundary texel from the SAME samples -> bit-identical. WITHOUT neighbours the border central-difference
// clamps one-sided on each side and the two copies diverge (the seam). Two REAL adjacent tiles with
// different content (payload, not the analytic packer oracle).
TEST(TerrainNormalRegen, AdjacentTileBoundaryNormalBitEqualWithNeighbor)
{
    constexpr uint32 dim = 65;
    constexpr int32 interior = static_cast<int32>(dim) - 1; // 64: A's x=64 == B's x=0 (shared column)
    auto H = [](int32 gx, int32 gz) {
        return 0.30f + 0.15f * std::sin(gx * 0.09f) + 0.10f * std::cos(gz * 0.07f);
    };
    Terrain::HeightfieldData A(dim, dim), B(dim, dim);
    for (uint32 z = 0; z < dim; ++z)
        for (uint32 x = 0; x < dim; ++x)
        {
            A.SetSample(x, z, H(static_cast<int32>(x), static_cast<int32>(z)));              // A: gx = x
            B.SetSample(x, z, H(interior + static_cast<int32>(x), static_cast<int32>(z)));   // B: gx = 64 + x
        }

    auto texel = [](const std::vector<uint8>& n, uint32 x, uint32 z) {
        uint32 v;
        std::memcpy(&v, &n[(static_cast<size_t>(z) * dim + x) * 4], sizeof(v));
        return v;
    };

    // WITH neighbours: A's +X neighbour is B (neighborRight), B's -X neighbour is A (neighborLeft).
    std::vector<uint8> nA, nB;
    uint32 w = 0, h = 0;
    GenerateNormalmapFromHeightfield(A, 100.0f, 100.0f, 20.0f, nA, w, h, nullptr, &B, nullptr, nullptr);
    GenerateNormalmapFromHeightfield(B, 100.0f, 100.0f, 20.0f, nB, w, h, &A, nullptr, nullptr, nullptr);
    for (uint32 z = 0; z < dim; ++z)
        EXPECT_EQ(texel(nA, dim - 1, z), texel(nB, 0, z))
            << "shared boundary normal must be bit-equal across the two slots, row " << z;

    // WITHOUT neighbours: the clamped one-sided differences diverge (proves the test is discriminating).
    std::vector<uint8> nA0, nB0;
    GenerateNormalmapFromHeightfield(A, 100.0f, 100.0f, 20.0f, nA0, w, h);
    GenerateNormalmapFromHeightfield(B, 100.0f, 100.0f, 20.0f, nB0, w, h);
    uint32 diffs = 0;
    for (uint32 z = 0; z < dim; ++z)
        if (texel(nA0, dim - 1, z) != texel(nB0, 0, z))
            ++diffs;
    EXPECT_GT(diffs, 0u) << "boundary normals should diverge WITHOUT neighbours (test not discriminating)";
}

// Work-quiescence oracle (planet-modifier review): an idle spherical terrain carrying
// static modifiers must bake NOTHING after the initial settle. The sphere sculpt version
// advances only on a real bake (BakePlanetModifierRegions, gated by dirty.Count > 0 AND
// the modifier change gate), so a version that holds constant across idle frames is a
// zero-bake count oracle.
TEST(TerrainRegionBake, IdleSphericalTerrainWithModifiersBakesNothing)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    const TerrainHandle handle = CreateBakedTerrain(svc);
    CreatePlanetEntity(world, handle);
    CreateNoiseModifier(world, 0.0f, 0.0f);
    world.SwapLifecycleEvents();

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // initial full sphere bake (no baseline yet)
    const uint64 settled = svc.SphereSculptVersion();
    EXPECT_GT(settled, 0u) << "the initial bake must produce a sphere sculpt";

    // Idle frames: nothing changed, so neither the change gate nor the region diff has
    // any work — the version must not advance.
    for (int i = 0; i < 8; ++i)
    {
        world.SwapLifecycleEvents();
        system.Update(world, 1.0f / 60.0f);
    }
    EXPECT_EQ(svc.SphereSculptVersion(), settled)
        << "an idle spherical terrain with static modifiers must bake nothing";
}

// PLANET-RESIZE ORACLE (sculpt-content remap slice): a PlanetRadius change is a terrain-STATE
// change for the modifier bake — footprints are authored in world metres, so the resize moves
// every modifier's angular footprint and the modifier layer must re-derive at the new radius.
// DISCRIMINATING: ComputeTerrainStateHash folds Domain + PlanetRadius (spherical only); without
// that fold the change-gate hash matches the baseline and the resize bakes nothing, leaving the
// baked modifier content scaled with the planet.
TEST(TerrainRegionBake, PlanetRadiusChangeTriggersFullSphereRebake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    const TerrainHandle handle = CreateBakedTerrain(svc);
    const ECS::EntityHandle planet = CreatePlanetEntity(world, handle);
    CreateNoiseModifier(world, 0.0f, 0.0f);
    world.SwapLifecycleEvents();

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // initial full sphere bake
    uint64 settled = svc.SphereSculptVersion();
    EXPECT_GT(settled, 0u);
    for (int i = 0; i < 4; ++i) // settle: idle frames bake nothing
    {
        world.SwapLifecycleEvents();
        system.Update(world, 1.0f / 60.0f);
    }
    settled = svc.SphereSculptVersion();

    // The resize: the same edit an inspector PlanetRadius commit lands.
    world.GetComponentForWrite<Components::Terrain>(planet)->PlanetRadius = 6000.0f;
    world.SwapLifecycleEvents();
    system.Update(world, 1.0f / 60.0f);
    EXPECT_GT(svc.SphereSculptVersion(), settled)
        << "a planet radius change must trigger the full sphere modifier re-bake";
    EXPECT_EQ(svc.GetPlanetSculptGeometry().VirtualDim,
              GameEngine::CBTTerrain::DeriveSculptVirtualDim(6000.0f))
        << "the bake path's ConfigurePlanetSculpt must adopt (remap to) the new radius grid";

    // And it settles again: the new radius becomes the baseline, idle frames bake nothing.
    const uint64 afterResize = svc.SphereSculptVersion();
    for (int i = 0; i < 4; ++i)
    {
        world.SwapLifecycleEvents();
        system.Update(world, 1.0f / 60.0f);
    }
    EXPECT_EQ(svc.SphereSculptVersion(), afterResize)
        << "the resize must not leave the bake gate churning";
}

// ---- Flatten / Stamp on spherical (planet) terrain (round-6 P2, her #12) ------------------------
//
// A planet has no planar heightfield: modifiers bake WORLD-METRE offsets into the sphere sculpt
// atlas, additive over the closed-form base relief (CBTTerrain::PlanetRelief). The rendered surface
// radius at a direction is planetRadius + relief(dir) + sculpt(dir). Flatten levels that surface
// toward a radial target by emitting the offset that cancels the relief:
//   sculpt(dir) = weight * (targetRadius - planetRadius - relief(dir))
// so surface = lerp(planetRadius + relief(dir), targetRadius, weight) — the radial analogue of the
// planar h' = lerp(h, target, weight). These oracles sample the baked atlas by direction
// (SampleSphereSculptHeight) and reconstruct the surface with the same closed-form relief the GPU
// displaces with.

namespace
{
// Build an orthonormal tangent frame at unit direction N and return the unit direction at angular
// offset `angle` (radians) from N along azimuth `phi` — a point on the sphere inside a cap of
// half-angle `angle`.
Dir3 DirAtAngularOffset(Dir3 n, float32 angle, float32 phi)
{
    const std::array<float32, 3> narr = {n.x, n.y, n.z};
    const std::array<float32, 3> t1 = CBTTerrain::AnyTangent(narr);
    const std::array<float32, 3> t2 = {n.y * t1[2] - n.z * t1[1], n.z * t1[0] - n.x * t1[2],
                                       n.x * t1[1] - n.y * t1[0]};
    const float32 ca = std::cos(angle), sa = std::sin(angle);
    const float32 cp = std::cos(phi), sp = std::sin(phi);
    return {n.x * ca + (t1[0] * cp + t2[0] * sp) * sa, n.y * ca + (t1[1] * cp + t2[1] * sp) * sa,
            n.z * ca + (t1[2] * cp + t2[2] * sp) * sa};
}
} // namespace

// Her #12: a Flatten modifier dragged over a planet must actually flatten the heightmap under the
// ring. With UseEntityHeight (the drag default) the target radius is the entity's distance from the
// planet centre. Oracle: across a fan of directions inside the footprint the reconstructed surface
// radius collapses to a near-constant plateau at the target — even though the underlying relief
// varies by many metres there (discrimination) — and the far hemisphere is untouched.
TEST(TerrainRegionBake, SphereFlattenLevelsBaseReliefToEntityRadius)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    const TerrainHandle handle = CreateBakedTerrain(svc);
    const ECS::EntityHandle planetE = CreatePlanetEntity(world, handle); // PlanetRadius = 2000

    // Explicit relief so the oracle params are known AND the entity-read plumbing is exercised.
    Components::TerrainPlanetRelief relief{};
    relief.Amplitude = 200.0f;
    relief.Frequency = 6.0f;
    relief.Octaves = 3u;
    world.AddComponentImmediate<Components::TerrainPlanetRelief>(planetE, relief);

    constexpr float32 kPlanetRadius = 2000.0f;
    const Dir3 dirN = Normalize3({1.0f, 0.7f, 0.4f});
    // Entity well above the surface → an elevated, unambiguous plateau target.
    constexpr float32 kTargetRadius = 2200.0f;
    const Dir3 pos = {dirN.x * kTargetRadius, dirN.y * kTargetRadius, dirN.z * kTargetRadius};
    CreateSphereFlattenModifier(world, pos, /*radius*/ 250.0f, /*falloff*/ 50.0f,
                                /*useEntityHeight*/ true, /*targetHeight*/ 0.0f);
    world.SwapLifecycleEvents();

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // initial full sphere bake
    EXPECT_GT(svc.SphereSculptVersion(), 0u) << "the flatten bake must produce a sphere sculpt";

    auto reliefAt = [&](Dir3 d) {
        return CBTTerrain::PlanetRelief(d.x, d.y, d.z, relief.Amplitude, relief.Frequency,
                                        relief.Octaves);
    };
    auto surfaceRadiusAt = [&](Dir3 d) {
        return kPlanetRadius + reliefAt(d) + svc.SampleSphereSculptHeight(d.x, d.y, d.z, 0.0f);
    };

    // Fan of directions strictly inside the footprint radius (Radius/R = 0.125 rad; sample <= 0.09).
    float32 surfMin = 1e30f, surfMax = -1e30f, reliefMin = 1e30f, reliefMax = -1e30f, surfSum = 0.0f;
    int32 count = 0;
    for (float32 a : {0.0f, 0.03f, 0.06f, 0.09f})
        for (float32 phi : {0.0f, 1.57079633f, 3.14159265f, 4.71238898f})
        {
            const Dir3 d = DirAtAngularOffset(dirN, a, phi);
            const float32 s = surfaceRadiusAt(d);
            const float32 r = reliefAt(d);
            surfMin = std::min(surfMin, s);
            surfMax = std::max(surfMax, s);
            reliefMin = std::min(reliefMin, r);
            reliefMax = std::max(reliefMax, r);
            surfSum += s;
            ++count;
            if (a == 0.0f)
                break; // the centre is one point regardless of azimuth
        }
    const float32 surfSpread = surfMax - surfMin;
    const float32 reliefSpread = reliefMax - reliefMin;
    const float32 surfMean = surfSum / static_cast<float32>(count);

    // Discrimination: the footprint genuinely spans varying relief (so a "flat" result is meaningful).
    EXPECT_GT(reliefSpread, 10.0f) << "test not discriminating: the relief is nearly flat here";
    // Flattening: the reconstructed surface collapses to a near-constant plateau — the bumps are gone.
    EXPECT_LT(surfSpread, std::max(1.5f, 0.15f * reliefSpread))
        << "surface still bumpy: flatten did not cancel the base relief (spread " << surfSpread
        << " m vs relief spread " << reliefSpread << " m)";
    // Levels to the target radius (the entity's distance from centre).
    EXPECT_NEAR(surfMean, kTargetRadius, std::max(1.5f, 0.15f * reliefSpread))
        << "flatten did not level to the entity-radius target";

    // The opposite hemisphere is untouched (SET semantics — offset 0 outside every footprint).
    EXPECT_NEAR(svc.SampleSphereSculptHeight(-dirN.x, -dirN.y, -dirN.z, 0.0f), 0.0f, 1e-3f);
}

// UseEntityHeight = false: the target radius is planetRadius + TargetHeight (a signed radial offset
// from the nominal surface). Also asserts the footprint boundary — a direction beyond Radius+Falloff
// is untouched (offset 0), so the plateau is local, not global.
TEST(TerrainRegionBake, SphereFlattenTargetHeightModeAndFootprintBoundary)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    const TerrainHandle handle = CreateBakedTerrain(svc);
    const ECS::EntityHandle planetE = CreatePlanetEntity(world, handle);

    Components::TerrainPlanetRelief relief{};
    relief.Amplitude = 120.0f;
    relief.Frequency = 6.0f;
    relief.Octaves = 3u;
    world.AddComponentImmediate<Components::TerrainPlanetRelief>(planetE, relief);

    constexpr float32 kPlanetRadius = 2000.0f;
    constexpr float32 kTargetHeight = 150.0f; // target radius = 2150
    const Dir3 dirN = Normalize3({-0.3f, 1.0f, 0.6f});
    // Place the entity ON the base sphere; the radial target comes from TargetHeight, not |pos|.
    const Dir3 pos = {dirN.x * kPlanetRadius, dirN.y * kPlanetRadius, dirN.z * kPlanetRadius};
    constexpr float32 kRadius = 200.0f, kFalloff = 40.0f;
    CreateSphereFlattenModifier(world, pos, kRadius, kFalloff,
                                /*useEntityHeight*/ false, /*targetHeight*/ kTargetHeight);
    world.SwapLifecycleEvents();

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    auto reliefAt = [&](Dir3 d) {
        return CBTTerrain::PlanetRelief(d.x, d.y, d.z, relief.Amplitude, relief.Frequency,
                                        relief.Octaves);
    };

    // Centre: surface == planetRadius + TargetHeight (weight 1).
    const float32 centreSurface =
        kPlanetRadius + reliefAt(dirN) + svc.SampleSphereSculptHeight(dirN.x, dirN.y, dirN.z, 0.0f);
    EXPECT_NEAR(centreSurface, kPlanetRadius + kTargetHeight, 1.5f);

    // Just outside the footprint (angle > (Radius+Falloff)/R): untouched, offset ~ 0.
    const float32 outsideAngle = (kRadius + kFalloff) / kPlanetRadius + 0.02f;
    const Dir3 dOut = DirAtAngularOffset(dirN, outsideAngle, 0.0f);
    EXPECT_NEAR(svc.SampleSphereSculptHeight(dOut.x, dOut.y, dOut.z, 0.0f), 0.0f, 1e-3f)
        << "flatten leaked past its footprint";

    // The centre offset is substantial (the store was actually written toward the target).
    EXPECT_GT(std::abs(svc.SampleSphereSculptHeight(dirN.x, dirN.y, dirN.z, 0.0f)), 1.0f);
}

// Removing the last sphere modifier must re-derive its vacated footprint back to 0 (SET semantics),
// so the surface returns to the pure base relief — the sphere analogue of ModifierRemovalRestoresBase.
TEST(TerrainRegionBake, SphereFlattenRemovalRestoresBaseSurface)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    const TerrainHandle handle = CreateBakedTerrain(svc);
    CreatePlanetEntity(world, handle);

    const Dir3 dirN = Normalize3({0.2f, 0.9f, -0.4f});
    const Dir3 pos = {dirN.x * 2200.0f, dirN.y * 2200.0f, dirN.z * 2200.0f};
    const ECS::EntityHandle modE = CreateSphereFlattenModifier(world, pos, 250.0f, 50.0f, true, 0.0f);
    world.SwapLifecycleEvents();

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // bake the flatten
    EXPECT_GT(std::abs(svc.SampleSphereSculptHeight(dirN.x, dirN.y, dirN.z, 0.0f)), 1.0f)
        << "the flatten must have written the sculpt at its centre";

    world.DestroyEntityImmediate(modE);
    world.SwapLifecycleEvents(); // surface Removed<T>
    system.Update(world, 1.0f / 60.0f); // re-derive the vacated footprint to 0

    EXPECT_NEAR(svc.SampleSphereSculptHeight(dirN.x, dirN.y, dirN.z, 0.0f), 0.0f, 1e-3f)
        << "removing the flatten must restore the base relief surface (offset 0)";
}

// Stamp with no mask is the documented flat-white disc: a uniform HeightScale raise (Add) or drop
// (Subtract) across the footprint, additive over the relief. No texture asset needed.
TEST(TerrainRegionBake, SphereStampFlatWhiteDiscAddSubtract)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    const TerrainHandle handle = CreateBakedTerrain(svc);
    CreatePlanetEntity(world, handle);

    const Dir3 dirN = Normalize3({0.5f, 0.5f, 0.7f});
    const Dir3 pos = {dirN.x * 2000.0f, dirN.y * 2000.0f, dirN.z * 2000.0f};
    constexpr float32 kStampHeight = 40.0f, kRadius = 250.0f, kFalloff = 50.0f;

    // Add: the centre offset is +HeightScale (weight 1, flat-white mask == 1).
    const ECS::EntityHandle addE = CreateSphereStampModifier(
        world, pos, kRadius, kFalloff, kStampHeight, Components::TerrainModifierBlend::Add);
    world.SwapLifecycleEvents();
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_NEAR(svc.SampleSphereSculptHeight(dirN.x, dirN.y, dirN.z, 0.0f), kStampHeight, 1.0f);
    // Beyond the footprint: untouched.
    const Dir3 dOut = DirAtAngularOffset(dirN, (kRadius + kFalloff) / 2000.0f + 0.02f, 0.0f);
    EXPECT_NEAR(svc.SampleSphereSculptHeight(dOut.x, dOut.y, dOut.z, 0.0f), 0.0f, 1e-3f);

    // Subtract: the same disc, negated. Destroy + recreate share one lifecycle swap so the bake
    // sees both the Removed (old Add) and Added (new Subtract) in one gather.
    world.DestroyEntityImmediate(addE);
    CreateSphereStampModifier(world, pos, kRadius, kFalloff, kStampHeight,
                              Components::TerrainModifierBlend::Subtract);
    world.SwapLifecycleEvents();
    system.Update(world, 1.0f / 60.0f);
    EXPECT_NEAR(svc.SampleSphereSculptHeight(dirN.x, dirN.y, dirN.z, 0.0f), -kStampHeight, 1.0f);
}

TEST(TerrainRegionBake, SphereNonHeightEffectsDoNotCreateFlattenPlacements)
{
    ScopedTerrainService scoped;
    auto& service = TerrainService::Get();
    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreatePlanetEntity(world, CreateBakedTerrain(service));

    const Dir3 direction = Normalize3({0.5f, 0.5f, 0.7f});
    const Dir3 position = {direction.x * 2000.0f, direction.y * 2000.0f, direction.z * 2000.0f};
    constexpr float32 kStampHeight = 40.0f;
    const auto modifier = CreateSphereStampModifier(
        world, position, 250.0f, 50.0f, kStampHeight, Components::TerrainModifierBlend::Add);
    world.AddComponentImmediate<Components::TerrainSurfaceRulesEffect>(modifier, Components::TerrainSurfaceRulesEffect{});
    world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(modifier, {});
    world.AddComponentImmediate<Components::TerrainGrassEffect>(modifier, {});
    world.SwapLifecycleEvents();

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    EXPECT_NEAR(service.SampleSphereSculptHeight(direction.x, direction.y, direction.z, 0.0f),
                kStampHeight, 1.0f);
}

// Planet-grass gate oracle. TerrainExtractionSystem sources a terrain's grass params only when the
// domain supports the planar placement compute; a spherical planet would otherwise get blades rooted
// on a flat XZ grid, floating off its curved surface (the reported "grass in outer space" bug). The
// production gate is `TerrainGrassSupportsDomain(domain) ? grassComp : nullptr`; this locks that
// policy so a future edit cannot silently re-enable planar grass on a sphere without dealing with
// radial placement. The regression half (planar keeps grass) rides the same predicate.
TEST(TerrainGrassGate, SphericalDomainSourcesNoGrassPlanarUnchanged)
{
    EXPECT_TRUE(Components::TerrainGrassSupportsDomain(Components::TerrainDomain::Planar));
    EXPECT_FALSE(Components::TerrainGrassSupportsDomain(Components::TerrainDomain::Spherical));

    // Mirror the extraction gate expression exactly (same selection the system performs), so the
    // oracle fails if the gate is removed or the predicate flips.
    Components::TerrainGrass grass{};

    const Components::TerrainGrass* planarSourced =
        Components::TerrainGrassSupportsDomain(Components::TerrainDomain::Planar) ? &grass : nullptr;
    const Components::TerrainGrass* sphericalSourced =
        Components::TerrainGrassSupportsDomain(Components::TerrainDomain::Spherical) ? &grass : nullptr;

    EXPECT_EQ(planarSourced, &grass) << "planar terrain must keep its grass (regression untouched)";
    EXPECT_EQ(sphericalSourced, nullptr) << "spherical terrain must source no grass (no floaters)";
}

// ---------------------------------------------------------------------------------------------
// Analytic sphere modifiers, live wiring (sculpt shape-accuracy S2, GE_TERRAIN_ANALYTIC_MODIFIERS).
// The flag partitions closed-form-primitive modifiers (circular flattens) OUT of the store bake
// and publishes them as analytic placements the sampling chokepoints evaluate exactly. These
// oracles run the REAL system path (gather -> state hash -> BakeSphereModifiers -> TerrainService)
// and toggle the flag per test — AnalyticModifiersEnabled() reads the env fresh for exactly this.
// ---------------------------------------------------------------------------------------------

namespace
{
// Set / clear GE_TERRAIN_ANALYTIC_MODIFIERS for one test scope (the CRT env std::getenv reads).
struct ScopedAnalyticModifiersFlag
{
    explicit ScopedAnalyticModifiersFlag(const char* value)
    {
#ifdef _WIN32
        _putenv_s("GE_TERRAIN_ANALYTIC_MODIFIERS", value);
#else
        setenv("GE_TERRAIN_ANALYTIC_MODIFIERS", value, 1);
#endif
    }
    ~ScopedAnalyticModifiersFlag()
    {
#ifdef _WIN32
        _putenv_s("GE_TERRAIN_ANALYTIC_MODIFIERS", ""); // empty = removed on the Windows CRT
#else
        unsetenv("GE_TERRAIN_ANALYTIC_MODIFIERS");
#endif
    }
};
} // namespace

// No-double-apply (the S2 injection contract): with the flag ON a circular flatten is published as
// an analytic placement and NOT rasterized into the sculpt store — the store-only sample stays 0
// across its footprint while the COMPOSED sample levels the pad exactly to the target radius. If
// the bake ever also wrote the store, the composed height would be analytic + baked (double the
// offset) and the target assert below fails.
TEST(TerrainRegionBake, AnalyticFlattenPublishesPlacementNotStoreBake)
{
    ScopedAnalyticModifiersFlag analyticOn("1");
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    const TerrainHandle handle = CreateBakedTerrain(svc);
    const ECS::EntityHandle planetE = CreatePlanetEntity(world, handle); // PlanetRadius = 2000

    Components::TerrainPlanetRelief relief{};
    relief.Amplitude = 200.0f;
    relief.Frequency = 6.0f;
    relief.Octaves = 3u;
    world.AddComponentImmediate<Components::TerrainPlanetRelief>(planetE, relief);

    constexpr float32 kPlanetRadius = 2000.0f;
    constexpr float32 kTargetRadius = 2200.0f;
    const Dir3 dirN = Normalize3({1.0f, 0.7f, 0.4f});
    const Dir3 pos = {dirN.x * kTargetRadius, dirN.y * kTargetRadius, dirN.z * kTargetRadius};
    CreateSphereFlattenModifier(world, pos, /*radius*/ 250.0f, /*falloff*/ 50.0f,
                                /*useEntityHeight*/ true, /*targetHeight*/ 0.0f);
    world.SwapLifecycleEvents();

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    // Published as a placement…
    const auto mirror = svc.GetPlanetSculptMirror();
    ASSERT_EQ(mirror.AnalyticCount, 1u) << "the flatten must publish as an analytic placement";
    EXPECT_GT(svc.SphereSculptVersion(), 0u) << "an analytic publish IS an edit (version advances)";

    // …and NOT into the store: the store-only sample is 0 across the footprint (fan of dirs).
    auto reliefAt = [&](Dir3 d) {
        return CBTTerrain::PlanetRelief(d.x, d.y, d.z, relief.Amplitude, relief.Frequency,
                                        relief.Octaves);
    };
    for (float32 a : {0.0f, 0.05f, 0.1f})
        for (float32 phi : {0.0f, 2.0f, 4.0f})
        {
            const Dir3 d = DirAtAngularOffset(dirN, a, phi);
            EXPECT_EQ(CBTTerrain::SampleSphereSculptByDir(mirror.Sampler, d.x, d.y, d.z), 0.0f)
                << "store must hold NO texels for an analytically-published flatten (a=" << a
                << " phi=" << phi << ")";
            // The composed sample levels the surface EXACTLY to the target (weight 1 inside the
            // 250 m pad; 0.1 rad = 200 m < 250 m): composed == analytic alone, not analytic+bake.
            const float32 rel = reliefAt(d);
            const float32 surface =
                kPlanetRadius + rel + svc.SampleSphereSculptHeight(d.x, d.y, d.z, rel);
            EXPECT_NEAR(surface, kTargetRadius, 0.02f)
                << "composed surface must land exactly on the target (a=" << a << " phi=" << phi
                << ")";
            if (a == 0.0f)
                break;
        }

    // The retess/physics drive fired: the bake unioned the flatten's footprint into the dirty
    // regions even though no texel was written (the region SET semantics + margin).
    CBTTerrain::SphereEditRegions dirty{};
    EXPECT_TRUE(svc.ConsumeSphereSculptDirtyRegions(dirty))
        << "an analytic publish must dirty its footprint (edit-driven retess drive)";
    EXPECT_GT(dirty.Count, 0u);
}

// The #620 lifecycle matrix, CPU half, camera-still: create / move / property-change / delete of
// an analytic flatten must each advance the combined sculpt version AND produce non-empty dirty
// regions (what re-arms Classify's edit-driven retess + the physics refresh without camera
// motion). The editor gate proves the same matrix visually; this pins the plumbing.
TEST(TerrainRegionBake, AnalyticFlattenLifecycleEditsAdvanceVersionAndRegions)
{
    ScopedAnalyticModifiersFlag analyticOn("1");
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    const TerrainHandle handle = CreateBakedTerrain(svc);
    CreatePlanetEntity(world, handle);

    const Dir3 dirN = Normalize3({0.3f, 1.0f, -0.2f});
    const Dir3 pos = {dirN.x * 2100.0f, dirN.y * 2100.0f, dirN.z * 2100.0f};
    TerrainModifierSystem system;

    auto expectEditFired = [&](const char* what, uint64 versionBefore) {
        EXPECT_GT(svc.SphereSculptVersion(), versionBefore)
            << what << " must advance the sculpt version (an edit IS an edit)";
        CBTTerrain::SphereEditRegions dirty{};
        EXPECT_TRUE(svc.ConsumeSphereSculptDirtyRegions(dirty))
            << what << " must produce dirty regions (camera-still retess drive)";
        EXPECT_GT(dirty.Count, 0u) << what;
    };

    // CREATE.
    uint64 v = svc.SphereSculptVersion();
    const ECS::EntityHandle modE =
        CreateSphereFlattenModifier(world, pos, 200.0f, 40.0f, true, 0.0f);
    world.SwapLifecycleEvents();
    system.Update(world, 1.0f / 60.0f);
    ASSERT_EQ(svc.GetPlanetSculptMirror().AnalyticCount, 1u);
    expectEditFired("create", v);

    // MOVE (tangential shift — new footprint, same radius).
    v = svc.SphereSculptVersion();
    {
        auto* xf = world.GetComponentForWrite<Components::WorldTransform>(modE);
        ASSERT_NE(xf, nullptr);
        const Dir3 dir2 = Normalize3({0.5f, 1.0f, -0.2f});
        xf->matrix[12] = dir2.x * 2100.0f;
        xf->matrix[13] = dir2.y * 2100.0f;
        xf->matrix[14] = dir2.z * 2100.0f;
    }
    world.SwapLifecycleEvents();
    system.Update(world, 1.0f / 60.0f);
    expectEditFired("move", v);

    // PROPERTY (radius change).
    v = svc.SphereSculptVersion();
    world.GetComponentForWrite<Components::TerrainModifierVolume>(modE)->Radius = 120.0f;
    world.SwapLifecycleEvents();
    system.Update(world, 1.0f / 60.0f);
    expectEditFired("property change", v);

    // DELETE.
    v = svc.SphereSculptVersion();
    world.DestroyEntityImmediate(modE);
    world.SwapLifecycleEvents();
    system.Update(world, 1.0f / 60.0f);
    expectEditFired("delete", v);
    EXPECT_EQ(svc.GetPlanetSculptMirror().AnalyticCount, 0u)
        << "deleting the last analytic flatten must clear the placement set";
}

// Dark-ship discrimination: the SAME scene with the flag OFF publishes nothing and bakes the
// flatten into the store (the pre-S2 path, byte-for-byte — the 91-test baseline covers its
// shape; this pins that the flag actually gates the partition).
TEST(TerrainRegionBake, AnalyticFlagOffKeepsStoreBakePath)
{
    ScopedAnalyticModifiersFlag analyticOff("0");
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    const TerrainHandle handle = CreateBakedTerrain(svc);
    CreatePlanetEntity(world, handle);

    const Dir3 dirN = Normalize3({0.2f, 0.9f, -0.4f});
    const Dir3 pos = {dirN.x * 2200.0f, dirN.y * 2200.0f, dirN.z * 2200.0f};
    CreateSphereFlattenModifier(world, pos, 250.0f, 50.0f, true, 0.0f);
    world.SwapLifecycleEvents();

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const auto mirror = svc.GetPlanetSculptMirror();
    EXPECT_EQ(mirror.AnalyticCount, 0u) << "flag off must publish no analytic placements";
    EXPECT_GT(std::abs(CBTTerrain::SampleSphereSculptByDir(mirror.Sampler, dirN.x, dirN.y, dirN.z)),
              1.0f)
        << "flag off must bake the flatten into the store (the pre-S2 path)";
}

// Overflow honesty: the analytic set is bounded (kMaxSphereAnalyticModifiers). With one more
// eligible flatten than the cap, exactly the cap publish analytically and the excess falls back
// to the store bake (never dropped): exactly one centre carries store texels, and EVERY pad still
// levels to its target through the composed sample.
TEST(TerrainRegionBake, AnalyticOverflowFallsBackToStoreBake)
{
    ScopedAnalyticModifiersFlag analyticOn("1");
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    const TerrainHandle handle = CreateBakedTerrain(svc);
    CreatePlanetEntity(world, handle);

    constexpr uint32 kCount = CBTTerrain::kMaxSphereAnalyticModifiers + 1u; // 17
    constexpr float32 kTargetRadius = 2150.0f;
    std::vector<Dir3> dirs;
    for (uint32 i = 0; i < kCount; ++i)
    {
        // Well-separated directions on the upper hemisphere (footprints must not overlap:
        // radius+falloff = 130 m -> 0.065 rad at R=2000; spacing here is >= 0.3 rad).
        const float32 phi = 6.2831853f * static_cast<float32>(i) / static_cast<float32>(kCount);
        const float32 tilt = 0.5f + 0.3f * static_cast<float32>(i % 3);
        const Dir3 d = Normalize3({std::cos(phi), 1.2f + tilt, std::sin(phi)});
        dirs.push_back(d);
        CreateSphereFlattenModifier(world,
                                    {d.x * kTargetRadius, d.y * kTargetRadius, d.z * kTargetRadius},
                                    100.0f, 30.0f, true, 0.0f);
    }
    world.SwapLifecycleEvents();

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const auto mirror = svc.GetPlanetSculptMirror();
    EXPECT_EQ(mirror.AnalyticCount, CBTTerrain::kMaxSphereAnalyticModifiers)
        << "the analytic set must fill to its cap";

    uint32 bakedCentres = 0u;
    for (const Dir3& d : dirs)
    {
        if (std::abs(CBTTerrain::SampleSphereSculptByDir(mirror.Sampler, d.x, d.y, d.z)) > 1.0f)
            ++bakedCentres;
        // Every pad levels to its target regardless of which path carried it: surface =
        // R + relief + composed must equal the target (default TerrainPlanetRelief params).
        const Components::TerrainPlanetRelief relief{};
        const float32 rel = CBTTerrain::PlanetRelief(d.x, d.y, d.z, relief.Amplitude,
                                                     relief.Frequency, relief.Octaves);
        const float32 surface = 2000.0f + rel + svc.SampleSphereSculptHeight(d.x, d.y, d.z, rel);
        EXPECT_NEAR(surface, kTargetRadius, 1.0f) // store texel quantization for the baked one
            << "every pad must level to its target (analytic or baked)";
    }
    EXPECT_EQ(bakedCentres, 1u) << "exactly the overflow flatten must fall back to the store bake";
}

// ---------------------------------------------------------------------------------------------
// Analytic-while-stroking (sculpt shape-accuracy S3, GE_TERRAIN_ANALYTIC_MODIFIERS).
// A held brush stroke's dabs queue as TRANSIENT analytic placements (exact preview) and commit
// to the store on release through the normal ApplyDab path. These oracles pin the deferral, the
// byte-identical commit, the #630 undo composition, the overflow chunking, and the flag-off
// dark-ship.
// ---------------------------------------------------------------------------------------------

// Gate 2 (stroke-commit round-trip): a stroked (deferred) dab sequence commits to EXACTLY the
// store bytes an immediate (pre-S3) sequence produces — same pool, same page table — because the
// commit replays the same ApplyDab calls in the same order. Mid-stroke, the store holds NOTHING
// (the preview is transient) while the composed sample already shows the dabs.
TEST(TerrainRegionBake, StrokedDabsCommitByteIdenticalToImmediateDabs)
{
    ScopedAnalyticModifiersFlag analyticOn("1");
    constexpr float32 kPlanetRadius = 50000.0f;
    struct DabParams
    {
        float32 X, Y, Z, AngR, Strength;
        bool Lower;
    };
    // A stroke path: three distinct centres, a repeated (merge-eligible) centre, one lower dab.
    const DabParams dabs[] = {
        {1.0f, 0.02f, 0.01f, 100.0f / kPlanetRadius, 5.0f, false},
        {1.0f, 0.02f, 0.01f, 100.0f / kPlanetRadius, 5.0f, false}, // merges in the transient
        {1.0f, 0.025f, 0.01f, 100.0f / kPlanetRadius, 4.0f, false},
        {1.0f, 0.03f, 0.012f, 80.0f / kPlanetRadius, 3.0f, true},
    };

    // Control: the pre-S3 immediate path.
    std::vector<float32> controlPool;
    std::vector<uint32> controlTable;
    CBTTerrain::SphereSculptGeometry controlGeom{};
    {
        ScopedTerrainService scoped;
        auto& svc = TerrainService::Get();
        svc.ConfigurePlanetSculpt(kPlanetRadius);
        for (const DabParams& d : dabs)
            svc.ApplySphereSculptDab(d.X, d.Y, d.Z, d.AngR, d.Strength, d.Lower);
        svc.CopySphereSculptUpload(controlPool, controlTable, controlGeom);
    }

    // S3: the stroked path — defer, preview, commit.
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    svc.ConfigurePlanetSculpt(kPlanetRadius);
    svc.BeginSphereSculptStrokeCapture();
    for (const DabParams& d : dabs)
    {
        const CBTTerrain::SphereEditRegions regions =
            svc.ApplySphereSculptDabStroked(d.X, d.Y, d.Z, d.AngR, d.Strength, d.Lower);
        EXPECT_GT(regions.Count, 0u) << "a stroked dab must dirty its footprint (retess drive)";
    }

    // Mid-stroke: store empty, transient live, composed sample already shows the stroke.
    {
        const auto mirror = svc.GetPlanetSculptMirror();
        EXPECT_EQ(mirror.TransientDabCount, 3u)
            << "3 distinct centres (the repeated centre merges into one transient)";
        const float32 len0 = std::sqrt(dabs[0].X * dabs[0].X + dabs[0].Y * dabs[0].Y +
                                       dabs[0].Z * dabs[0].Z);
        const float32 ux = dabs[0].X / len0, uy = dabs[0].Y / len0, uz = dabs[0].Z / len0;
        EXPECT_EQ(CBTTerrain::SampleSphereSculptByDir(mirror.Sampler, ux, uy, uz), 0.0f)
            << "mid-stroke the store must hold NO texels (the preview is transient)";
        EXPECT_NEAR(svc.SampleSphereSculptHeight(ux, uy, uz, 0.0f), 2.0f * dabs[0].Strength, 1e-4f)
            << "the composed sample must show both merged dabs at the first centre";
        EXPECT_TRUE(svc.HasSphereSculptEdits()) << "a held stroke IS an edit (shader gate arms)";
    }

    // Release: commit + capture drain (the brush's EndSphereStroke order).
    svc.CommitSphereSculptStroke();
    std::vector<CBTTerrain::SphereSculptPageState> before = svc.TakeSphereSculptStrokeCapture();
    EXPECT_FALSE(before.empty()) << "the commit's writes must appear in the stroke capture";

    std::vector<float32> pool;
    std::vector<uint32> table;
    CBTTerrain::SphereSculptGeometry geom{};
    svc.CopySphereSculptUpload(pool, table, geom);
    EXPECT_EQ(geom.VirtualDim, controlGeom.VirtualDim);
    EXPECT_EQ(table, controlTable) << "page allocation order must match the immediate path";
    EXPECT_EQ(pool, controlPool) << "store bytes must match the immediate path exactly";

    const auto mirror = svc.GetPlanetSculptMirror();
    EXPECT_EQ(mirror.TransientDabCount, 0u) << "release must clear the transient set";

    // Gate 2 (undo composition): restoring the capture's pre-images returns the store to its
    // pre-stroke (empty) state — the transient contributed nothing to the undo payload.
    svc.RestoreSphereSculptPages(before);
    std::vector<float32> undonePool;
    std::vector<uint32> undoneTable;
    svc.CopySphereSculptUpload(undonePool, undoneTable, geom);
    for (float32 texel : undonePool)
        EXPECT_EQ(texel, 0.0f) << "undo must restore the pre-stroke (empty) store exactly";
}

// Gate 4 (dark-ship): with the flag OFF the stroked entry point IS the immediate path — store
// written per dab, no transient ever exists, byte-identical to pre-S3.
TEST(TerrainRegionBake, StrokedDabsFlagOffAreImmediatePreS3Path)
{
    ScopedAnalyticModifiersFlag analyticOff("0");
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    constexpr float32 kPlanetRadius = 50000.0f;
    svc.ConfigurePlanetSculpt(kPlanetRadius);

    svc.BeginSphereSculptStrokeCapture();
    const CBTTerrain::SphereEditRegions regions = svc.ApplySphereSculptDabStroked(
        1.0f, 0.02f, 0.01f, 100.0f / kPlanetRadius, 5.0f, false);
    EXPECT_GT(regions.Count, 0u);

    const auto mirror = svc.GetPlanetSculptMirror();
    EXPECT_EQ(mirror.TransientDabCount, 0u) << "flag off must never create a transient";
    const float32 len = std::sqrt(1.0f + 0.02f * 0.02f + 0.01f * 0.01f);
    EXPECT_GT(CBTTerrain::SampleSphereSculptByDir(mirror.Sampler, 1.0f / len, 0.02f / len,
                                                  0.01f / len),
              1.0f)
        << "flag off must write the store immediately (the pre-S3 path)";
    svc.CommitSphereSculptStroke(); // must be an inert no-op
    const uint64 versionAfterCommit = svc.SphereSculptVersion();
    svc.CommitSphereSculptStroke();
    EXPECT_EQ(svc.SphereSculptVersion(), versionAfterCommit)
        << "an empty commit must not churn the version";
    svc.TakeSphereSculptStrokeCapture();
}

// Overflow chunking: with 15 published flattens the transient budget is 1, so a second distinct
// centre mid-stroke commits the oldest chunk to the store (commit-as-you-go) — and the final
// content still matches the immediate path byte for byte (FIFO order preserved).
TEST(TerrainRegionBake, StrokedDabOverflowCommitsOldestChunkInOrder)
{
    ScopedAnalyticModifiersFlag analyticOn("1");
    constexpr float32 kPlanetRadius = 50000.0f;
    constexpr uint32 kFlattens = CBTTerrain::kMaxSphereAnalyticModifiers - 1u; // budget = 1
    std::array<CBTTerrain::SphereAnalyticFlatten, CBTTerrain::kMaxSphereAnalyticModifiers>
        placements{};
    for (uint32 i = 0; i < kFlattens; ++i)
    {
        const float32 phi = 6.2831853f * static_cast<float32>(i) / static_cast<float32>(kFlattens);
        placements[i] = CBTTerrain::MakeSphereAnalyticFlatten(
            std::cos(phi) * kPlanetRadius, 0.4f * kPlanetRadius, std::sin(phi) * kPlanetRadius,
            100.0f, 20.0f, kPlanetRadius + 5.0f, kPlanetRadius);
    }

    struct DabParams
    {
        float32 X, Y, Z;
    };
    const DabParams centres[] = {{1.0f, 0.02f, 0.01f}, {1.0f, 0.026f, 0.01f}, {1.0f, 0.032f, 0.01f}};
    const float32 angR = 100.0f / kPlanetRadius;

    std::vector<float32> controlPool;
    std::vector<uint32> controlTable;
    CBTTerrain::SphereSculptGeometry geom{};
    {
        ScopedTerrainService scoped;
        auto& svc = TerrainService::Get();
        svc.ConfigurePlanetSculpt(kPlanetRadius);
        for (const DabParams& c : centres)
            svc.ApplySphereSculptDab(c.X, c.Y, c.Z, angR, 5.0f, false);
        svc.CopySphereSculptUpload(controlPool, controlTable, geom);
    }

    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    svc.ConfigurePlanetSculpt(kPlanetRadius);
    svc.SetPlanetAnalyticModifiers(placements.data(), kFlattens);

    svc.BeginSphereSculptStrokeCapture();
    svc.ApplySphereSculptDabStroked(centres[0].X, centres[0].Y, centres[0].Z, angR, 5.0f, false);
    EXPECT_EQ(svc.GetPlanetSculptMirror().TransientDabCount, 1u);
    // Second distinct centre: budget exhausted -> the first dab commits to the store mid-stroke.
    svc.ApplySphereSculptDabStroked(centres[1].X, centres[1].Y, centres[1].Z, angR, 5.0f, false);
    {
        const auto mirror = svc.GetPlanetSculptMirror();
        EXPECT_EQ(mirror.TransientDabCount, 1u) << "budget 1: newest stays transient";
        const float32 len = std::sqrt(centres[0].X * centres[0].X + centres[0].Y * centres[0].Y +
                                      centres[0].Z * centres[0].Z);
        EXPECT_GT(CBTTerrain::SampleSphereSculptByDir(mirror.Sampler, centres[0].X / len,
                                                      centres[0].Y / len, centres[0].Z / len),
                  1.0f)
            << "the overflowed oldest dab must already be committed (commit-as-you-go)";
    }
    svc.ApplySphereSculptDabStroked(centres[2].X, centres[2].Y, centres[2].Z, angR, 5.0f, false);
    svc.CommitSphereSculptStroke();
    svc.TakeSphereSculptStrokeCapture();

    std::vector<float32> pool;
    std::vector<uint32> table;
    svc.CopySphereSculptUpload(pool, table, geom);
    EXPECT_EQ(table, controlTable);
    EXPECT_EQ(pool, controlPool)
        << "chunked commits must preserve FIFO order -> byte-identical final store";
}

// The transient composes WITH the published modifier placements through every chokepoint (S2
// bake-rect + placement-set interactions hold with the transient in play): a flatten placement
// and a held dab both appear in the composed sample, and the commit adds no double-apply.
TEST(TerrainRegionBake, TransientDabComposesWithAnalyticFlattenPlacements)
{
    ScopedAnalyticModifiersFlag analyticOn("1");
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    constexpr float32 kPlanetRadius = 50000.0f;
    svc.ConfigurePlanetSculpt(kPlanetRadius);

    // A published flatten placement (the S2 path, service-level).
    CBTTerrain::SphereAnalyticFlatten flat = CBTTerrain::MakeSphereAnalyticFlatten(
        kPlanetRadius, 0.0f, 0.0f, 200.0f, 0.0f, kPlanetRadius + 10.0f, kPlanetRadius);
    svc.SetPlanetAnalyticModifiers(&flat, 1u);

    // A held dab INSIDE the pad: composed = flatten + dab (relief 0 here).
    svc.BeginSphereSculptStrokeCapture();
    svc.ApplySphereSculptDabStroked(1.0f, 0.001f, 0.0f, 50.0f / kPlanetRadius, 5.0f, false);

    const float32 len = std::sqrt(1.0f + 0.001f * 0.001f);
    const float32 composed = svc.SampleSphereSculptHeight(1.0f / len, 0.001f / len, 0.0f, 0.0f);
    EXPECT_NEAR(composed, 10.0f + 5.0f, 1e-3f)
        << "composed must be flatten target-offset + dab amplitude (no cross-talk)";

    svc.CommitSphereSculptStroke();
    svc.TakeSphereSculptStrokeCapture();
    const float32 committed = svc.SampleSphereSculptHeight(1.0f / len, 0.001f / len, 0.0f, 0.0f);
    // The committed store quantizes the dab (100 m-class footprint at 8 m texels: small error)
    // but must NOT double-apply either term.
    EXPECT_NEAR(committed, 15.0f, 1.0f);
    EXPECT_EQ(svc.GetPlanetSculptMirror().TransientDabCount, 0u);
}

// DERIVED-WHEN-CHANGED oracle for the planet's base relief (part A: the terrain-state hash).
// A Flatten volume bakes the OFFSET that cancels the relief under it, so the composed surface
// sits at the target radius. That offset is derived from the relief — when the Inspector's
// "Planet Relief" Amplitude moves, the offset must be re-derived, exactly the way dragging
// "Planet Radius" three rows above already re-bakes.
//
// Failure mode it pins: the pad keeps the offset that cancelled the OLD relief, so it acquires a
// bulge shaped like (old relief - new relief) — flat no longer, and at the wrong radius.
TEST(TerrainRegionBake, SphereReliefEditReDerivesFlattenAgainstNewRelief)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    const TerrainHandle handle = CreateBakedTerrain(svc);
    const ECS::EntityHandle planetE = CreatePlanetEntity(world, handle); // PlanetRadius = 2000

    constexpr float32 kFrequency = 6.0f;
    constexpr uint32 kOctaves = 3u;
    constexpr float32 kAmplitudeBefore = 200.0f;
    constexpr float32 kAmplitudeAfter = 80.0f;

    Components::TerrainPlanetRelief relief{};
    relief.Amplitude = kAmplitudeBefore;
    relief.Frequency = kFrequency;
    relief.Octaves = kOctaves;
    world.AddComponentImmediate<Components::TerrainPlanetRelief>(planetE, relief);

    constexpr float32 kPlanetRadius = 2000.0f;
    const Dir3 dirN = Normalize3({1.0f, 0.7f, 0.4f});
    constexpr float32 kTargetRadius = 2200.0f;
    const Dir3 pos = {dirN.x * kTargetRadius, dirN.y * kTargetRadius, dirN.z * kTargetRadius};
    CreateSphereFlattenModifier(world, pos, /*radius*/ 250.0f, /*falloff*/ 50.0f,
                                /*useEntityHeight*/ true, /*targetHeight*/ 0.0f);
    world.SwapLifecycleEvents();

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f); // initial full sphere bake against amplitude 200
    const uint64 versionAfterFirstBake = svc.SphereSculptVersion();
    ASSERT_GT(versionAfterFirstBake, 0u) << "the flatten bake must produce a sphere sculpt";

    // A quiescent scene must stay quiescent, so "the version moved" below means the EDIT moved it.
    system.Update(world, 1.0f / 60.0f);
    ASSERT_EQ(svc.SphereSculptVersion(), versionAfterFirstBake)
        << "an idle tick re-baked — the oracle below is not discriminating";

    // The edit: drag Amplitude in the Planet Relief inspector section.
    {
        auto* r = world.GetComponentForWrite<Components::TerrainPlanetRelief>(planetE);
        ASSERT_NE(r, nullptr);
        r->Amplitude = kAmplitudeAfter;
    }

    system.Update(world, 1.0f / 60.0f); // must wake the gather AND move the state hash

    EXPECT_GT(svc.SphereSculptVersion(), versionAfterFirstBake)
        << "a relief edit did not re-bake the sphere: the terrain-state hash never moved, so the "
           "flatten still emits the offset that cancelled the OLD relief";

    auto reliefAt = [&](Dir3 d, float32 amplitude) {
        return CBTTerrain::PlanetRelief(d.x, d.y, d.z, amplitude, kFrequency, kOctaves);
    };
    // The composed surface the renderer draws NOW: base + the NEW relief + the baked offset.
    auto surfaceRadiusAt = [&](Dir3 d) {
        return kPlanetRadius + reliefAt(d, kAmplitudeAfter)
               + svc.SampleSphereSculptHeight(d.x, d.y, d.z, 0.0f);
    };

    // Fan of directions strictly inside the footprint (Radius/R = 0.125 rad; sample <= 0.09).
    float32 surfMin = 1e30f, surfMax = -1e30f, surfSum = 0.0f;
    float32 deltaMin = 1e30f, deltaMax = -1e30f;
    int32 count = 0;
    for (float32 a : {0.0f, 0.03f, 0.06f, 0.09f})
        for (float32 phi : {0.0f, 1.57079633f, 3.14159265f, 4.71238898f})
        {
            const Dir3 d = DirAtAngularOffset(dirN, a, phi);
            const float32 s = surfaceRadiusAt(d);
            surfMin = std::min(surfMin, s);
            surfMax = std::max(surfMax, s);
            surfSum += s;
            // The bulge the stale bake would leave: (old relief - new relief) over the footprint.
            const float32 bulge = reliefAt(d, kAmplitudeBefore) - reliefAt(d, kAmplitudeAfter);
            deltaMin = std::min(deltaMin, bulge);
            deltaMax = std::max(deltaMax, bulge);
            ++count;
            if (a == 0.0f)
                break; // the centre is one point regardless of azimuth
        }
    const float32 surfSpread = surfMax - surfMin;
    const float32 surfMean = surfSum / static_cast<float32>(count);
    const float32 bulgeSpread = deltaMax - deltaMin;

    // Discrimination: a stale bake would visibly deform this footprint, so a flat result is a
    // real re-derivation and not an amplitude change too small to see.
    ASSERT_GT(bulgeSpread, 10.0f)
        << "test not discriminating: the amplitude change barely deforms the pad";

    // The pad is still flat, and still at the target radius, against the NEW relief.
    EXPECT_LT(surfSpread, std::max(1.5f, 0.15f * bulgeSpread))
        << "the pad bulged by (old relief - new relief): the flatten offset was never re-derived";
    EXPECT_NEAR(surfMean, kTargetRadius, std::max(1.5f, 0.15f * bulgeSpread))
        << "the pad no longer levels to its target radius after the relief edit";
}

// The reported inspector-vs-gizmo asymmetry, end to end. The viewport gizmo
// writes WorldTransform itself (SceneViewTransformTool's PreviewWorldTransform),
// so the modifier's column is stamped by that write and this system's
// Changed<WorldTransform> probe fires. The inspector writes only the local
// Transform and leaves WorldTransform to TransformHierarchySystem — which
// propagates through pointers cached at topology rebuild, outside any query
// visit, so the propagation has to issue the write-grant stamp itself.
//
// The modifier is parented so the hierarchy stays on its cached path: the flat
// parallel path writes through a Write<WorldTransform> query whose stamp-at-visit
// covers the column for free, and would hide the gap entirely.
TEST(TerrainRegionBake, ModifierMovedByLocalTransformEditRebakes)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const TerrainHandle handle = CreateBakedTerrain(svc);
    auto* data = svc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    RegisterModifierLifecycleEvents(world);
    CreateTerrainEntity(world, handle);

    const ECS::EntityHandle root = world.CreateEntity();
    world.AddComponentImmediate<Components::Transform>(root, Components::Transform{});

    // Authored exactly as a scene entity is: modifier + local Transform +
    // Parent, and NO WorldTransform. The hierarchy derives that.
    const ECS::EntityHandle mod = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Circle;
    vol.Radius = 25.0f;
    vol.Falloff = 8.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(mod, vol);

    Components::TerrainFlattenEffect flatten{};
    flatten.UseVolumeHeight = false;
    flatten.TargetHeight = 20.0f;
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(mod, flatten);
    world.AddComponentImmediate<Components::Transform>(
        mod,
        Components::Transform::FromTRS(GameEngine::Mathematics::Vector3(30.0f, 0.0f, 40.0f),
                                       GameEngine::Mathematics::Quaternion{},
                                       GameEngine::Mathematics::Vector3(1.0f, 1.0f, 1.0f)));
    Components::Parent parent{};
    parent.Entity = root;
    world.AddComponentImmediate<Components::Parent>(mod, parent);

    GameEngine::Engine::Renderer::TransformHierarchySystem hierarchy;
    TerrainModifierSystem system;
    constexpr float32 kDt = 1.0f / 60.0f;

    auto tick = [&] {
        hierarchy.Update(world, kDt);
        system.Update(world, kDt);
    };

    tick(); // WorldTransform derived, modifier baked at (30, 40)
    ASSERT_NE(world.GetComponent<Components::WorldTransform>(mod), nullptr)
        << "hierarchy did not derive a WorldTransform; the rest of the test is vacuous";

    // Settle to idle. Without this the final assertion would also pass on a
    // system that simply re-bakes every frame — the idle check below is what
    // makes HeightfieldVersion a discriminating signal rather than a clock.
    for (int i = 0; i < 4; ++i)
        tick();
    const uint64 idleVersion = data->HeightfieldVersion;
    ASSERT_GT(idleVersion, 0u) << "no initial bake happened";
    tick();
    ASSERT_EQ(data->HeightfieldVersion, idleVersion)
        << "the modifier system re-bakes while idle, so it cannot discriminate a real move";

    const auto topologyBuildsBeforeEdit = hierarchy.GetTopologyBuildCount();

    // The inspector's write: the local Transform, and nothing else. A data-only
    // component set — no archetype move, so no topology rebuild to stamp
    // everything on the way past.
    world.AddComponentImmediate<Components::Transform>(
        mod,
        Components::Transform::FromTRS(GameEngine::Mathematics::Vector3(-20.0f, 0.0f, -35.0f),
                                       GameEngine::Mathematics::Quaternion{},
                                       GameEngine::Mathematics::Vector3(1.0f, 1.0f, 1.0f)));
    tick();

    ASSERT_EQ(hierarchy.GetTopologyBuildCount(), topologyBuildsBeforeEdit)
        << "topology rebuilt: it stamps every chunk, so this run proves nothing";
    const auto* movedXf = world.GetComponent<Components::WorldTransform>(mod);
    ASSERT_NE(movedXf, nullptr);
    ASSERT_FLOAT_EQ(movedXf->matrix[12], -20.0f)
        << "hierarchy never propagated the edit; the bake assertion below would be vacuous";

    EXPECT_GT(data->HeightfieldVersion, idleVersion)
        << "a modifier moved by a local Transform edit (the inspector path) did not re-bake";
}
