// The paving through the recipe controllers, over a headless device: what a square and a road
// build, and the state each reports.
#include "Assets/ModelAsset.h"
#include "Components/Hierarchy.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Spline/SplineNetworkOverrides.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Spline/SplinePaving.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Vector3.h"
#include "Paving/PavingApproachGround.h"
#include "Scripting/ScriptsConfig.h"
#include "SharedHeadlessDevice.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineNetwork.h"
#include "SplineECS/SplineService.h"
#include "SplineRecipes/Paving/PavingSources.h"
#include "SplineRecipes/Paving/PavingStatus.h"
#include "SplineRecipes/RecipeBuildStates.h"
#include "SplineRecipes/RecipeControllers.h"
#include "SplineRecipes/SplineSurfaceConform.h"
#include "Terrain/TerrainTypes.h"
#include "Types/Fnv1a.h"
#include "TerrainECS/TerrainService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace GameEngine::SplineRecipes
{
namespace
{

using Mathematics::Vector3;

constexpr float32 kFrameSeconds = 0.3f;
constexpr uint32 kMaxUpdates = 8;
// Vertices this close along a road's axis, in meters, lie on one ring.
constexpr float32 kRingToleranceMeters = 1.0e-3f;
// Two vertices this close on the ground plane, in meters, stand on one point.
constexpr float32 kSamePointMeters = 1.0e-2f;
// UVs this close, in tiles, read as one grid.
constexpr float32 kSameUVTiles = 5.0e-3f;
// Inside the top of any road the tests build, in meters from its centerline.
constexpr float32 kNearCenterlineMeters = 0.5f;

// A material reference no asset holds, told apart by `tag`.
Components::MaterialRef TestMaterial(uint8 tag)
{
    GUID::Data data{};
    data[0] = tag;
    return Components::MaterialRef(GUID(data));
}

// The border edges of `plan` that round a road's mouth.
std::size_t CountMouthEdges(const PavingPlan& plan)
{
    std::size_t count = 0;
    for (const SplineGeometry::PavedRegion& region : plan.Footprint.Regions)
    {
        for (const SplineGeometry::PavedBoundaryRun& run : region.Runs)
        {
            count += static_cast<std::size_t>(std::count_if(run.Edges.begin(), run.Edges.end(), [](const auto& edge) {
                return edge.Source == SplineGeometry::PavedEdgeSource::Mouth;
            }));
        }
    }
    return count;
}

class SplinePavingControllerTests : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (engine.IsInitialized())
            return;
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAsyncHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scriptsConfig);
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        ASSERT_TRUE(engine.Initialize(config));
    }

    void SetUp() override
    {
        Rendering::IDevice* device = SharedHeadlessDevice();
        if (!device)
            GTEST_SKIP() << "no Vulkan device available";
        ASSERT_TRUE(m_RenderServices.Initialize(device));
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
        m_OwnsNetwork = !SplineECS::SplineNetwork::IsInitialized();
        if (m_OwnsNetwork)
            SplineECS::SplineNetwork::Initialize();
        m_OwnsTerrain = !TerrainECS::TerrainService::IsInitialized();
        if (m_OwnsTerrain)
            TerrainECS::TerrainService::Initialize();
    }

    void TearDown() override
    {
        m_Controllers.ReleaseSceneScoped(m_World, &m_RenderServices);
        if (m_OwnsTerrain)
            TerrainECS::TerrainService::Shutdown();
        if (m_OwnsNetwork)
            SplineECS::SplineNetwork::Shutdown();
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
    }

    // A linear network member through `points`, closed when `closed`, its width channel 2 m.
    ECS::EntityHandle CreateMember(std::initializer_list<Vector3> points, bool closed,
                                   Components::SplineNetworkMemberKind kind)
    {
        auto& splines = SplineECS::SplineService::Get();
        const auto handle = splines.CreateSpline(Spline::SplineType::Linear, closed);
        auto* data = splines.GetSplineData(handle);
        for (const Vector3& point : points)
            data->AddPoint(point, 2.0f);
        splines.RebuildCache(handle);
        const ECS::EntityHandle entity = m_World.CreateEntity();
        Components::SplineComponent spline{};
        spline.SplineDataIndex = handle.Index();
        spline.SplineDataGeneration = handle.Generation();
        m_World.AddComponentImmediate(entity, spline);
        m_World.AddComponentImmediate(entity, Components::WorldTransform{});
        m_World.AddComponentImmediate(entity, Components::SplineNetworkMember{kind});
        return entity;
    }

    ECS::EntityHandle CreateSquare()
    {
        return CreateMember({Vector3(-10.0f, 0.0f, -10.0f), Vector3(10.0f, 0.0f, -10.0f), Vector3(10.0f, 0.0f, 10.0f),
                             Vector3(-10.0f, 0.0f, 10.0f)},
                            true, Components::SplineNetworkMemberKind::Area);
    }

    // Flat terrain at y = 0, 40 m square about the origin.
    void CreateFlatTerrain()
    {
        Terrain::TerrainConfig config{};
        config.HeightmapWidth = 9;
        config.HeightmapHeight = 9;
        config.WorldSizeX = 40.0f;
        config.WorldSizeZ = 40.0f;
        config.HeightScale = 1.0f;
        config.LODLevels = 1;
        config.PatchGridSize = 8;
        const auto handle = TerrainECS::TerrainService::Get().CreateTerrain(config);
        ASSERT_NE(handle.Generation, 0u);
        Components::Terrain terrain{};
        terrain.SizeX = config.WorldSizeX;
        terrain.SizeZ = config.WorldSizeZ;
        terrain.HeightScale = config.HeightScale;
        terrain.TerrainDataHandle = handle.Index;
        terrain.TerrainDataGeneration = handle.Generation;
        const ECS::EntityHandle entity = m_World.CreateEntity();
        m_World.AddComponentImmediate(entity, terrain);
        m_World.AddComponentImmediate(entity, Components::WorldTransform{});
    }

    void Update()
    {
        SplineECS::SplineNetwork::Get().Update(m_World);
        m_Controllers.Update(m_World, &m_RenderServices, nullptr, kFrameSeconds);
        m_World.ProcessCommands();
    }

    // Past the settle window and the rebuild it gates.
    void UpdateFrames()
    {
        for (uint32 i = 0; i < kMaxUpdates; ++i)
            Update();
    }

    RecipeStatus Status(ECS::EntityHandle recipe) { return m_Controllers.BuildStates().StateOf(m_World, recipe); }
    std::string_view ReasonOf(ECS::EntityHandle recipe) { return m_Controllers.BuildStates().ReasonOf(m_World, recipe); }

    uint32 ChunkCount()
    {
        uint32 count = 0;
        m_World.Query<ECS::Read<Components::RuntimeOnlyEntity>, ECS::Read<Components::MeshRenderer>>().Each(
            [&count](ECS::EntityHandle, const Components::RuntimeOnlyEntity&, const Components::MeshRenderer&) { ++count; });
        return count;
    }

    // The chunk entities under `parent`.
    uint32 ChunkCountUnder(ECS::EntityHandle parent)
    {
        uint32 count = 0;
        m_World.Query<ECS::Read<Components::RuntimeOnlyEntity>, ECS::Read<Components::MeshRenderer>,
                      ECS::Read<Components::Parent>>()
            .Each([&count, parent](ECS::EntityHandle, const Components::RuntimeOnlyEntity&, const Components::MeshRenderer&,
                      const Components::Parent& link) { count += link.Entity == parent ? 1u : 0u; });
        return count;
    }

    // One approach piece under a paving, as the tests read it.
    struct ApproachPiece
    {
        // The spline's points in world meters, from the node outward.
        std::vector<Spline::SplineControlPoint> Points;
        Components::TerrainModifierVolume Volume{};
        // It carries a flatten and a ground claim: the paving grades the arm.
        bool Grades = false;
    };

    std::vector<ApproachPiece> ApproachesUnder(ECS::EntityHandle parent)
    {
        std::vector<ApproachPiece> pieces;
        m_World.Query<ECS::Read<Components::TerrainModifierVolume>, ECS::Read<Components::Parent>>().Each(
            [this, parent, &pieces](ECS::EntityHandle entity, const Components::TerrainModifierVolume&,
                                     const Components::Parent& link)
            {
                if (link.Entity == parent)
                    AppendApproachPiece(entity, pieces);
            });
        return pieces;
    }

    // Reads the runtime-only piece `entity` into `pieces`.
    void AppendApproachPiece(ECS::EntityHandle entity, std::vector<ApproachPiece>& pieces)
    {
        const auto* spline = m_World.GetComponent<Components::SplineComponent>(entity);
        if (!spline || !m_World.GetComponent<Components::RuntimeOnlyEntity>(entity))
            return;
        const auto* volume = m_World.GetComponent<Components::TerrainModifierVolume>(entity);
        EXPECT_EQ(volume->Shape, Components::TerrainVolumeShape::SplinePath);
        const auto* grass = m_World.GetComponent<Components::TerrainGrassEffect>(entity);
        EXPECT_TRUE(grass && grass->DensityScale == 0.0f) << "a piece clears the grass";
        EXPECT_NE(m_World.GetComponent<Components::TerrainPaintLayerEffect>(entity), nullptr) << "and paints";
        const auto* data = SplineECS::SplineService::Get().GetSplineData(
            SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
        ApproachPiece& piece = pieces.emplace_back();
        piece.Points = data ? data->Points : std::vector<Spline::SplineControlPoint>{};
        piece.Volume = *volume;
        piece.Grades = m_World.GetComponent<Components::TerrainFlattenEffect>(entity) != nullptr
                       && m_World.GetComponent<Components::TerrainGroundClaimEffect>(entity) != nullptr;
    }

    // One frame of `ground` alone: its update, then the world's queued commands.
    void UpdateApproachGround(PavingApproachGround& ground, std::span<const PavingApproach> approaches,
                              uint64 approachesVersion)
    {
        ground.Update(m_World, approaches, approachesVersion);
        m_World.ProcessCommands();
    }

    // The bound center height of the one chunk under `parent`.
    float32 ChunkCenterYUnder(ECS::EntityHandle parent)
    {
        float32 y = 0.0f;
        m_World.Query<ECS::Read<Components::LocalBounds>, ECS::Read<Components::Parent>>().Each(
            [&](ECS::EntityHandle, const Components::LocalBounds& bounds, const Components::Parent& link)
            {
                if (link.Entity == parent)
                    y = bounds.Box.center.y;
            });
        return y;
    }

    // The meshes of the chunks under `parent` whose name holds `part`, in name order.
    std::vector<const Mesh*> ChunkMeshesUnder(ECS::EntityHandle parent, std::string_view part)
    {
        std::vector<const Mesh*> meshes;
        const Rendering::MeshGPURegistry& registry = m_RenderServices.GetMeshGPURegistry();
        m_World.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::Parent>>().Each(
            [&](ECS::EntityHandle, const Components::MeshRenderer& renderer, const Components::Parent& link)
            {
                const auto* entry = link.Entity == parent
                                        ? registry.Find(Rendering::MeshGPUHandle(renderer.meshGpuHandleId))
                                        : nullptr;
                if (entry && entry->cpuMesh && entry->cpuMesh->Name.find(part) != std::string::npos)
                    meshes.push_back(entry->cpuMesh.get());
            });
        std::sort(meshes.begin(), meshes.end(), [](const Mesh* a, const Mesh* b) { return a->Name < b->Name; });
        return meshes;
    }

    // The top vertices (y at or above 0) of `road`'s strip on its ring nearest the node at the origin,
    // measured along world axis `axis` (0 = x, 2 = z). The paving's lid and border under it are skipped.
    std::vector<Vertex> StripTopVerticesNearestNode(ECS::EntityHandle road, uint32 axis)
    {
        float32 nearest = std::numeric_limits<float32>::max();
        std::vector<Vertex> ring;
        for (const Mesh* mesh : ChunkMeshesUnder(road, ""))
        {
            if (mesh->Name.find("_Lid") != std::string::npos || mesh->Name.find("_Border") != std::string::npos)
                continue;
            for (const Vertex& vertex : mesh->Vertices)
            {
                if (vertex.Position[1] < 0.0f)
                    continue;
                const float32 along = std::abs(vertex.Position[axis]);
                if (along < nearest - kRingToleranceMeters)
                {
                    nearest = along;
                    ring.clear();
                }
                if (std::abs(along - nearest) <= kRingToleranceMeters)
                    ring.push_back(vertex);
            }
        }
        return ring;
    }

    std::vector<Vector3> StripTopNearestNode(ECS::EntityHandle road, uint32 axis)
    {
        std::vector<Vector3> ring;
        for (const Vertex& vertex : StripTopVerticesNearestNode(road, axis))
            ring.emplace_back(vertex.Position[0], vertex.Position[1], vertex.Position[2]);
        return ring;
    }

    // A digest of every vertex and index of the chunks under `parent`, in name order.
    uint64 ChunkDigestUnder(ECS::EntityHandle parent)
    {
        uint64 digest = Hashing::kFnv1a64OffsetBasis;
        for (const Mesh* mesh : ChunkMeshesUnder(parent, ""))
        {
            digest = Hashing::Fnv1a64(mesh->Vertices.data(), mesh->Vertices.size() * sizeof(Vertex), digest);
            digest = Hashing::Fnv1a64(mesh->Indices.data(), mesh->Indices.size() * sizeof(uint32), digest);
        }
        return digest;
    }

    ECS::World m_World;
    Engine::Renderer::RenderServices m_RenderServices;
    RecipeControllers m_Controllers;
    bool m_OwnsSplines = false;
    bool m_OwnsNetwork = false;
    bool m_OwnsTerrain = false;
};

