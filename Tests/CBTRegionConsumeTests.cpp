// CBTUpdateSystem::ConsumeDirtyRegion integration coverage (adversarial-review C5).
// Drives the real ECS -> TerrainService -> classify path that C4/C5 rely on, with a
// real slot-0 terrain, and asserts:
//   * C1: the FIRST terrain (TerrainService slot 0 == handle index 0, generation 1)
//     yields a non-empty dirty rect. The pre-fix guard treated handle-index 0 as
//     "unset" and early-returned, so CBT's default single-terrain scene was inert.
//   * m3: the log cursor only COMMITS once the render side confirms it recorded the
//     pushed rect (the update-record count advanced). A frame whose update never ran
//     re-arms the rect instead of silently dropping the edit.
// Engine-linked (TerrainService lives in TerrainECS -> Engine.dll); modelled on
// TerrainRegionBakeTests.

#include <gtest/gtest.h>

#include "CBTTerrain/CBTInstance.h"          // CBTClassifyDesc
#include "CBTTerrain/CBTSphereFaceMap.h"     // SphereEditRegions (cross-face drain)
#include "CBTTerrainECS/Systems/CBTUpdateSystem.h"
#include "CBTTerrainECS/Systems/RegisterCBTSystems.h"
#include "Components/Terrain/Terrain.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECS/World.h"
#include "Rendering/Core/Device.h"
#include "Scene/SceneSchemaRegistry.h"
#include "SplineECS/Systems/RegisterSplineSystems.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/Planet/PlanetSculptDab.h"
#include "TerrainECS/TerrainHeightPageDriver.h"
#include "TerrainECS/TerrainPageFeature.h"
#include "TerrainECS/TerrainPages.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/Systems/RegisterTerrainSystems.h"
#include "TerrainECS/Systems/TerrainWorldHooks.h"
#include "TerrainPagedField.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using GameEngine::CBTTerrainECS::CBTUpdateSystem;

// Standalone headless Vulkan device for the TerrainRenderFeature extraction-math oracle
// (mirrors CBTTestHarness::MakeHeadlessDevice; that helper lives in the CBTTerrain test
// tree, not linkable here). Returns null when no device is available -> the oracle skips.
std::unique_ptr<Rendering::IDevice> MakeHeadlessDeviceOrNull()
{
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    Rendering::DeviceDesc dd{};
    dd.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto device = Rendering::DeviceFactory::CreateDevice(dd);
    if (!device || !device->Initialize(dd))
        return nullptr;
    return device;
}

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

constexpr uint32 kHeightmapDim = 129;

Terrain::TerrainConfig MakeConfig()
{
    Terrain::TerrainConfig cfg{};
    cfg.HeightmapWidth = kHeightmapDim;
    cfg.HeightmapHeight = kHeightmapDim;
    cfg.WorldSizeX = 256.0f;
    cfg.WorldSizeZ = 256.0f;
    cfg.HeightScale = 64.0f;
    cfg.LODLevels = 4;
    return cfg;
}

// One terrain entity carrying the service handle exactly as TerrainExtractionSystem
// wires it (index + generation).
void AddTerrainEntity(ECS::World& world, TerrainHandle handle)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
}

// A TILED terrain entity exactly as TerrainExtractionSystem wires it once a terrain
// auto-tiles: the single-terrain handle is ZEROED and the tiled handle carries the
// live slot. `enabled` lets a test cover the disabled case.
ECS::EntityHandle AddTiledTerrainEntity(ECS::World& world, uint32 tiledIndex,
                                        uint32 tiledGeneration, bool enabled = true)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.TerrainDataHandle = 0;
    terrain.TerrainDataGeneration = 0;
    terrain.TiledTerrainHandle = tiledIndex;
    terrain.TiledTerrainGeneration = tiledGeneration;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    ECS::Entity(&world, e).SetEnabled<Components::Terrain>(enabled);
    return e;
}

bool RectIsEmpty(const CBTTerrain::CBTClassifyDesc& c)
{
    return c.DirtyMaxU <= c.DirtyMinU || c.DirtyMaxV <= c.DirtyMinV;
}

// The face rect the classify carries for `face`, or null.
const CBTTerrain::SphereFaceUVRect* FaceRect(const CBTTerrain::CBTClassifyDesc& c, uint32 face)
{
    for (uint32 i = 0; i < c.SphereDirty.Count; ++i)
        if (c.SphereDirty.Rects[i].Face == face)
            return &c.SphereDirty.Rects[i];
    return nullptr;
}

// One touched (face, face-local UV rect) region — the single-face-edit case.
CBTTerrain::SphereEditRegions OneFace(uint32 face, float minU, float minV, float maxU, float maxV)
{
    CBTTerrain::SphereEditRegions r{};
    r.Count = 1u;
    r.Rects[0] = CBTTerrain::SphereFaceUVRect{face, minU, minV, maxU, maxV};
    return r;
}

// `count` frames of the service's height pages for `request`, from a camera 5 m above face +X. Each frame
// copies the planet's overlay into the request before the update, as RequestPlanetHeightPages does.
void RunPlanetPageFrames(TerrainService& service, TerrainRenderFeature& renderFeature, TerrainPageFeature& pageFeature,
                         PlanetPagedRequest& request, uint64& frame, uint64 count)
{
    const Mathematics::Vector3 camera{2005.0f, 0.0f, 0.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    for (const uint64 end = frame + count; frame < end; ++frame)
    {
        if (const TerrainService::PlanetPageOverlay* overlay = service.FindPlanetPageOverlay(request.Planet))
        {
            request.Overlay = overlay->Layer;
            request.Modifiers = overlay->Modifiers;
            request.OverlayVersion = overlay->Version;
            request.OverlayChanged = overlay->Changed;
            request.OverlayChangedSince = overlay->ChangedSince;
        }
        service.GetTerrainPages().RequestPlanet(request, {&camera, 1}, view);
        UpdateHeightPages(service, renderFeature, pageFeature, frame, 1.0f / 60.0f, 2);
    }
}
} // namespace

// C1 regression guard: slot 0 (handle index 0, generation 1) must reclassify. This
// FAILS if ConsumeDirtyRegion's terrain guard reverts to the `handle == 0` sentinel.
TEST(CBTRegionConsume, FirstTerrainSlotYieldsNonEmptyRect)
{
    ScopedTerrainService svc;
    const TerrainHandle handle = TerrainService::Get().CreateTerrain(MakeConfig());
    ASSERT_EQ(handle.Index, 0u) << "first terrain must occupy slot 0 (the overloaded-handle case)";
    ASSERT_NE(handle.Generation, 0u);

    ECS::World world;
    AddTerrainEntity(world, handle);

    // CreateTerrain seeds a full-terrain dirty entry (MarkFullDirty at version 1);
    // a fresh CBT cursor (version 0) must collect it -> the whole UV square.
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc classify{};
    system.ConsumeDirtyRegion(world, /*updateRecordCount=*/0u, classify);

    EXPECT_FALSE(RectIsEmpty(classify))
        << "the default single-terrain scene produced an empty dirty rect — C5 is inert";
    EXPECT_NEAR(classify.DirtyMinU, 0.0f, 1e-3f);
    EXPECT_NEAR(classify.DirtyMinV, 0.0f, 1e-3f);
    EXPECT_NEAR(classify.DirtyMaxU, 1.0f, 1e-3f);
    EXPECT_NEAR(classify.DirtyMaxV, 1.0f, 1e-3f);
}

// m3: with no confirmation (the update-record count never advances), the pushed rect
// must be RE-ARMED every frame, not committed-then-dropped. FAILS if the cursor
// advances unconditionally at consume time.
TEST(CBTRegionConsume, UnconfirmedEditIsReArmedNotDropped)
{
    ScopedTerrainService svc;
    const TerrainHandle handle = TerrainService::Get().CreateTerrain(MakeConfig());
    ECS::World world;
    AddTerrainEntity(world, handle);

    CBTUpdateSystem system(nullptr);

    // Frame A: push the full-dirty rect at record count 0.
    CBTTerrain::CBTClassifyDesc a{};
    system.ConsumeDirtyRegion(world, 0u, a);
    ASSERT_FALSE(RectIsEmpty(a));

    // Frame B: same record count (0) => no update recorded the rect => re-arm.
    CBTTerrain::CBTClassifyDesc b{};
    system.ConsumeDirtyRegion(world, 0u, b);
    EXPECT_FALSE(RectIsEmpty(b)) << "an unconfirmed edit was dropped instead of re-armed";
}

// m3: once the render side confirms (record count advanced), the cursor commits and a
// quiescent terrain produces an empty rect; a subsequent NEW edit re-arms again.
TEST(CBTRegionConsume, ConfirmedEditBecomesQuiescentThenReArmsOnNewEdit)
{
    ScopedTerrainService svc;
    const TerrainHandle handle = TerrainService::Get().CreateTerrain(MakeConfig());
    ECS::World world;
    AddTerrainEntity(world, handle);
    TerrainData* data = TerrainService::Get().GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    CBTUpdateSystem system(nullptr);

    // Push at record count 0.
    CBTTerrain::CBTClassifyDesc push{};
    system.ConsumeDirtyRegion(world, 0u, push);
    ASSERT_FALSE(RectIsEmpty(push));

    // Record count advanced to 1 => the pushed rect was recorded => commit. Nothing
    // newer in the log => quiescent (empty rect).
    CBTTerrain::CBTClassifyDesc quiescent{};
    system.ConsumeDirtyRegion(world, 1u, quiescent);
    EXPECT_TRUE(RectIsEmpty(quiescent)) << "confirmed edit still re-armed — the cursor never committed";

    // A new region edit must reach the classify again (still record count 1: the new
    // edit is a fresh in-flight rect, not yet confirmed).
    data->MarkRegionDirty(16, 16, 48, 48);
    CBTTerrain::CBTClassifyDesc reedit{};
    system.ConsumeDirtyRegion(world, 1u, reedit);
    EXPECT_FALSE(RectIsEmpty(reedit)) << "a new edit after a committed one was not reclassified";
}

// ---------------------------------------------------------------------------
// Planet editing (plan §planet-editing): ConsumePlanetLayerEdits — the (face,rect) analogue
// of ConsumeDirtyRegion. `regions` is the clear-on-read drain from TerrainService, so the
// tests pass a NON-EMPTY drain on an edit frame and an EMPTY drain on subsequent frames
// (the service already cleared it). Every touched face goes to the GPU in the same update
// and is re-armed from the pending set (not a fresh ingest) until the render side confirms
// it recorded.
// ---------------------------------------------------------------------------

// No edits -> no rect is dirty (quiescence: the default planet pays nothing).
TEST(CBTRegionConsume, SphereSculptQuiescentUntilEdit)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc c{};
    system.ConsumePlanetLayerEdits(/*updateRecordCount=*/0u, CBTTerrain::SphereEditRegions{}, c);
    EXPECT_FALSE(c.HasDirtyRegion());
}

// An edit pushes the edited face's (face-local) rect into the classify; the planar rect
// stays empty.
TEST(CBTRegionConsume, SphereSculptEditPushesFaceRect)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc c{};
    system.ConsumePlanetLayerEdits(0u, OneFace(2u, 0.25f, 0.30f, 0.55f, 0.60f), c);
    EXPECT_TRUE(RectIsEmpty(c));
    ASSERT_EQ(c.SphereDirty.Count, 1u);
    const CBTTerrain::SphereFaceUVRect* rect = FaceRect(c, 2u);
    ASSERT_NE(rect, nullptr);
    EXPECT_NEAR(rect->MinU, 0.25f, 1e-4f);
    EXPECT_NEAR(rect->MaxV, 0.60f, 1e-4f);
}