TEST_F(SplinePavingControllerTests, ASquareNoRoadEntersIsPavedWithALidAndABorder)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});

    UpdateFrames();

    EXPECT_EQ(Status(square).State, BuildState::Built);
    EXPECT_EQ(ChunkCount(), 2u);
}

TEST_F(SplinePavingControllerTests, TheLidSitsTheLiftAboveTheGroundUnderIt)
{
    CreateFlatTerrain();
    // The square's entity stands 5 m above the terrain: a ray that misses would put the lid there.
    const ECS::EntityHandle square = CreateSquare();
    constexpr float32 kSquareY = 5.0f;
    m_World.GetComponentForWrite<Components::WorldTransform>(square)->matrix[13] = kSquareY;
    Components::SplinePaving recipe{};
    recipe.BorderWidth = 0.0f;
    recipe.ConformTarget = Components::SplineConformTarget::TerrainOnly;
    m_World.AddComponentImmediate(square, recipe);

    UpdateFrames();

    ASSERT_EQ(Status(square).State, BuildState::Built);
    uint32 lids = 0;
    m_World.Query<ECS::Read<Components::LocalBounds>, ECS::Read<Components::Parent>>().Each(
        [&](ECS::EntityHandle, const Components::LocalBounds& bounds, const Components::Parent& link)
        {
            if (link.Entity != square || bounds.Box.halfExtents.x <= 0.0f)
                return;
            ++lids;
            EXPECT_NEAR(bounds.Box.center.y + kSquareY, kGroundSurfaceLiftMeters, 1.0e-4f);
            EXPECT_NEAR(bounds.Box.halfExtents.y, 0.0f, 1.0e-4f);
        });
    EXPECT_EQ(lids, 1u);
}