// An unconfirmed edit (record count never advances) is RE-ARMED from the pending set, not
// dropped — the same no-scene-view guard the planar path carries. The second frame drains EMPTY
// (the service already cleared it), so a passing re-arm proves the pending set held the face.
TEST(CBTRegionConsume, SphereSculptUnconfirmedReArmed)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc a{};
    system.ConsumePlanetLayerEdits(0u, OneFace(0u, 0.1f, 0.1f, 0.4f, 0.4f), a);
    ASSERT_TRUE(a.HasDirtyRegion());
    CBTTerrain::CBTClassifyDesc b{};
    system.ConsumePlanetLayerEdits(0u, CBTTerrain::SphereEditRegions{}, b);
    EXPECT_NE(FaceRect(b, 0u), nullptr) << "an unconfirmed sphere edit was dropped instead of re-armed";
}

// Once confirmed (record count advanced), the face goes quiescent (no rect); a new edit
// re-arms.
TEST(CBTRegionConsume, SphereSculptConfirmedQuiescentThenReArms)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc push{};
    system.ConsumePlanetLayerEdits(0u, OneFace(3u, 0.2f, 0.2f, 0.5f, 0.5f), push);
    ASSERT_TRUE(push.HasDirtyRegion());

    // Record count advanced -> the pushed rect was recorded -> commit; drain is now EMPTY.
    CBTTerrain::CBTClassifyDesc quiescent{};
    system.ConsumePlanetLayerEdits(1u, CBTTerrain::SphereEditRegions{}, quiescent);
    EXPECT_FALSE(quiescent.HasDirtyRegion()) << "confirmed sphere edit still re-armed";

    // A new edit reclassifies again.
    CBTTerrain::CBTClassifyDesc reedit{};
    system.ConsumePlanetLayerEdits(1u, OneFace(4u, 0.6f, 0.6f, 0.8f, 0.8f), reedit);
    EXPECT_NE(FaceRect(reedit, 4u), nullptr) << "a new sphere dab after a committed one was not reclassified";
}

// A dab across a cube edge, or a page on a face side, changes two faces. Both reclassify in the
// update the change lands in (one face an update leaves the neighbor face a frame stale), then quiescent.
TEST(CBTRegionConsume, SphereSculptCrossFaceReclassifiesBothFacesInOneUpdate)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::SphereEditRegions regions{};
    regions.Count = 2u;
    regions.Rects[0] = CBTTerrain::SphereFaceUVRect{0u, 0.80f, 0.40f, 1.00f, 0.60f}; // +X edge band
    regions.Rects[1] = CBTTerrain::SphereFaceUVRect{2u, 0.00f, 0.40f, 0.20f, 0.60f}; // +Y edge band

    CBTTerrain::CBTClassifyDesc c1{};
    system.ConsumePlanetLayerEdits(0u, regions, c1);
    ASSERT_EQ(c1.SphereDirty.Count, 2u) << "the neighbor face waits for a later update";
    for (uint32 i = 0; i < 2u; ++i)
    {
        const CBTTerrain::SphereFaceUVRect* rect = FaceRect(c1, regions.Rects[i].Face);
        ASSERT_NE(rect, nullptr) << "face " << regions.Rects[i].Face;
        EXPECT_NEAR(rect->MinU, regions.Rects[i].MinU, 1e-4f);
        EXPECT_NEAR(rect->MinV, regions.Rects[i].MinV, 1e-4f);
        EXPECT_NEAR(rect->MaxU, regions.Rects[i].MaxU, 1e-4f);
        EXPECT_NEAR(rect->MaxV, regions.Rects[i].MaxV, 1e-4f);
    }

    CBTTerrain::CBTClassifyDesc c2{};
    system.ConsumePlanetLayerEdits(1u, CBTTerrain::SphereEditRegions{}, c2);
    EXPECT_FALSE(c2.HasDirtyRegion()) << "both faces recorded but Classify still dirty — not quiescent";
}

// A corner edit touches THREE faces; all three reclassify in one update, then quiescent.
TEST(CBTRegionConsume, SphereSculptCornerReclassifiesThreeFacesInOneUpdate)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::SphereEditRegions regions{};
    regions.Count = 3u;
    regions.Rects[0] = CBTTerrain::SphereFaceUVRect{0u, 0.90f, 0.90f, 1.00f, 1.00f};
    regions.Rects[1] = CBTTerrain::SphereFaceUVRect{2u, 0.00f, 0.90f, 0.10f, 1.00f};
    regions.Rects[2] = CBTTerrain::SphereFaceUVRect{4u, 0.90f, 0.00f, 1.00f, 0.10f};

    CBTTerrain::CBTClassifyDesc c{};
    system.ConsumePlanetLayerEdits(0u, regions, c);
    EXPECT_EQ(c.SphereDirty.Count, 3u);
    EXPECT_TRUE(FaceRect(c, 0u) && FaceRect(c, 2u) && FaceRect(c, 4u));

    // A rect that grows before its update records is pushed again at its new extent.
    CBTTerrain::CBTClassifyDesc grown{};
    system.ConsumePlanetLayerEdits(0u, OneFace(2u, 0.00f, 0.80f, 0.10f, 1.00f), grown);
    ASSERT_NE(FaceRect(grown, 2u), nullptr);
    EXPECT_NEAR(FaceRect(grown, 2u)->MinV, 0.80f, 1e-4f);
    EXPECT_EQ(grown.SphereDirty.Count, 3u);

    CBTTerrain::CBTClassifyDesc quiescent{};
    system.ConsumePlanetLayerEdits(1u, CBTTerrain::SphereEditRegions{}, quiescent);
    EXPECT_FALSE(quiescent.HasDirtyRegion()) << "all corner faces recorded but Classify still dirty";
}

// C8 draw-gate oracle: CBT is the only terrain renderer and CBTRenderNode emits its
// indirect draw ONLY when the feature is active; the feature is active iff
// ComputeTerrainActive is true. A TILED terrain zeroes its single-terrain handle and
// carries only TiledTerrainHandle, so the pre-C8 single-handle-only gate returned FALSE
// for it — CBT never activated, no draw was emitted, and the terrain vanished at the
// tiling flip. This asserts the gate opens for a tiled-only terrain (DISCRIMINATING:
// reverting ComputeTerrainActive to the single-handle-only test fails this).
TEST(CBTRegionConsume, TiledTerrainActivatesCBTDrawGate)
{
    ECS::World world;
    AddTiledTerrainEntity(world, /*tiledIndex=*/0u, /*tiledGeneration=*/1u);
    EXPECT_TRUE(CBTUpdateSystem::ComputeTerrainActive(world))
        << "a tiled terrain (single handle zeroed) left CBT inactive — the vanish bug";
}

TEST(CBTRegionConsume, SingleTerrainActivatesCBTDrawGate)
{
    ScopedTerrainService svc;
    const TerrainHandle handle = TerrainService::Get().CreateTerrain(MakeConfig());
    ECS::World world;
    AddTerrainEntity(world, handle);
    EXPECT_TRUE(CBTUpdateSystem::ComputeTerrainActive(world));
}

TEST(CBTRegionConsume, NoHandleOrDisabledTerrainLeavesCBTInactive)
{
    ECS::World empty;
    EXPECT_FALSE(CBTUpdateSystem::ComputeTerrainActive(empty))
        << "no terrain entity should leave CBT quiescent";

    ECS::World disabled;
    AddTiledTerrainEntity(disabled, 0u, 1u, /*enabled=*/false);
    EXPECT_FALSE(CBTUpdateSystem::ComputeTerrainActive(disabled))
        << "a disabled tiled terrain must not activate CBT";
}

// C8 delete-leak oracle: removing a Terrain component must free a TILED terrain's data
// (and its unified GPU textures). Extraction ZEROES TerrainDataHandle for a tiled
// terrain and carries only the tiled pair, so a release that checks the single handle
// alone strands the tiled store plus up to ~805 MiB of unified textures until app exit.
// DISCRIMINATING: drop the tiled half of ReleaseTerrainOwnedResources and the store is
// still non-null here.
//
// Release is the world's OnRemove<Terrain> hook, so the world needs the registrar the
// engine installs on its primary world — a world without it holds Terrain components
// that free nothing. That is the contract, and it is why this registers explicitly
// rather than relying on the schema to release by itself.
TEST(CBTRegionConsume, RemoveTiledTerrainComponentReleasesTiledStore)
{
    ScopedTerrainService svc;

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 4096.0f;
    cfg.WorldSizeZ = 4096.0f;
    cfg.HeightScale = 200.0f;
    cfg.SamplesPerMeter = 1.0f;
    cfg.PatchGridSize = 32;
    const TiledTerrainHandle tiled = TerrainService::Get().CreateTiledTerrain(cfg);
    TerrainService::Get().LoadTile(tiled, {0, 0});
    ASSERT_NE(TerrainService::Get().GetTiledTerrainData(tiled), nullptr);

    // Entity carrying ONLY the tiled handle (single zeroed), as extraction wires it.
    ECS::World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world); // as EnsurePrimaryWorld does
    const ECS::EntityHandle e = AddTiledTerrainEntity(world, tiled.Index, tiled.Generation);

    const auto* schema = GameEngine::Scene::SceneSchemaRegistry::Find("Terrain");
    ASSERT_NE(schema, nullptr) << "Terrain scene schema not registered";
    EXPECT_TRUE(schema->Remove(world, e));

    EXPECT_EQ(TerrainService::Get().GetTiledTerrainData(tiled), nullptr)
        << "tiled terrain data leaked after component removal";
    EXPECT_EQ(world.GetComponent<Components::Terrain>(e), nullptr);
}

// ---------------------------------------------------------------------------
// Tiled streaming (E6): ConsumeUnifiedDirtyRect — the tiled analogue of
// ConsumeDirtyRegion. A tiled terrain's edits (tile streaming + modifier re-bakes) land
// in the unified GPU texture, which TerrainRenderFeature accumulates into a UV dirty rect;
// CBTUpdateSystem drains it here with the SAME confirm / re-arm discipline as the planar
// log path so a rect drained on a non-recording frame is re-armed, not dropped. Drives the
// discipline directly (no device / RenderServices), like the ConsumePlanetLayerEdits tests.
// ---------------------------------------------------------------------------

// No upload since the last drain -> the rect stays empty (streaming quiescence: a parked
// camera over settled tiles must publish nothing so Classify stays idle-zero-work).
TEST(CBTRegionConsume, UnifiedDirtyQuiescentUntilUpload)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc c{};
    system.ConsumeUnifiedDirtyRect(/*takenHasRect=*/false, 0, 0, 0, 0, /*recordCount=*/0u, c);
    EXPECT_TRUE(RectIsEmpty(c));
}

// A drained upload rect reaches the classify.
TEST(CBTRegionConsume, UnifiedDirtyUploadPushesRect)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc c{};
    system.ConsumeUnifiedDirtyRect(true, 0.25f, 0.30f, 0.55f, 0.60f, 0u, c);
    EXPECT_FALSE(RectIsEmpty(c));
    EXPECT_NEAR(c.DirtyMinU, 0.25f, 1e-4f);
    EXPECT_NEAR(c.DirtyMinV, 0.30f, 1e-4f);
    EXPECT_NEAR(c.DirtyMaxU, 0.55f, 1e-4f);
    EXPECT_NEAR(c.DirtyMaxV, 0.60f, 1e-4f);
}