// A road's strip that conforms sits the ground lift above the ground; a plain extrude beside it
// sits on the ground, so a road meets the paving at one height.
TEST_F(SplinePavingControllerTests, ARoadsStripSitsTheLiftAboveTheGroundAndAPlainExtrudeDoesNot)
{
    CreateFlatTerrain();
    const ECS::EntityHandle road = CreateMember({Vector3(-15.0f, 0.0f, -5.0f), Vector3(15.0f, 0.0f, -5.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    const ECS::EntityHandle plain = CreateMember({Vector3(-15.0f, 0.0f, 5.0f), Vector3(15.0f, 0.0f, 5.0f)}, false,
                                                 Components::SplineNetworkMemberKind::Road);
    m_World.RemoveComponentImmediate<Components::SplineNetworkMember>(plain);
    m_World.AddComponentImmediate(road, Components::SplineExtrude{});
    m_World.AddComponentImmediate(plain, Components::SplineExtrude{});

    UpdateFrames();

    ASSERT_EQ(Status(road).State, BuildState::Built);
    ASSERT_EQ(Status(plain).State, BuildState::Built);
    EXPECT_NEAR(ChunkCenterYUnder(road) - ChunkCenterYUnder(plain), kGroundSurfaceLiftMeters, 1.0e-4f);
}

TEST_F(SplinePavingControllerTests, AStripThatBecomesARoadAfterItsBuildRisesByTheLift)
{
    CreateFlatTerrain();
    const ECS::EntityHandle strip = CreateMember({Vector3(-15.0f, 0.0f, 5.0f), Vector3(15.0f, 0.0f, 5.0f)}, false,
                                                 Components::SplineNetworkMemberKind::Road);
    m_World.RemoveComponentImmediate<Components::SplineNetworkMember>(strip);
    m_World.AddComponentImmediate(strip, Components::SplineExtrude{});
    UpdateFrames();
    ASSERT_EQ(Status(strip).State, BuildState::Built);
    const float32 plainY = ChunkCenterYUnder(strip);

    m_World.AddComponentImmediate(strip, Components::SplineNetworkMember{Components::SplineNetworkMemberKind::Road});
    UpdateFrames();

    EXPECT_NEAR(ChunkCenterYUnder(strip) - plainY, kGroundSurfaceLiftMeters, 1.0e-4f);
}

TEST_F(SplinePavingControllerTests, ASquareARoadEntersIsOneRegionTheSquareBuilds)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle road = CreateMember({Vector3(-40.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    m_World.AddComponentImmediate(road, Components::SplinePaving{});

    UpdateFrames();

    EXPECT_EQ(Status(square).State, BuildState::Built);
    EXPECT_TRUE(ReasonOf(square).empty());
    // The road's own paving paves no crossing, and says so.
    EXPECT_EQ(Status(road).State, BuildState::Failed);
    EXPECT_EQ(ReasonOf(road), kNothingToPaveReason);
    EXPECT_EQ(ChunkCount(), 2u);
}

TEST_F(SplinePavingControllerTests, AFlaredMouthRoundsTheRoadsEntryAndAStraightOneMeetsItAtACorner)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    CreateMember({Vector3(-40.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 0.0f)}, false,
                 Components::SplineNetworkMemberKind::Road);
    UpdateFrames();

    EXPECT_GT(CountMouthEdges(PlanPavings(GatherPavings(m_World, PavingGatherDepth::Full).Sources)), 0u);
    m_World.GetComponentForWrite<Components::SplinePaving>(square)->Mouth = Components::SplinePavingMouth::Straight;
    EXPECT_EQ(CountMouthEdges(PlanPavings(GatherPavings(m_World, PavingGatherDepth::Full).Sources)), 0u);
}

TEST_F(SplinePavingControllerTests, AnObservingGatherReadsWhatTheGateComparesAndSamplesNoCurve)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    CreateMember({Vector3(-40.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 0.0f)}, false,
                 Components::SplineNetworkMemberKind::Road);
    UpdateFrames();

    const PavingWorld observed = GatherPavings(m_World, PavingGatherDepth::Observe);
    const PavingWorld full = GatherPavings(m_World, PavingGatherDepth::Full);

    ASSERT_EQ(full.Sources.size(), 1u);
    ASSERT_EQ(observed.Sources.size(), 1u);
    EXPECT_FALSE(full.Sources[0].Arms.empty());
    EXPECT_FALSE(full.Sources[0].Outline.empty());
    EXPECT_EQ(full.Splines.size(), 2u);
    EXPECT_EQ(observed.Splines, full.Splines);
    EXPECT_EQ(observed.Junctions, full.Junctions);
    EXPECT_TRUE(observed.Sources[0].Arms.empty());
    EXPECT_TRUE(observed.Sources[0].Outline.empty());
}

TEST_F(SplinePavingControllerTests, ARoadsArmIsAsWideAsTheTopOfItsStrip)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle road = CreateMember({Vector3(-40.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    // The default Bevel: Width 2 with a 0.15 m inset, so its top ends at 0.85 of the 2 m width channel.
    m_World.AddComponentImmediate(road, Components::SplineExtrude{});
    UpdateFrames();

    const PavingWorld bevel = GatherPavings(m_World, PavingGatherDepth::Full);
    ASSERT_EQ(bevel.Sources.size(), 1u);
    ASSERT_FALSE(bevel.Sources[0].Arms.empty());
    for (const SplineGeometry::PavedArmStation& station : bevel.Sources[0].Arms[0].Stations)
    {
        EXPECT_NEAR(station.HalfWidthLeft, 1.7f, 1.0e-4f);
        EXPECT_NEAR(station.HalfWidthRight, 1.7f, 1.0e-4f);
    }

    // A flat profile has no inset: its top is the whole width channel.
    auto* strip = m_World.GetComponentForWrite<Components::SplineExtrude>(road);
    strip->EdgeDrop = 0.0f;
    strip->EdgeInset = 0.0f;
    const PavingWorld flat = GatherPavings(m_World, PavingGatherDepth::Full);
    ASSERT_FALSE(flat.Sources[0].Arms.empty());
    for (const SplineGeometry::PavedArmStation& station : flat.Sources[0].Arms[0].Stations)
        EXPECT_NEAR(station.HalfWidthLeft, 2.0f, 1.0e-4f);
    EXPECT_NE(bevel.Splines, flat.Splines) << "the gate does not see the strip's profile";
}

TEST_F(SplinePavingControllerTests, ARoadsStripStopsUnderTheRegionAndGoesOnPastIt)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    // 40 m of road: one chunk whole, one chunk on each side of the square once the region takes
    // the crossing over.
    const ECS::EntityHandle road = CreateMember({Vector3(-20.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude strip{};
    strip.ConformMode = Components::SplinePlacementConform::None;
    m_World.AddComponentImmediate(road, strip);

    UpdateFrames();

    EXPECT_EQ(Status(road).State, BuildState::Built);
    EXPECT_EQ(ChunkCountUnder(road), 2u);
    EXPECT_EQ(ChunkCountUnder(square), 2u);

    m_World.RemoveComponentImmediate<Components::SplinePaving>(square);
    UpdateFrames();

    EXPECT_EQ(ChunkCountUnder(road), 1u);
}

// Road B ends on road A, and both carry a crown. Where each strip stops at the junction, its top
// lies at the lid's height across its whole width: the crown runs out before the paving.
TEST_F(SplinePavingControllerTests, ACrownedRoadMeetsTheJunctionLidFlushAcrossItsWidth)
{
    CreateFlatTerrain();
    const ECS::EntityHandle roadA = CreateMember({Vector3(-18.0f, 0.0f, 0.0f), Vector3(18.0f, 0.0f, 0.0f)}, false,
                                                 Components::SplineNetworkMemberKind::Road);
    const ECS::EntityHandle roadB = CreateMember({Vector3(0.0f, 0.0f, 18.0f), Vector3(0.0f, 0.0f, 0.0f)}, false,
                                                 Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude crown{};
    crown.Profile = Components::SplineExtrudeProfile::Crown;
    m_World.AddComponentImmediate(roadA, crown);
    m_World.AddComponentImmediate(roadB, crown);
    m_World.AddComponentImmediate(roadA, Components::SplinePaving{});

    UpdateFrames();

    ASSERT_EQ(Status(roadA).State, BuildState::Built);
    ASSERT_EQ(Status(roadB).State, BuildState::Built);
    const std::vector<const Mesh*> lids = ChunkMeshesUnder(roadA, "_Lid");
    ASSERT_EQ(lids.size(), 1u);
    float32 lidY = -1.0e9f;
    for (const Vertex& vertex : lids[0]->Vertices)
        lidY = std::max(lidY, vertex.Position[1]);

    for (const auto& [road, axis] : {std::pair(roadA, 0u), std::pair(roadB, 2u)})
    {
        const std::vector<Vector3> ring = StripTopNearestNode(road, axis);
        ASSERT_GE(ring.size(), 3u) << "the crown's edges and its apex";
        for (const Vector3& point : ring)
            EXPECT_NEAR(point.y, lidY, 2.0e-3f) << "road " << road.id << " at (" << point.x << ", " << point.z << ")";
    }
}

// Road B ends on road A, which paves the junction. Every strip cap vertex has a lid vertex on it with
// the strip's UV, and within half a meter of road A's centerline the lid's U is road A's arc length.
TEST_F(SplinePavingControllerTests, TheJunctionLidContinuesEachRoadsGridFromItsStrip)
{
    CreateFlatTerrain();
    const ECS::EntityHandle roadA = CreateMember({Vector3(-18.0f, 0.0f, 0.0f), Vector3(18.0f, 0.0f, 0.0f)}, false,
                                                 Components::SplineNetworkMemberKind::Road);
    const ECS::EntityHandle roadB = CreateMember({Vector3(0.0f, 0.0f, 18.0f), Vector3(0.0f, 0.0f, 0.0f)}, false,
                                                 Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude crown{};
    crown.Profile = Components::SplineExtrudeProfile::Crown;
    crown.TilesPerMetreU = 0.5f;
    crown.TilesPerMetreV = 0.5f;
    m_World.AddComponentImmediate(roadA, crown);
    m_World.AddComponentImmediate(roadB, crown);
    m_World.AddComponentImmediate(roadA, Components::SplinePaving{});

    UpdateFrames();

    ASSERT_EQ(Status(roadA).State, BuildState::Built);
    ASSERT_EQ(Status(roadB).State, BuildState::Built);
    const std::vector<const Mesh*> lids = ChunkMeshesUnder(roadA, "_Lid");
    ASSERT_EQ(lids.size(), 1u);
    const std::vector<Vertex>& lid = lids[0]->Vertices;

    // Road A rises from x = -18, road B from z = 18.
    for (const auto& [road, axis, startAt, rises] :
         {std::tuple(roadA, 0u, -18.0f, 1.0f), std::tuple(roadB, 2u, 18.0f, -1.0f)})
    {
        const std::vector<Vertex> cap = StripTopVerticesNearestNode(road, axis);
        ASSERT_GE(cap.size(), 3u) << "the crown's edges and its apex";
        for (const Vertex& strip : cap)
        {
            const float32 arcLength = (strip.Position[axis] - startAt) * rises;
            EXPECT_NEAR(strip.TexCoords[0], arcLength * crown.TilesPerMetreU, kSameUVTiles)
                << "road " << road.id << ": the strip's U is its arc length up to the cap";
            float32 closest = std::numeric_limits<float32>::max();
            for (const Vertex& vertex : lid)
            {
                if (std::hypot(vertex.Position[0] - strip.Position[0], vertex.Position[2] - strip.Position[2]) >
                    kSamePointMeters)
                    continue;
                closest = std::min(closest, std::hypot(vertex.TexCoords[0] - strip.TexCoords[0],
                                                       vertex.TexCoords[1] - strip.TexCoords[1]));
            }
            EXPECT_LE(closest, kSameUVTiles) << "road " << road.id << " cap vertex at (" << strip.Position[0] << ", "
                                             << strip.Position[2] << ")";
        }
    }

    uint32 nearCenterline = 0;
    for (const Vertex& vertex : lid)
    {
        if (std::abs(vertex.Position[2]) > kNearCenterlineMeters)
            continue;
        ++nearCenterline;
        EXPECT_NEAR(vertex.TexCoords[0], (vertex.Position[0] + 18.0f) * crown.TilesPerMetreU, kSameUVTiles)
            << "lid vertex at x " << vertex.Position[0];
    }
    EXPECT_GT(nearCenterline, 0u);
}

// A road ends inside a square, which paves the mouth. The road's grid fills the mouth's arcs and is cut
// on the square's rim; inside the rim the lid keeps the square's own grid.
TEST_F(SplinePavingControllerTests, TheRoadsGridFillsTheSquaresMouthAndStopsOnItsRim)
{
    constexpr float32 kRimX = -10.0f;
    // Past the top of the road's strip (4/3 m from its centerline) and inside the mouth's arcs.
    constexpr float32 kBesideTopMeters = 1.5f;
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle road = CreateMember({Vector3(-30.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude crown{};
    crown.Profile = Components::SplineExtrudeProfile::Crown;
    crown.ConformMode = Components::SplinePlacementConform::None;
    crown.TilesPerMetreU = 0.5f;
    crown.TilesPerMetreV = 0.5f;
    m_World.AddComponentImmediate(road, crown);

    UpdateFrames();

    ASSERT_EQ(Status(square).State, BuildState::Built);
    const std::vector<const Mesh*> lids = ChunkMeshesUnder(square, "_Lid");
    ASSERT_EQ(lids.size(), 1u);
    uint32 onRim = 0;
    uint32 inMouthArcs = 0;
    for (const Vertex& vertex : lids[0]->Vertices)
    {
        // The road rises from x = -30; the square's own U is x.
        const float32 x = vertex.Position[0];
        if (std::abs(vertex.TexCoords[0] - (x + 30.0f) * crown.TilesPerMetreU) > kSameUVTiles)
            continue;
        EXPECT_LE(x, kRimX + kSamePointMeters) << "the road's grid at (" << x << ", " << vertex.Position[2] << ")";
        onRim += std::abs(x - kRimX) <= kSamePointMeters ? 1u : 0u;
        inMouthArcs += x < kRimX - kSamePointMeters && std::abs(vertex.Position[2]) > kBesideTopMeters ? 1u : 0u;
    }
    EXPECT_GT(onRim, 0u) << "the cut lies on the rim";
    EXPECT_GT(inMouthArcs, 0u) << "the road's grid fills the arcs beside its top";
}

// Two roads of equal width cross. Where their tops overlap, the lid takes the longer road's grid, in
// either creation order.
TEST_F(SplinePavingControllerTests, TheLongerOfTwoEqualRoadsGivesTheLidItsGridInEitherCreationOrder)
{
    constexpr float32 kSecondCrossingZ = 100.0f;
    const ECS::EntityHandle longerFirst = CreateMember({Vector3(-30.0f, 0.0f, 0.0f), Vector3(30.0f, 0.0f, 0.0f)},
                                                       false, Components::SplineNetworkMemberKind::Road);
    const ECS::EntityHandle shorterAfter = CreateMember({Vector3(0.0f, 0.0f, -20.0f), Vector3(0.0f, 0.0f, 20.0f)},
                                                        false, Components::SplineNetworkMemberKind::Road);
    const ECS::EntityHandle shorterFirst =
        CreateMember({Vector3(0.0f, 0.0f, kSecondCrossingZ - 20.0f), Vector3(0.0f, 0.0f, kSecondCrossingZ + 20.0f)},
                     false, Components::SplineNetworkMemberKind::Road);
    const ECS::EntityHandle longerAfter =
        CreateMember({Vector3(-30.0f, 0.0f, kSecondCrossingZ), Vector3(30.0f, 0.0f, kSecondCrossingZ)}, false,
                     Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude crown{};
    crown.Profile = Components::SplineExtrudeProfile::Crown;
    crown.ConformMode = Components::SplinePlacementConform::None;
    crown.TilesPerMetreU = 0.5f;
    crown.TilesPerMetreV = 0.5f;
    for (const ECS::EntityHandle road : {longerFirst, shorterAfter, shorterFirst, longerAfter})
        m_World.AddComponentImmediate(road, crown);
    m_World.AddComponentImmediate(longerFirst, Components::SplinePaving{});
    m_World.AddComponentImmediate(longerAfter, Components::SplinePaving{});

    UpdateFrames();

    // Each longer road rises from x = -30 along its crossing's z.
    for (const auto& [longer, z] : {std::tuple(longerFirst, 0.0f), std::tuple(longerAfter, kSecondCrossingZ)})
    {
        ASSERT_EQ(Status(longer).State, BuildState::Built);
        const std::vector<const Mesh*> lids = ChunkMeshesUnder(longer, "_Lid");
        ASSERT_EQ(lids.size(), 1u);
        uint32 nearCenterline = 0;
        for (const Vertex& vertex : lids[0]->Vertices)
        {
            if (std::abs(vertex.Position[2] - z) > kNearCenterlineMeters)
                continue;
            ++nearCenterline;
            EXPECT_NEAR(vertex.TexCoords[0], (vertex.Position[0] + 30.0f) * crown.TilesPerMetreU, kSameUVTiles)
                << "road " << longer.id << ": lid vertex at x " << vertex.Position[0];
        }
        EXPECT_GT(nearCenterline, 0u) << "road " << longer.id;
    }
}

// The square owns its approaches: a road that grades gets the square's grass clear and paint from the
// rim to the approach stop, and a road that owns its ground keeps its own.
TEST_F(SplinePavingControllerTests, ASquareOnOwnsClearsTheApproachesOfARoadThatGradesAndNotOfOneThatOwns)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle road = CreateMember({Vector3(-20.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude strip{};
    strip.ConformMode = Components::SplinePlacementConform::None;
    ASSERT_EQ(Components::ResolveGroundAuthority(strip), Components::SplineGroundAuthority::Grades);
    m_World.AddComponentImmediate(road, strip);

    UpdateFrames();

    const std::vector<ApproachPiece> approaches = ApproachesUnder(square);
    ASSERT_EQ(approaches.size(), 2u);
    for (const ApproachPiece& piece : approaches)
    {
        EXPECT_FALSE(piece.Grades) << "a road that grades keeps its own grade on its arm";
        const std::vector<Spline::SplineControlPoint>& points = piece.Points;
        ASSERT_GE(points.size(), 2u);
        EXPECT_NEAR(std::abs(points.front().Position.x), 10.0f, 1e-3f) << "an approach starts at the node on the rim";
        EXPECT_GT(std::abs(points.back().Position.x), 11.0f) << "and reaches out past the rim to the stop";
        EXPECT_LT(std::abs(points.back().Position.x), 20.0f);
        const Components::SplinePaving paving{};
        EXPECT_GE(points.front().Radius, 0.5f * strip.Width + paving.BorderWidth + paving.MouthRadius)
            << "and covers the curb and the mouth's arcs";
    }

    m_World.GetComponentForWrite<Components::SplineExtrude>(road)->GroundAuthority =
        Components::SplineGroundAuthority::Owns;
    UpdateFrames();
    EXPECT_TRUE(ApproachesUnder(square).empty()) << "a road that owns its ground clears its own arm";

    m_World.GetComponentForWrite<Components::SplineExtrude>(road)->GroundAuthority =
        Components::SplineGroundAuthority::Auto;
    UpdateFrames();
    EXPECT_EQ(ApproachesUnder(square).size(), 2u);
}

TEST_F(SplinePavingControllerTests, ASquareOnNoneLaysNoApproachPieces)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle road = CreateMember({Vector3(-20.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude strip{};
    strip.ConformMode = Components::SplinePlacementConform::None;
    m_World.AddComponentImmediate(road, strip);
    UpdateFrames();
    ASSERT_EQ(ApproachesUnder(square).size(), 2u);

    m_World.GetComponentForWrite<Components::SplinePaving>(square)->GroundAuthority =
        Components::SplineGroundAuthority::None;
    UpdateFrames();

    EXPECT_TRUE(ApproachesUnder(square).empty());
    EXPECT_EQ(ChunkCountUnder(square), 2u) << "the paving itself still builds";
}

// A ring road with a corner inside the square runs through the square and out along each leg: each
// arm's piece starts where the arm leaves the rim and runs to the arm's stop.
TEST_F(SplinePavingControllerTests, AnArmThatRunsThroughTheSquareGetsAPieceFromTheRimToItsStop)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle ring = CreateMember({Vector3(6.0f, 0.0f, 6.0f), Vector3(16.0f, 0.0f, 6.0f),
                                                 Vector3(16.0f, 0.0f, 16.0f), Vector3(6.0f, 0.0f, 16.0f)},
                                                true, Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude strip{};
    strip.ConformMode = Components::SplinePlacementConform::None;
    m_World.AddComponentImmediate(ring, strip);

    UpdateFrames();

    const std::vector<ApproachPiece> approaches = ApproachesUnder(square);
    ASSERT_EQ(approaches.size(), 2u);
    for (const ApproachPiece& piece : approaches)
    {
        ASSERT_GE(piece.Points.size(), 2u);
        const Vector3 start = piece.Points.front().Position;
        const Vector3 end = piece.Points.back().Position;
        // The leg along z = 6 leaves the rim at x = 10, and the leg along x = 6 at z = 10.
        const bool alongX = std::abs(start.z - 6.0f) < 1e-3f;
        EXPECT_NEAR(alongX ? start.x : start.z, 10.0f, 1e-3f) << "the piece starts on the rim";
        EXPECT_GT(alongX ? end.x : end.z, 13.0f) << "and reaches the stop past the mouth";
        EXPECT_NEAR(alongX ? end.z : end.x, 6.0f, 1e-3f);
    }
}

// A road that ends past the rim, before its stop, gets a piece from the rim to its end.
TEST_F(SplinePavingControllerTests, ARoadThatEndsPastTheRimGetsAPieceToItsEnd)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle road = CreateMember({Vector3(-20.0f, 0.0f, 0.0f), Vector3(12.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude strip{};
    strip.ConformMode = Components::SplinePlacementConform::None;
    m_World.AddComponentImmediate(road, strip);

    UpdateFrames();

    const std::vector<ApproachPiece> approaches = ApproachesUnder(square);
    ASSERT_EQ(approaches.size(), 2u) << "the arm that ends the road has a piece too";
    const bool eastFirst = approaches[0].Points.back().Position.x > 0.0f;
    EXPECT_NEAR(approaches[eastFirst ? 0 : 1].Points.back().Position.x, 12.0f, 1e-3f);
}

// Under a road that does not grade its own ground, the square grades the arm: the piece carries the
// square's flatten and claim.
TEST_F(SplinePavingControllerTests, ASquareOnOwnsGradesTheApproachesOfARoadOnNone)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle road = CreateMember({Vector3(-20.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude strip{};
    strip.ConformMode = Components::SplinePlacementConform::None;
    strip.GroundAuthority = Components::SplineGroundAuthority::None;
    m_World.AddComponentImmediate(road, strip);

    UpdateFrames();

    const std::vector<ApproachPiece> approaches = ApproachesUnder(square);
    ASSERT_EQ(approaches.size(), 2u);
    for (const ApproachPiece& piece : approaches)
        EXPECT_TRUE(piece.Grades);
}

// The pieces take the square's volume fade, weight and priority, and follow an edit of them.
TEST_F(SplinePavingControllerTests, AnApproachPieceTakesTheSquaresFalloffWeightAndPriority)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle road = CreateMember({Vector3(-20.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude strip{};
    strip.ConformMode = Components::SplinePlacementConform::None;
    m_World.AddComponentImmediate(road, strip);
    UpdateFrames();

    auto* volume = m_World.GetComponentForWrite<Components::TerrainModifierVolume>(square);
    ASSERT_NE(volume, nullptr);
    volume->Falloff = 1.0f;
    volume->Weight = 0.5f;
    volume->Priority = 12.0f;
    UpdateFrames();

    const std::vector<ApproachPiece> approaches = ApproachesUnder(square);
    ASSERT_EQ(approaches.size(), 2u);
    for (const ApproachPiece& piece : approaches)
    {
        EXPECT_EQ(piece.Volume.Falloff, 1.0f);
        EXPECT_EQ(piece.Volume.Weight, 0.5f);
        EXPECT_EQ(piece.Volume.Priority, 12.0f);
    }
}

// The pieces read the world only when the approaches or an effect that they copy changes.
TEST_F(SplinePavingControllerTests, ApproachPiecesReadTheWorldOnlyWhenTheirInputsChange)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    UpdateFrames();
    ASSERT_NE(m_World.GetComponent<Components::TerrainGrassEffect>(square), nullptr);
    const PavingApproach approach{.Owner = square,
                                  .Points = {Vector3(10.0f, 0.0f, 0.0f), Vector3(14.0f, 0.0f, 0.0f)},
                                  .HalfWidths = {3.0f, 3.0f}};
    const std::span<const PavingApproach> approaches(&approach, 1);
    PavingApproachGround ground;
    // The second frame sees the pieces that the first one laid.
    UpdateApproachGround(ground, approaches, 1);
    UpdateApproachGround(ground, approaches, 1);
    ASSERT_EQ(ApproachesUnder(square).size(), 1u);
    const uint32 reads = ground.ReadsForTests();

    for (uint32 i = 0; i < kMaxUpdates; ++i)
        UpdateApproachGround(ground, approaches, 1);
    EXPECT_EQ(ground.ReadsForTests(), reads) << "an idle frame reads nothing";

    m_World.GetComponentForWrite<Components::TerrainGrassEffect>(square)->HeightScale = 0.5f;
    UpdateApproachGround(ground, approaches, 1);
    EXPECT_GT(ground.ReadsForTests(), reads) << "an edit of the square's grass";
    UpdateApproachGround(ground, approaches, 1);
    UpdateApproachGround(ground, approaches, 1);
    const uint32 settled = ground.ReadsForTests();
    UpdateApproachGround(ground, approaches, 2);
    EXPECT_GT(ground.ReadsForTests(), settled) << "a new set of approaches";

    UpdateApproachGround(ground, approaches, 2);
    ECS::EntityHandle laid{};
    m_World.Query<ECS::Read<Components::TerrainModifierVolume>, ECS::Read<Components::Parent>>().Each(
        [&laid, square](ECS::EntityHandle entity, const Components::TerrainModifierVolume&, const Components::Parent& link)
        { laid = link.Entity == square ? entity : laid; });
    m_World.DestroyEntityImmediate(laid);
    UpdateApproachGround(ground, approaches, 2);
    EXPECT_EQ(ApproachesUnder(square).size(), 1u) << "a piece removed from the world is laid again";
    ground.Shutdown(m_World);
}

// A road with a verge builds it beside its strip, chunk for chunk, and the square's curb leaves out its
// end faces where the road leaves the square: two per mouth, four points each.
TEST_F(SplinePavingControllerTests, ARoadsVergeIsBuiltBesideItsStripAndTheCurbMeetsItWithNoEndFace)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle road = CreateMember({Vector3(-20.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude strip{};
    strip.ConformMode = Components::SplinePlacementConform::None;
    m_World.AddComponentImmediate(road, strip);
    UpdateFrames();
    const std::vector<const Mesh*> cappedCurb = ChunkMeshesUnder(square, "_Border");
    ASSERT_EQ(cappedCurb.size(), 1u);
    const size_t cappedVertices = cappedCurb[0]->Vertices.size();
    EXPECT_TRUE(ChunkMeshesUnder(road, "_Verge_").empty());

    m_World.GetComponentForWrite<Components::SplineExtrude>(road)->Verge = true;
    UpdateFrames();

    EXPECT_EQ(Status(road).State, BuildState::Built);
    EXPECT_EQ(ChunkMeshesUnder(road, "_Verge_").size(), 2u);
    EXPECT_EQ(ChunkCountUnder(road), 4u);
    const std::vector<const Mesh*> curb = ChunkMeshesUnder(square, "_Border");
    ASSERT_EQ(curb.size(), 1u);
    EXPECT_EQ(cappedVertices - curb[0]->Vertices.size(), 16u);

    m_World.GetComponentForWrite<Components::SplineExtrude>(road)->Verge = false;
    UpdateFrames();
    EXPECT_EQ(ChunkCountUnder(road), 2u);
}

// A verge switched on beside two pavings takes the curb material of the first paving along the road;
// a verge whose Material is set keeps it.
TEST_F(SplinePavingControllerTests, AVergeTakesTheCurbMaterialOfTheFirstPavingAlongTheRoad)
{
    const ECS::EntityHandle near = CreateSquare();
    // The squares stand far apart, so each paving builds its own region and curb.
    const ECS::EntityHandle far = CreateMember({Vector3(100.0f, 0.0f, -10.0f), Vector3(120.0f, 0.0f, -10.0f),
                                                Vector3(120.0f, 0.0f, 10.0f), Vector3(100.0f, 0.0f, 10.0f)},
                                               true, Components::SplineNetworkMemberKind::Area);
    Components::SplinePaving nearPaving{};
    nearPaving.BorderMaterial = TestMaterial(1);
    m_World.AddComponentImmediate(near, nearPaving);
    Components::SplinePaving farPaving{};
    farPaving.BorderMaterial = TestMaterial(2);
    m_World.AddComponentImmediate(far, farPaving);
    const ECS::EntityHandle road = CreateMember({Vector3(130.0f, 0.0f, 0.0f), Vector3(-20.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude strip{};
    strip.ConformMode = Components::SplinePlacementConform::None;
    m_World.AddComponentImmediate(road, strip);
    UpdateFrames();

    Components::SplineExtrude empty{};
    m_Controllers.PrefillVergeMaterial(m_World, road, empty);
    EXPECT_EQ(empty.VergeMaterial, TestMaterial(2)) << "the road reaches the far square first";
    Components::SplineExtrude set{};
    set.VergeMaterial = TestMaterial(3);
    m_Controllers.PrefillVergeMaterial(m_World, road, set);
    EXPECT_EQ(set.VergeMaterial, TestMaterial(3));
}

// With the verge off, a road and the square it enters build these meshes, to the byte: the curb is
// capped at the throat and the road has no verge.
TEST_F(SplinePavingControllerTests, ARoadWithNoVergeBuildsACappedCurbAndNoVerge)
{
    // The float math of the square's meshes rounds differently on arm64, so each architecture pins
    // its own bytes. The road's meshes are the same bytes on both.
#if defined(__aarch64__) || defined(_M_ARM64)
    constexpr uint64 kSquareDigest = 0xb094b6de04451142ull;
#else
    constexpr uint64 kSquareDigest = 0xfce75b03317ec4ffull;
#endif
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle road = CreateMember({Vector3(-20.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude strip{};
    strip.ConformMode = Components::SplinePlacementConform::None;
    m_World.AddComponentImmediate(road, strip);
    UpdateFrames();

    EXPECT_EQ(ChunkDigestUnder(square), kSquareDigest) << std::hex << ChunkDigestUnder(square);
    EXPECT_EQ(ChunkDigestUnder(road), 0x973fd0a1fc5bb035ull) << std::hex << ChunkDigestUnder(road);
}

TEST_F(SplinePavingControllerTests, TwoRoadsCrossingPaveItOnceAndTheShorterReadsPavedByTheLonger)
{
    const ECS::EntityHandle longer = CreateMember({Vector3(-30.0f, 0.0f, 0.0f), Vector3(30.0f, 0.0f, 0.0f)}, false,
                                                  Components::SplineNetworkMemberKind::Road);
    const ECS::EntityHandle shorter = CreateMember({Vector3(0.0f, 0.0f, -20.0f), Vector3(0.0f, 0.0f, 20.0f)}, false,
                                                   Components::SplineNetworkMemberKind::Road);
    m_World.AddComponentImmediate(longer, Components::SplinePaving{});
    m_World.AddComponentImmediate(shorter, Components::SplinePaving{});

    UpdateFrames();

    const PavingStatus* owner = m_Controllers.PavingStatusOf(longer);
    const PavingStatus* pavedBy = m_Controllers.PavingStatusOf(shorter);
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(pavedBy, nullptr);
    EXPECT_EQ(owner->Role, PavingRole::Owns);
    ASSERT_EQ(owner->Junctions.size(), 1u);
    EXPECT_EQ(owner->Junctions[0].State, PavingJunctionState::Paved);
    EXPECT_EQ(owner->Junctions[0].Road, shorter);
    EXPECT_NEAR(owner->Junctions[0].RoadMeters, 20.0f, 0.05f) << "the crossing lies halfway along the shorter road";
    EXPECT_EQ(pavedBy->Role, PavingRole::PavedBy);
    EXPECT_EQ(pavedBy->PavedBy, longer);
    ASSERT_EQ(pavedBy->Junctions.size(), 1u);
    EXPECT_EQ(pavedBy->Junctions[0].State, PavingJunctionState::PavedByOther);
    EXPECT_EQ(pavedBy->Junctions[0].PavedBy, longer);
    EXPECT_TRUE(pavedBy->Notices.empty());
}

TEST_F(SplinePavingControllerTests, ARoadLeftOutOfARoadsPavingReadsLeftOutAtTheCrossing)
{
    // The paved road's arm toward -Z comes first at the crossing, before either arm of the road left out.
    const ECS::EntityHandle paved = CreateMember({Vector3(0.0f, 0.0f, -30.0f), Vector3(0.0f, 0.0f, 30.0f)}, false,
                                                 Components::SplineNetworkMemberKind::Road);
    const ECS::EntityHandle leftOut = CreateMember({Vector3(-20.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 0.0f)}, false,
                                                   Components::SplineNetworkMemberKind::Road);
    Components::SplinePaving paving{};
    paving.Members[0] = {leftOut, Components::SplinePavingMemberMode::Ignore};
    paving.MemberCount = 1;
    m_World.AddComponentImmediate(paved, paving);

    UpdateFrames();

    const PavingStatus* status = m_Controllers.PavingStatusOf(paved);
    ASSERT_NE(status, nullptr);
    ASSERT_EQ(status->Junctions.size(), 1u);
    EXPECT_EQ(status->Junctions[0].Road, leftOut);
    EXPECT_EQ(status->Junctions[0].State, PavingJunctionState::Ignored);
}

TEST_F(SplinePavingControllerTests, ARoadAboveASquareThatOwnsItsGroundIsPavedIntoIt)
{
    const ECS::EntityHandle square = CreateSquare();
    Components::SplinePaving paving{};
    paving.GroundAuthority = Components::SplineGroundAuthority::Owns;
    m_World.AddComponentImmediate(square, paving);
    // The road's end lies inside the square's rim, half a meter above it.
    const ECS::EntityHandle road = CreateMember({Vector3(-40.0f, 0.5f, 0.0f), Vector3(-4.0f, 0.5f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);

    UpdateFrames();

    const PavingStatus* status = m_Controllers.PavingStatusOf(square);
    ASSERT_NE(status, nullptr);
    ASSERT_EQ(status->Junctions.size(), 1u);
    EXPECT_EQ(status->Junctions[0].Road, road);
    EXPECT_EQ(status->Junctions[0].State, PavingJunctionState::Paved);
    EXPECT_TRUE(status->Notices.empty());
    const PavingWorld paved = GatherPavings(m_World, PavingGatherDepth::Full);
    ASSERT_EQ(paved.Sources.size(), 1u);
    EXPECT_TRUE(paved.Sources[0].UnpavedRoads.empty());
}

TEST_F(SplinePavingControllerTests, ARoadAboveASquareThatGradesItsGroundIsPavedIntoIt)
{
    const ECS::EntityHandle square = CreateSquare();
    Components::SplinePaving paving{};
    paving.GroundAuthority = Components::SplineGroundAuthority::Grades;
    m_World.AddComponentImmediate(square, paving);
    const ECS::EntityHandle road = CreateMember({Vector3(-40.0f, 0.5f, 0.0f), Vector3(-4.0f, 0.5f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);

    UpdateFrames();

    const PavingStatus* status = m_Controllers.PavingStatusOf(square);
    ASSERT_NE(status, nullptr);
    ASSERT_EQ(status->Junctions.size(), 1u);
    EXPECT_EQ(status->Junctions[0].Road, road);
    EXPECT_EQ(status->Junctions[0].State, PavingJunctionState::Paved);
    EXPECT_TRUE(status->Notices.empty());
}

TEST_F(SplinePavingControllerTests, ASplitLevelRuleKeepsARoadOutOfASquareThatOwnsItsGround)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const ECS::EntityHandle road = CreateMember({Vector3(-40.0f, 0.5f, 0.0f), Vector3(-4.0f, 0.5f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    // The road meets the rim 30 m along itself and 70 m along the square.
    Components::SplineNetworkOverrides overrides{};
    overrides.Records[0] = {square, 30.0f, 70.0f, Components::SplineNodeRule::SplitLevel, {}};
    m_World.AddComponentImmediate(road, overrides);

    UpdateFrames();

    const PavingStatus* status = m_Controllers.PavingStatusOf(square);
    ASSERT_NE(status, nullptr);
    ASSERT_EQ(status->Junctions.size(), 1u);
    EXPECT_EQ(status->Junctions[0].Road, road);
    EXPECT_EQ(status->Junctions[0].State, PavingJunctionState::HeightsDiffer);
}

TEST_F(SplinePavingControllerTests, ARoadAboveASquareThatLeavesTheGroundIsNotPavedAndTheNoticeNamesBothFixes)
{
    const ECS::EntityHandle square = CreateSquare();
    Components::SplinePaving paving{};
    paving.GroundAuthority = Components::SplineGroundAuthority::None;
    m_World.AddComponentImmediate(square, paving);
    // The road's end lies inside the square's rim, half a meter above it.
    CreateMember({Vector3(-40.0f, 0.5f, 0.0f), Vector3(-4.0f, 0.5f, 0.0f)}, false,
                 Components::SplineNetworkMemberKind::Road);

    UpdateFrames();

    const PavingStatus* status = m_Controllers.PavingStatusOf(square);
    ASSERT_NE(status, nullptr);
    ASSERT_EQ(status->Junctions.size(), 1u);
    EXPECT_EQ(status->Junctions[0].State, PavingJunctionState::HeightsDiffer);
    EXPECT_NEAR(status->Junctions[0].RoadAboveMeters, 0.5f, 1.0e-3f);
    ASSERT_EQ(status->Notices.size(), 1u);
    EXPECT_NE(status->Notices[0].find("0.5 m above"), std::string::npos) << status->Notices[0];
    EXPECT_NE(status->Notices[0].find("Join at Grade to pave"), std::string::npos) << status->Notices[0];
}

TEST_F(SplinePavingControllerTests, ARoadAboveASquareThatLeavesTheGroundIsCutOutOfTheLidAlongItsStrip)
{
    const ECS::EntityHandle square = CreateSquare();
    Components::SplinePaving paving{};
    paving.GroundAuthority = Components::SplineGroundAuthority::None;
    m_World.AddComponentImmediate(square, paving);
    const ECS::EntityHandle road = CreateMember({Vector3(-40.0f, 0.5f, 0.0f), Vector3(-4.0f, 0.5f, 0.0f)}, false,
                                                Components::SplineNetworkMemberKind::Road);
    UpdateFrames();

    const PavingWorld paved = GatherPavings(m_World, PavingGatherDepth::Full);

    ASSERT_EQ(paved.Sources.size(), 1u);
    const PavingSource& source = paved.Sources[0];
    EXPECT_EQ(source.UnpavedRoads, std::vector<uint32>{road.id});
    ASSERT_FALSE(source.Arms.empty());
    for (const SplineGeometry::PavedArm& arm : source.Arms)
        EXPECT_EQ(arm.Road, road.id);
    const PavingPlan plan = PlanPavings(paved.Sources);
    ASSERT_EQ(plan.SourceArms.size(), 1u);
    EXPECT_EQ(plan.SourceArms[0], std::vector<uint32>(source.Arms.size(), kNoPavedArm));
}

TEST_F(SplinePavingControllerTests, AClosedRoadCutOutIsCutAlongItsStripAndItsJunctionsReadCutOut)
{
    const ECS::EntityHandle square = CreateSquare();
    // A ring road across the square's west rim.
    const ECS::EntityHandle ring = CreateMember({Vector3(-15.0f, 0.0f, -4.0f), Vector3(-5.0f, 0.0f, -4.0f),
                                                 Vector3(-5.0f, 0.0f, 4.0f), Vector3(-15.0f, 0.0f, 4.0f)},
                                                true, Components::SplineNetworkMemberKind::Road);
    Components::SplinePaving paving{};
    paving.Members[0] = {ring, Components::SplinePavingMemberMode::Exclude};
    paving.MemberCount = 1;
    m_World.AddComponentImmediate(square, paving);
    UpdateFrames();

    const PavingWorld paved = GatherPavings(m_World, PavingGatherDepth::Full);
    ASSERT_EQ(paved.Sources.size(), 1u);
    EXPECT_EQ(paved.Sources[0].UnpavedRoads, std::vector<uint32>{ring.id});
    EXPECT_FALSE(paved.Sources[0].Arms.empty());
    const PavingStatus* status = m_Controllers.PavingStatusOf(square);
    ASSERT_NE(status, nullptr);
    ASSERT_FALSE(status->Junctions.empty());
    for (const PavingJunctionStatus& junction : status->Junctions)
        EXPECT_EQ(junction.State, PavingJunctionState::CutOut);
}

TEST_F(SplinePavingControllerTests, RemovingThePavingRetiresItsChunks)
{
    const ECS::EntityHandle square = CreateSquare();
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    UpdateFrames();
    ASSERT_EQ(ChunkCount(), 2u);

    m_World.RemoveComponentImmediate<Components::SplinePaving>(square);
    Update();

    EXPECT_EQ(ChunkCount(), 0u);
}

} // namespace
} // namespace GameEngine::SplineRecipes