// An unconfirmed streamed rect (record count never advances) is RE-ARMED, not dropped —
// the same no-scene-view guard the planar + sphere paths carry.
TEST(CBTRegionConsume, UnifiedDirtyUnconfirmedReArmed)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc a{};
    system.ConsumeUnifiedDirtyRect(true, 0.1f, 0.1f, 0.4f, 0.4f, 0u, a);
    ASSERT_FALSE(RectIsEmpty(a));
    // Next frame: no NEW upload (takenHasRect=false) + same record count => re-arm the union.
    CBTTerrain::CBTClassifyDesc b{};
    system.ConsumeUnifiedDirtyRect(false, 0, 0, 0, 0, 0u, b);
    EXPECT_FALSE(RectIsEmpty(b)) << "an unconfirmed streamed rect was dropped instead of re-armed";
    EXPECT_NEAR(b.DirtyMinU, 0.1f, 1e-4f);
    EXPECT_NEAR(b.DirtyMaxU, 0.4f, 1e-4f);
}

// Once confirmed (record count advanced), a held rect goes quiescent (empty); a NEW upload
// re-arms again.
TEST(CBTRegionConsume, UnifiedDirtyConfirmedQuiescentThenReArms)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc push{};
    system.ConsumeUnifiedDirtyRect(true, 0.2f, 0.2f, 0.5f, 0.5f, 0u, push);
    ASSERT_FALSE(RectIsEmpty(push));

    // Record count advanced -> the pushed rect was recorded -> commit; no new upload.
    CBTTerrain::CBTClassifyDesc quiescent{};
    system.ConsumeUnifiedDirtyRect(false, 0, 0, 0, 0, 1u, quiescent);
    EXPECT_TRUE(RectIsEmpty(quiescent)) << "confirmed streamed rect still re-armed";

    // A new upload (still record count 1) reclassifies again.
    CBTTerrain::CBTClassifyDesc reupload{};
    system.ConsumeUnifiedDirtyRect(true, 0.6f, 0.6f, 0.8f, 0.8f, 1u, reupload);
    EXPECT_FALSE(RectIsEmpty(reupload)) << "a new upload after a committed one was not reclassified";
    EXPECT_NEAR(reupload.DirtyMinU, 0.6f, 1e-4f);
}

// Multiple uploads before a confirm union into their bounding rect (a multi-tile streaming
// frame, or several unconfirmed frames, must reclassify the whole touched footprint).
TEST(CBTRegionConsume, UnifiedDirtyUnionsAcrossUnconfirmedFrames)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc a{};
    system.ConsumeUnifiedDirtyRect(true, 0.1f, 0.1f, 0.3f, 0.3f, 0u, a);
    CBTTerrain::CBTClassifyDesc b{};
    system.ConsumeUnifiedDirtyRect(true, 0.5f, 0.5f, 0.7f, 0.7f, 0u, b); // still unconfirmed
    EXPECT_NEAR(b.DirtyMinU, 0.1f, 1e-4f);
    EXPECT_NEAR(b.DirtyMinV, 0.1f, 1e-4f);
    EXPECT_NEAR(b.DirtyMaxU, 0.7f, 1e-4f);
    EXPECT_NEAR(b.DirtyMaxV, 0.7f, 1e-4f);
}

// ---------------------------------------------------------------------------
// Extraction math (E6): TerrainRenderFeature's unified-height dirty accumulator maps a
// patched texel sub-rect to the correct UV AABB and unions across uploads. Device-backed
// (needs the unified textures); skips when no headless Vulkan device is available.
// ---------------------------------------------------------------------------
TEST(CBTRegionConsume, UnifiedHeightDirtyRectExtractionMath)
{
    auto device = MakeHeadlessDeviceOrNull();
    if (!device)
        GTEST_SKIP() << "no headless Vulkan device";

    {
        TerrainRenderFeature feature;
        ASSERT_TRUE(feature.Initialize(device.get()));

        const TerrainHandle handle{0u, 1u};
        constexpr uint32 kW = 128u;
        constexpr uint32 kH = 128u;
        feature.EnsureUnifiedTiledTextures(handle, kW, kH);

        constexpr float32 kTexel = 1.0f / 128.0f;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        uint64 ver = 0;
        // Nothing uploaded yet -> empty.
        EXPECT_FALSE(feature.PeekUnifiedHeightDirtyRect(handle, u0, v0, u1, v1, ver));

        // Interior 32x32 block at texel origin (32,64). The raw texel rect is [32,64..64,96];
        // the accumulator pads ONE texel each side (bilinear footprint), so the UV AABB is
        // exactly [31,63 .. 65,97] texels wide.
        const std::vector<float32> block(32u * 32u, 0.5f);
        feature.UploadHeightmapRegion(handle, block.data(), 32u, 32u, 32u, 64u);
        ASSERT_TRUE(feature.PeekUnifiedHeightDirtyRect(handle, u0, v0, u1, v1, ver));
        EXPECT_NEAR(u0, 31.0f * kTexel, 1e-5f) << "min U not padded one texel";
        EXPECT_NEAR(v0, 63.0f * kTexel, 1e-5f) << "min V not padded one texel";
        EXPECT_NEAR(u1, 65.0f * kTexel, 1e-5f) << "max U not padded one texel";
        EXPECT_NEAR(v1, 97.0f * kTexel, 1e-5f) << "max V not padded one texel";
        EXPECT_EQ(ver, 1u);

        // A second upload at texel origin (0,0): its padded min would go below 0 and must
        // CLAMP to 0 (not wrap negative); the union preserves the interior upload's max.
        feature.UploadHeightmapRegion(handle, block.data(), 32u, 32u, 0u, 0u);
        ASSERT_TRUE(feature.PeekUnifiedHeightDirtyRect(handle, u0, v0, u1, v1, ver));
        EXPECT_NEAR(u0, 0.0f, 1e-5f) << "edge-touching min U did not clamp to 0";
        EXPECT_NEAR(v0, 0.0f, 1e-5f) << "edge-touching min V did not clamp to 0";
        EXPECT_NEAR(u1, 65.0f * kTexel, 1e-5f);
        EXPECT_NEAR(v1, 97.0f * kTexel, 1e-5f);
        EXPECT_EQ(ver, 2u);

        // Take drains it -> a subsequent peek is empty; version stays monotonic (always
        // written, per the Take/Peek contract — even on the false return below).
        float t0 = 0, t1 = 0, t2 = 0, t3 = 0;
        uint64 tver = 0;
        ASSERT_TRUE(feature.TakeUnifiedHeightDirtyRect(handle, t0, t1, t2, t3, tver));
        EXPECT_EQ(tver, 2u);
        float d0 = 0, d1 = 0, d2 = 0, d3 = 0;
        uint64 dver = 0;
        EXPECT_FALSE(feature.PeekUnifiedHeightDirtyRect(handle, d0, d1, d2, d3, dver));
        EXPECT_EQ(dver, 2u) << "version must stay monotonic across a drain";

        // A block flush against the far edge (dstX+srcW == W): the padded max must CLAMP to
        // 1.0, and the accumulator is empty post-drain so this rect stands alone.
        feature.UploadHeightmapRegion(handle, block.data(), 32u, 32u, 96u, 96u);
        ASSERT_TRUE(feature.PeekUnifiedHeightDirtyRect(handle, u0, v0, u1, v1, ver));
        EXPECT_NEAR(u0, 95.0f * kTexel, 1e-5f);
        EXPECT_NEAR(v0, 95.0f * kTexel, 1e-5f);
        EXPECT_NEAR(u1, 1.0f, 1e-5f) << "edge-touching max U did not clamp to 1";
        EXPECT_NEAR(v1, 1.0f, 1e-5f) << "edge-touching max V did not clamp to 1";
        EXPECT_EQ(ver, 3u);

        feature.ReleaseTerrainResources(handle);
    } // feature destructs (destroys its textures) while the device is still alive

    device->Shutdown();
}

// Routing (E6, C1-mirage guard): ConsumeUnifiedTiledDirtyRegion must resolve the tiled
// terrain's TiledTerrainHandle -> GlobalGpuHandle (via TerrainService) and drain THAT
// handle's accumulator into the classify. The global handle is deliberately a DISTINCT slot
// from the tiled handle, so a resolution that used the tiled handle directly (the exact
// handle-disambiguation class that left a feature inert for months) would drain an empty
// slot and leave the rect empty -> this fails. Device-backed (needs the unified textures).
TEST(CBTRegionConsume, UnifiedTiledDirtyRoutingResolvesGlobalHandleAndLandsRect)
{
    auto device = MakeHeadlessDeviceOrNull();
    if (!device)
        GTEST_SKIP() << "no headless Vulkan device";

    ScopedTerrainService svc;
    {
        TerrainRenderFeature feature;
        ASSERT_TRUE(feature.Initialize(device.get()));

        TiledTerrainConfig cfg{};
        cfg.WorldSizeX = 4096.0f;
        cfg.WorldSizeZ = 4096.0f;
        cfg.HeightScale = 200.0f;
        cfg.SamplesPerMeter = 1.0f;
        cfg.PatchGridSize = 32;
        const TiledTerrainHandle tiled = TerrainService::Get().CreateTiledTerrain(cfg);
        TiledTerrainData* tiledData = TerrainService::Get().GetTiledTerrainData(tiled);
        ASSERT_NE(tiledData, nullptr);

        // Global GPU handle on a DISTINCT slot from the tiled handle (as extraction assigns
        // one via CreateTerrain). Seed the accumulator ONLY for this global handle.
        const TerrainHandle globalHandle{tiled.Index + 7u, 1u};
        tiledData->GlobalGpuHandleIndex = globalHandle.Index;
        tiledData->GlobalGpuHandleGeneration = globalHandle.Generation;

        constexpr uint32 kW = 128u;
        constexpr uint32 kH = 128u;
        feature.EnsureUnifiedTiledTextures(globalHandle, kW, kH);
        const std::vector<float32> block(32u * 32u, 0.5f);
        feature.UploadHeightmapRegion(globalHandle, block.data(), 32u, 32u, 32u, 64u);

        Components::Terrain tuning{};
        tuning.TiledTerrainHandle = tiled.Index;
        tuning.TiledTerrainGeneration = tiled.Generation;

        CBTUpdateSystem system(nullptr);
        CBTTerrain::CBTClassifyDesc classify{};
        system.ConsumeUnifiedTiledDirtyRegion(&feature, /*updateRecordCount=*/0u, tuning, classify);
        EXPECT_FALSE(RectIsEmpty(classify))
            << "tiled routing did not resolve the global handle / drain the rect";
        constexpr float32 kTexel = 1.0f / 128.0f;
        EXPECT_NEAR(classify.DirtyMinU, 31.0f * kTexel, 1e-4f);
        EXPECT_NEAR(classify.DirtyMaxV, 97.0f * kTexel, 1e-4f);

        feature.ReleaseTerrainResources(globalHandle);
    }
    device->Shutdown();
}

// A paged terrain's edit reaches the classify as the rect its pages publish: an edit of level-0
// samples 100 to 110 on a flat 257-sample tile reaches samples 96 to 116 at its coarsest level
// (TerrainHeightPagesTests), not the whole terrain its coarsest page covers. The pages' rects go
// into the accumulator as the height-page driver publishes them (UpdateHeightPages). Deviceless:
// the accumulator and the drain need no texture.
TEST(CBTRegionConsume, APagedTerrainEditReachesTheClassifyAsTheRectItsPagesPublish)
{
    ScopedTerrainService svc;
    TerrainRenderFeature feature;
    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 4096.0f;
    cfg.WorldSizeZ = 4096.0f;
    cfg.HeightScale = 200.0f;
    cfg.SamplesPerMeter = 1.0f;
    cfg.PatchGridSize = 32;
    const TiledTerrainHandle tiled = TerrainService::Get().CreateTiledTerrain(cfg);
    TiledTerrainData* tiledData = TerrainService::Get().GetTiledTerrainData(tiled);
    ASSERT_NE(tiledData, nullptr);
    const TerrainHandle globalHandle{tiled.Index + 3u, 1u};
    tiledData->GlobalGpuHandleIndex = globalHandle.Index;
    tiledData->GlobalGpuHandleGeneration = globalHandle.Generation;

    PagedTerrainRequest request = TerrainECS::Test::FlatTileRequest(257);
    request.Terrain = globalHandle.Index;
    request.TerrainGeneration = globalHandle.Generation;

    TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    for (; frame < 62; ++frame)
    {
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    pages.DirtyRects().clear();

    const PageStreaming::PageSampleRect edit{100, 100, 110, 110};
    auto layer = std::make_shared<PageStreaming::HeightPageLayer>(1u, PageStreaming::BuildPageStoreLevels(257, 257));
    layer->WriteLevel0(0, edit, std::vector<float32>(edit.Width() * edit.Height(), 5.0f));
    request.Overlay = layer;
    request.OverlayVersion = 1;
    request.OverlayChangedSince = 0;
    request.OverlayChanged = edit;
    for (; frame < 72; ++frame)
    {
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        for (const HeightPageDirtyRect& rect : pages.DirtyRects())
            feature.AccumulateAtlasHeightDirtyUV(TerrainHandle{rect.Terrain, rect.TerrainGeneration}, rect.MinU,
                                                 rect.MinV, rect.MaxU, rect.MaxV);
        pages.DirtyRects().clear();
    }

    Components::Terrain tuning{};
    tuning.TiledTerrainHandle = tiled.Index;
    tuning.TiledTerrainGeneration = tiled.Generation;
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc classify{};
    system.ConsumeUnifiedTiledDirtyRegion(&feature, /*updateRecordCount=*/0u, tuning, classify);
    EXPECT_FLOAT_EQ(classify.DirtyMinU, 96.0f / 256.0f);
    EXPECT_FLOAT_EQ(classify.DirtyMinV, 96.0f / 256.0f);
    EXPECT_FLOAT_EQ(classify.DirtyMaxU, 116.0f / 256.0f);
    EXPECT_FLOAT_EQ(classify.DirtyMaxV, 116.0f / 256.0f);
}

// The planet layers' drain: a sculpt dab on face +X and one on face +Y of a paged planet rewrite pages on
// both faces, the height page update publishes their rects into the service's union, one drain returns a
// rect around each dab, and a second drain returns nothing. The request is built as the driver builds it
// (RequestPlanetHeightPages needs a device). Deviceless.
TEST(CBTRegionConsume, PlanetDabsOnTwoFacesReachTheDrainAsOneRectOnEachFaceOnce)
{
    ScopedTerrainService scoped;
    TerrainService& svc = TerrainService::Get();
    TerrainRenderFeature renderFeature;
    TerrainPageFeature pageFeature;
    PlanetPagedRequest request = TerrainECS::Test::FlatPlanetRequest(true);
    const uint32 faceSamples = request.Shape.FaceSamples;
    uint64 frame = 1;
    RunPlanetPageFrames(svc, renderFeature, pageFeature, request, frame, 40);
    CBTTerrain::SphereEditRegions drained{};
    svc.ConsumePlanetLayerDirtyRegions(drained);

    constexpr float32 kAngularRadius = 50.0f / 2000.0f;
    svc.ApplyPlanetSculptDab(request.Planet, faceSamples, PlanetSculptDab({1.0f, 0.0f, 0.0f}, kAngularRadius, 5.0f));
    svc.ApplyPlanetSculptDab(request.Planet, faceSamples, PlanetSculptDab({0.0f, 1.0f, 0.0f}, kAngularRadius, 5.0f));
    const TerrainService::PlanetPageOverlay* overlay = svc.FindPlanetPageOverlay(request.Planet);
    ASSERT_NE(overlay, nullptr);
    std::array<bool, PageStreaming::kPageStoreMaxFaces> dabFaces{};
    for (uint32 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
        dabFaces[face] = overlay->Changed[face].has_value();
    ASSERT_EQ(std::count(dabFaces.begin(), dabFaces.end(), true), 2) << "each dab lies inside one face";
    RunPlanetPageFrames(svc, renderFeature, pageFeature, request, frame, 20);

    drained = {};
    ASSERT_TRUE(svc.ConsumePlanetLayerDirtyRegions(drained)) << "the rewritten pages reach the drain";
    ASSERT_EQ(drained.Count, 2u);
    for (uint32 i = 0; i < drained.Count; ++i)
    {
        const CBTTerrain::SphereFaceUVRect& rect = drained.Rects[i];
        ASSERT_LT(rect.Face, PageStreaming::kPageStoreMaxFaces);
        EXPECT_TRUE(dabFaces[rect.Face]) << "face " << rect.Face << " holds no dab";
        // The dab spans about 0.03 of the face; each rewritten level widens the rect by its tent's reach.
        EXPECT_LT(std::max(rect.MaxU - rect.MinU, rect.MaxV - rect.MinV), 0.1f) << "face " << rect.Face;
        EXPECT_GT(rect.MaxU, rect.MinU);
        EXPECT_GT(rect.MaxV, rect.MinV);
    }
    EXPECT_NE(drained.Rects[0].Face, drained.Rects[1].Face);

    CBTTerrain::SphereEditRegions again{};
    EXPECT_FALSE(svc.ConsumePlanetLayerDirtyRegions(again)) << "the drain clears what it returns";
    EXPECT_EQ(again.Count, 0u);
}

// Defensive routing: a null feature or a tiled handle with no live TiledTerrainData drains
// nothing and leaves the rect empty (no crash) — the discipline still runs so a prior
// pending rect could re-arm / commit.
TEST(CBTRegionConsume, UnifiedTiledDirtyNullFeatureOrMissingDataIsEmptyRect)
{
    ScopedTerrainService svc;
    CBTUpdateSystem system(nullptr);

    Components::Terrain tuning{};
    tuning.TiledTerrainHandle = 99u; // no such tiled data
    tuning.TiledTerrainGeneration = 1u;

    CBTTerrain::CBTClassifyDesc nullFeature{};
    system.ConsumeUnifiedTiledDirtyRegion(nullptr, 0u, tuning, nullFeature);
    EXPECT_TRUE(RectIsEmpty(nullFeature));
}

// ---------------------------------------------------------------------------
// Schedule ordering: CBTUpdate must run STRICTLY BEFORE TerrainModifiers.
// ---------------------------------------------------------------------------

// CBTUpdateSystem calls TerrainService::ResetPlanetSculpt when the active planet
// entity changes (CBTUpdateSystem.cpp, the currentPlanet block); the reset drops the
// planet page layers. TerrainModifierSystem::BakeSphereModifiers writes the modifier
// layer.
//
// The reset must come before the bake. Reversed, the reset drops the bake, and the
// modifier change gate keys on a hash that did not move, so the bake does not run
// again. Same-wave is equally wrong: waves dispatch onto the JobSystem, so reset and
// bake would interleave nondeterministically.
//
// This case covers the end-to-end order across all three modules, so it also
// catches an inversion introduced through some other module's edges. It cannot
// tell a declared edge from an accidental one — DeclaredEdgeOrdersTerrainModifiers
// below is the one that isolates the edge itself.
TEST(CBTUpdateSchedule, CBTUpdateIsScheduledBeforeTerrainModifiers)
{
    ECS::SystemScheduleBuilder builder;
    SplineECS::AddSplineSystemsToSchedule(builder);
    TerrainECS::AddTerrainSystemsToSchedule(builder, nullptr);
    CBTTerrainECS::AddCBTSystemsToSchedule(builder, nullptr);

    ECS::SystemManager manager;
    // Partial: TransformHierarchy / Camera / PhysicsWorldBootstrap belong to
    // modules this fixture does not build, so their edges are legitimately
    // unresolved here.
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);

    const auto& plan = manager.GetExecutionPlan();
    ASSERT_TRUE(plan.IsValid());

    int cbtWave = -1;
    int modifierWave = -1;
    for (std::size_t w = 0; w < plan.Waves.size(); ++w)
    {
        for (std::size_t idx : plan.Waves[w].SystemIndices)
        {
            const char* name = manager.GetSequentialSystemName(idx);
            if (!name)
                continue;
            if (std::string_view(name) == "CBTUpdate")
                cbtWave = static_cast<int>(w);
            else if (std::string_view(name) == "TerrainModifiers")
                modifierWave = static_cast<int>(w);
        }
    }

    ASSERT_GE(cbtWave, 0) << "CBTUpdate is not in the execution plan";
    ASSERT_GE(modifierWave, 0) << "TerrainModifiers is not in the execution plan";
    // Reported so a failure shows the actual waves, not just the comparison.
    EXPECT_LT(cbtWave, modifierWave)
        << "CBTUpdate wave " << cbtWave << ", TerrainModifiers wave " << modifierWave
        << " — the planet layer reset must complete before the modifier bake, and "
           "same-wave systems execute in parallel";
}

// The declared edge in isolation. Only TerrainECS and CBTTerrainECS contribute, so
// TerrainModifiers' other dependencies (TransformHierarchy, SplineExtraction) are
// deliberately unregistered and their edges inactive — the ONLY thing that can keep
// TerrainModifiers out of CBTUpdate's wave is the declared "CBTUpdate" edge itself.
// Strip that edge and both land in wave 0, where they would execute in parallel and
// race the planet layer reset against the modifier bake; this test fails there,
// where the end-to-end case above would still pass on the strength of the spline
// and transform edges.
TEST(CBTUpdateSchedule, DeclaredEdgeOrdersTerrainModifiersAfterCBTUpdate)
{
    ECS::SystemScheduleBuilder builder;
    TerrainECS::AddTerrainSystemsToSchedule(builder, nullptr);
    CBTTerrainECS::AddCBTSystemsToSchedule(builder, nullptr);

    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);

    const auto& plan = manager.GetExecutionPlan();
    ASSERT_TRUE(plan.IsValid());

    int cbtWave = -1;
    int modifierWave = -1;
    for (std::size_t w = 0; w < plan.Waves.size(); ++w)
    {
        for (std::size_t idx : plan.Waves[w].SystemIndices)
        {
            const char* name = manager.GetSequentialSystemName(idx);
            if (!name)
                continue;
            if (std::string_view(name) == "CBTUpdate")
                cbtWave = static_cast<int>(w);
            else if (std::string_view(name) == "TerrainModifiers")
                modifierWave = static_cast<int>(w);
        }
    }

    ASSERT_GE(cbtWave, 0) << "CBTUpdate is not in the execution plan";
    ASSERT_GE(modifierWave, 0) << "TerrainModifiers is not in the execution plan";
    EXPECT_LT(cbtWave, modifierWave)
        << "CBTUpdate wave " << cbtWave << ", TerrainModifiers wave " << modifierWave
        << " — TerrainModifiers does not declare a dependency on CBTUpdate, so the "
           "planet layer reset is no longer ordered before the bake";
}
