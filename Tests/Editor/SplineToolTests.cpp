#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <gtest/gtest.h>
#include <iterator>
#include <limits>
#include <vector>

#include "Core/Application.h"
#include "Core/Engine.h"

#include "Components/Name.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Settings/SplineEditorSettings.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Input/KeyCodes.h"
#include "Terrain/TerrainTypes.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Memory/AllocationCountScope.h"
#include "PendingBvhGrid.h"
#include "SceneView/SplineTool/SplineOwnerQuery.h"
#include "SceneView/SplineTool/SplineSceneGizmo.h"
#include "SceneView/SplineTool/SplineTool.h"
#include "SceneView/SplineTool/SplineToolStripEntry.h"
#include "SceneView/TransformTool.h"
#include "Scene/MeshBvhCache.h"
#include "Scene/SceneTlas.h"
#include "Scripting/ScriptsConfig.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineNetwork.h"
#include "SplineECS/SplineService.h"
#include "TestEnvVar.h"
#include "TestTempDir.h"
#include "TerrainECS/TerrainService.h"
#include "Types/ColorUtils.h"
#include "UndoRedo/UndoRedoService.h"
#include "UndoRedo/Spline/SplineUndoHelpers.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;

using GameEngine::Components::SplineComponent;
using GameEngine::Components::Terrain;
using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;
using GameEngine::ECS::Entity;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

using GameEngine::Editor::SceneTools::ComputeSplineScreenScale;
using GameEngine::Editor::SceneTools::AcquireSplineInteractionState;
using GameEngine::Editor::SceneTools::SplineInteractionState;
using GameEngine::Editor::SceneTools::GizmoRay;
using GameEngine::Editor::SceneTools::GizmoRenderContext;
using GameEngine::Editor::SceneTools::PickScreenScale;
using GameEngine::Editor::SceneTools::PointerButton;
using GameEngine::Editor::SceneTools::PointerPhase;
using GameEngine::Editor::SceneTools::ScenePointerEvent;
using GameEngine::Editor::SceneTools::SplineTool;
using GameEngine::Rendering::CameraId;
using GameEngine::Rendering::ViewId;

namespace
{

class SplineToolTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            GameEngine::ScriptsConfig scriptsConfig{};
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
        if (!GameEngine::SplineECS::SplineService::IsInitialized())
            GameEngine::SplineECS::SplineService::Initialize();
        if (!GameEngine::TerrainECS::TerrainService::IsInitialized())
            GameEngine::TerrainECS::TerrainService::Initialize();
    }

    void SetUp() override
    {
        World* world = GetWorld();
        ASSERT_NE(world, nullptr);
        world->Clear();
        State = AcquireSplineInteractionState(*world);
        State->Selection() = {};
        State->Hover() = {};
    }

    std::shared_ptr<SplineInteractionState> State;

    World* GetWorld() const
    {
        return EngineCore::GetInstance().EnsurePrimaryWorld();
    }
};

class ScopedSplineStickToMeshSetting
{
  public:
    explicit ScopedSplineStickToMeshSetting(bool value)
        : m_Previous(GameEngine::Editor::SplineEditorSettings::Get().GetStickToMesh())
    {
        GameEngine::Editor::SplineEditorSettings::Get().SetStickToMesh(value);
    }

    ~ScopedSplineStickToMeshSetting()
    {
        GameEngine::Editor::SplineEditorSettings::Get().SetStickToMesh(m_Previous);
    }

  private:
    bool m_Previous = false;
};

// The snap's settings at their defaults, saved under a scratch user-data root, restored after.
class ScopedSplineSnapSettings
{
  public:
    explicit ScopedSplineSnapSettings(const char* name)
        : m_Root(GameEngine::TestUtils::MakeUniqueTempDirectory(name))
    {
        GameEngine::Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", m_Root.string().c_str());
        GameEngine::Editor::SplineEditorSettings& settings = GameEngine::Editor::SplineEditorSettings::Get();
        m_KnotSize = settings.GetKnotSize();
        m_ConstantScreenSize = settings.GetConstantScreenSize();
        m_SmartDistance = settings.GetSmartDistanceScaling();
        m_AutoConnect = settings.GetAutoConnect();
        m_AutoCloseLoop = settings.GetAutoCloseLoop();
        m_Smoothing = settings.GetSmoothingIterations();
        m_Conform = settings.GetConformToSurface();
        m_SnapToMeshes = settings.GetSnapToMeshes();
        settings.SetKnotSize(0.3f);
        settings.SetConstantScreenSize(true);
        settings.SetSmartDistanceScaling(false);
        settings.SetAutoConnect(true);
        settings.SetAutoCloseLoop(true);
        settings.SetSmoothingIterations(0);
        settings.SetConformToSurface(false);
        settings.SetSnapToMeshes(true);
    }

    ~ScopedSplineSnapSettings()
    {
        GameEngine::Editor::SplineEditorSettings& settings = GameEngine::Editor::SplineEditorSettings::Get();
        settings.SetKnotSize(m_KnotSize);
        settings.SetConstantScreenSize(m_ConstantScreenSize);
        settings.SetSmartDistanceScaling(m_SmartDistance);
        settings.SetAutoConnect(m_AutoConnect);
        settings.SetAutoCloseLoop(m_AutoCloseLoop);
        settings.SetSmoothingIterations(m_Smoothing);
        settings.SetConformToSurface(m_Conform);
        settings.SetSnapToMeshes(m_SnapToMeshes);
        GameEngine::Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", "");
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    const std::filesystem::path& Root() const { return m_Root; }

  private:
    std::filesystem::path m_Root;
    float m_KnotSize = 0.0f;
    bool m_ConstantScreenSize = true;
    bool m_SmartDistance = false;
    bool m_AutoConnect = true;
    bool m_AutoCloseLoop = true;
    int m_Smoothing = 0;
    bool m_Conform = false;
    bool m_SnapToMeshes = true;
};

Vector3 Normalized(Vector3 v)
{
    const float lenSq = Vector3::Dot(v, v);
    if (lenSq <= 0.0f)
        return Vector3(0.0f, 0.0f, -1.0f);
    return v * (1.0f / std::sqrt(lenSq));
}

ScenePointerEvent MakePointer(PointerPhase phase,
                              float viewX,
                              float viewY,
                              const Vector3& rayOrigin,
                              const Vector3& rayDirection)
{
    ScenePointerEvent event{};
    event.phase = phase;
    event.button = PointerButton::Left;
    event.viewX = viewX;
    event.viewY = viewY;
    event.viewW = 800.0f;
    event.viewH = 600.0f;
    event.ray.origin = rayOrigin;
    event.ray.direction = Normalized(rayDirection);
    event.cameraPos = rayOrigin;
    event.cameraRight = Vector3(1.0f, 0.0f, 0.0f);
    event.cameraUp = Vector3(0.0f, 1.0f, 0.0f);
    event.cameraForward = Vector3(0.0f, 0.0f, -1.0f);
    event.tanHalfFovY = 0.5f;
    return event;
}

TEST_F(SplineToolTests, DirectKnotDragUsesStableViewPlaneWhenSceneRayMisses)
{
    World* world = GetWorld();
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(splineService, nullptr);

    const auto splineHandle = splineService->CreateSpline(GameEngine::Spline::SplineType::Linear, false);
    auto* splineData = splineService->GetSplineData(splineHandle);
    ASSERT_NE(splineData, nullptr);
    splineData->AddPoint(Vector3(0.0f, 10.0f, 0.0f), 1.0f);
    splineData->AddPoint(Vector3(4.0f, 10.0f, 0.0f), 1.0f);
    splineService->RebuildCache(splineHandle);

    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(
        Vector3(0.0f, 0.0f, 0.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));

    SplineComponent spline{};
    spline.SplineDataIndex = splineHandle.Index();
    spline.SplineDataGeneration = splineHandle.Generation();
    entity.Set(spline);
    world->ProcessCommands();

    SplineTool tool(*GetWorld());
    tool.SetSelectionListQuery([handle = entity.GetHandle()] {
        return std::vector<GameEngine::ECS::EntityHandle>{handle};
    });

    const Vector3 camera(0.0f, 10.0f, 10.0f);
    tool.OnPointerEvent(MakePointer(
        PointerPhase::Down,
        100.0f,
        100.0f,
        camera,
        Vector3(0.0f, 0.0f, -1.0f)));

    tool.OnPointerEvent(MakePointer(
        PointerPhase::Move,
        120.0f,
        100.0f,
        camera,
        Vector3(2.0f, 0.0f, -10.0f)));

    ASSERT_GE(splineData->Points.size(), 1u);
    EXPECT_NEAR(splineData->Points[0].Position.x, 2.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.y, 10.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.z, 0.0f, 1.0e-4f);

    tool.OnPointerEvent(MakePointer(
        PointerPhase::Move,
        140.0f,
        100.0f,
        camera,
        Vector3(3.0f, 0.0f, -10.0f)));

    EXPECT_NEAR(splineData->Points[0].Position.x, 3.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.y, 10.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.z, 0.0f, 1.0e-4f);

    tool.OnPointerEvent(MakePointer(
        PointerPhase::Up,
        140.0f,
        100.0f,
        camera,
        Vector3(3.0f, 0.0f, -10.0f)));
}

// The edit-drag plane normal comes from the camera forward behind a fallback
// ladder: camera forward, then the pointer ray, then +Z. Only the normalize's
// boolean can step that ladder, so a guard that reports success on a
// non-finite input pins a NaN plane and every pointer position projects onto
// it. The ray rung here carries the same direction the healthy camera does, so
// a working ladder must land the knot exactly where the sibling drag test
// above lands it.
TEST_F(SplineToolTests, DirectKnotDragFallsBackWhenCameraForwardIsNonFinite)
{
    World* world = GetWorld();
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(splineService, nullptr);

    const auto splineHandle = splineService->CreateSpline(GameEngine::Spline::SplineType::Linear, false);
    auto* splineData = splineService->GetSplineData(splineHandle);
    ASSERT_NE(splineData, nullptr);
    splineData->AddPoint(Vector3(0.0f, 10.0f, 0.0f), 1.0f);
    splineData->AddPoint(Vector3(4.0f, 10.0f, 0.0f), 1.0f);
    splineService->RebuildCache(splineHandle);

    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(
        Vector3(0.0f, 0.0f, 0.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));

    SplineComponent spline{};
    spline.SplineDataIndex = splineHandle.Index();
    spline.SplineDataGeneration = splineHandle.Generation();
    entity.Set(spline);
    world->ProcessCommands();

    SplineTool tool(*GetWorld());
    tool.SetSelectionListQuery([handle = entity.GetHandle()] {
        return std::vector<GameEngine::ECS::EntityHandle>{handle};
    });

    const float nan = std::numeric_limits<float>::quiet_NaN();
    auto withNonFiniteForward = [nan](ScenePointerEvent event) {
        event.cameraForward = Vector3(nan, nan, nan);
        return event;
    };

    const Vector3 camera(0.0f, 10.0f, 10.0f);
    tool.OnPointerEvent(withNonFiniteForward(MakePointer(
        PointerPhase::Down,
        100.0f,
        100.0f,
        camera,
        Vector3(0.0f, 0.0f, -1.0f))));

    tool.OnPointerEvent(withNonFiniteForward(MakePointer(
        PointerPhase::Move,
        120.0f,
        100.0f,
        camera,
        Vector3(2.0f, 0.0f, -10.0f))));

    ASSERT_GE(splineData->Points.size(), 1u);
    EXPECT_TRUE(std::isfinite(splineData->Points[0].Position.x));
    EXPECT_TRUE(std::isfinite(splineData->Points[0].Position.y));
    EXPECT_TRUE(std::isfinite(splineData->Points[0].Position.z));
    EXPECT_NEAR(splineData->Points[0].Position.x, 2.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.y, 10.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.z, 0.0f, 1.0e-4f);

    tool.OnPointerEvent(withNonFiniteForward(MakePointer(
        PointerPhase::Up,
        140.0f,
        100.0f,
        camera,
        Vector3(3.0f, 0.0f, -10.0f))));
}

TEST_F(SplineToolTests, BrushStrokeCanContinueAcrossTerrain)
{
    ScopedSplineStickToMeshSetting stickToMesh(true);

    World* world = GetWorld();
    auto* terrainService = GameEngine::TerrainECS::TerrainService::TryGet();
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(terrainService, nullptr);
    ASSERT_NE(splineService, nullptr);

    GameEngine::Terrain::TerrainConfig config{};
    config.HeightmapWidth = 9;
    config.HeightmapHeight = 9;
    config.WorldSizeX = 20.0f;
    config.WorldSizeZ = 20.0f;
    config.HeightScale = 1.0f;
    config.LODLevels = 1;
    config.PatchGridSize = 8;

    const auto terrainHandle = terrainService->CreateTerrain(config);
    ASSERT_NE(terrainHandle.Generation, 0u);

    Entity terrainEntity = world->Create();
    Terrain terrain{};
    terrain.SizeX = config.WorldSizeX;
    terrain.SizeZ = config.WorldSizeZ;
    terrain.HeightScale = config.HeightScale;
    terrain.TerrainDataHandle = terrainHandle.Index;
    terrain.TerrainDataGeneration = terrainHandle.Generation;
    terrainEntity.Set(terrain);
    terrainEntity.Set(WorldTransform{});
    world->ProcessCommands();

    SplineTool tool(*GetWorld());
    tool.OnPointerEvent(MakePointer(
        PointerPhase::Down,
        100.0f,
        100.0f,
        Vector3(-2.0f, 10.0f, 0.0f),
        Vector3(0.0f, -1.0f, 0.0f)));

    tool.OnPointerEvent(MakePointer(
        PointerPhase::Move,
        120.0f,
        100.0f,
        Vector3(0.0f, 10.0f, 0.0f),
        Vector3(0.0f, -1.0f, 0.0f)));

    tool.OnPointerEvent(MakePointer(
        PointerPhase::Up,
        140.0f,
        100.0f,
        Vector3(2.0f, 10.0f, 0.0f),
        Vector3(0.0f, -1.0f, 0.0f)));

    int splineEntityCount = 0;
    const GameEngine::Spline::SplineData* createdSpline = nullptr;
    world->Query<GameEngine::ECS::Read<SplineComponent>>()
        .Each([&](GameEngine::ECS::EntityHandle, const SplineComponent& spline)
        {
            ++splineEntityCount;
            const GameEngine::SplineECS::SplineHandle handle(
                spline.SplineDataIndex, spline.SplineDataGeneration);
            createdSpline = splineService->GetSplineData(handle);
        });

    EXPECT_EQ(splineEntityCount, 1);
    ASSERT_NE(createdSpline, nullptr);
    EXPECT_GE(createdSpline->Points.size(), 2u);
}

// Escape cancels a brush stroke mid-drag: the Scene View hands the tool the input layer's key
// codes, so the rest of the drag and its release make no spline.
TEST_F(SplineToolTests, EscapeCancelsABrushStrokeMidDrag)
{
    World* world = GetWorld();
    auto* terrainService = GameEngine::TerrainECS::TerrainService::TryGet();
    ASSERT_NE(terrainService, nullptr);
    GameEngine::Terrain::TerrainConfig config{};
    config.HeightmapWidth = 9;
    config.HeightmapHeight = 9;
    config.WorldSizeX = 20.0f;
    config.WorldSizeZ = 20.0f;
    config.HeightScale = 1.0f;
    config.LODLevels = 1;
    config.PatchGridSize = 8;
    const auto terrainHandle = terrainService->CreateTerrain(config);
    ASSERT_NE(terrainHandle.Generation, 0u);
    Entity terrainEntity = world->Create();
    Terrain terrain{};
    terrain.SizeX = config.WorldSizeX;
    terrain.SizeZ = config.WorldSizeZ;
    terrain.HeightScale = config.HeightScale;
    terrain.TerrainDataHandle = terrainHandle.Index;
    terrain.TerrainDataGeneration = terrainHandle.Generation;
    terrainEntity.Set(terrain);
    terrainEntity.Set(WorldTransform{});
    world->ProcessCommands();

    SplineTool tool(*world);
    const auto pointer = [](PointerPhase phase, float x) {
        return MakePointer(phase, 100.0f + x * 10.0f, 100.0f, Vector3(x, 10.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
    };
    tool.OnPointerEvent(pointer(PointerPhase::Down, -3.0f));
    tool.OnPointerEvent(pointer(PointerPhase::Move, -1.0f));
    GameEngine::Editor::SceneTools::SceneKeyEvent escape{};
    escape.keyCode = static_cast<std::uint32_t>(GameEngine::Input::kKeyCode_Escape);
    escape.pressed = true;
    tool.OnKeyEvent(escape);
    tool.OnPointerEvent(pointer(PointerPhase::Move, 1.0f));
    tool.OnPointerEvent(pointer(PointerPhase::Up, 3.0f));

    int splines = 0;
    world->Query<GameEngine::ECS::Read<SplineComponent>>().Each(
        [&splines](GameEngine::ECS::EntityHandle, const SplineComponent&) { ++splines; });
    EXPECT_EQ(splines, 0) << "the stroke Escape cancelled made a spline at its release";
}

// ---- Control point deletion ----

struct SplineFixture
{
    GameEngine::SplineECS::SplineHandle Handle{};
    GameEngine::Spline::SplineData* Data = nullptr;
    GameEngine::ECS::EntityHandle SplineEntity{};
};

// A flat terrain at y = 0, `worldSize` meters on a side, that a raycast and a drape can hit.
void CreateFlatTerrain(World* world, float worldSize)
{
    auto* terrainService = GameEngine::TerrainECS::TerrainService::TryGet();
    ASSERT_NE(terrainService, nullptr);
    GameEngine::Terrain::TerrainConfig config{};
    config.HeightmapWidth = 9;
    config.HeightmapHeight = 9;
    config.WorldSizeX = worldSize;
    config.WorldSizeZ = worldSize;
    config.HeightScale = 1.0f;
    config.LODLevels = 1;
    config.PatchGridSize = 8;
    const auto terrainHandle = terrainService->CreateTerrain(config);
    ASSERT_NE(terrainHandle.Generation, 0u);
    Entity terrainEntity = world->Create();
    Terrain terrain{};
    terrain.SizeX = config.WorldSizeX;
    terrain.SizeZ = config.WorldSizeZ;
    terrain.HeightScale = config.HeightScale;
    terrain.TerrainDataHandle = terrainHandle.Index;
    terrain.TerrainDataGeneration = terrainHandle.Generation;
    terrainEntity.Set(terrain);
    terrainEntity.Set(WorldTransform{});
    world->ProcessCommands();
}

SplineFixture MakeSplineEntity(World* world, uint32_t pointCount, bool closed = false)
{
    SplineFixture fixture;
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    EXPECT_NE(splineService, nullptr);

    fixture.Handle = splineService->CreateSpline(GameEngine::Spline::SplineType::Linear, closed);
    fixture.Data = splineService->GetSplineData(fixture.Handle);
    EXPECT_NE(fixture.Data, nullptr);

    for (uint32_t i = 0; i < pointCount; ++i)
    {
        fixture.Data->AddPoint(Vector3(static_cast<float>(i) * 4.0f, 10.0f, 0.0f),
                               1.0f + static_cast<float>(i));
        fixture.Data->SetPointRoll(i, 0.25f * static_cast<float>(i + 1));
        fixture.Data->SetPointTangents(i,
                                       Vector3(-1.0f * static_cast<float>(i + 1), 0.0f, 0.0f),
                                       Vector3(1.0f * static_cast<float>(i + 1), 0.0f, 0.0f));
    }
    splineService->RebuildCache(fixture.Handle);

    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(
        Vector3(0.0f, 0.0f, 0.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));

    SplineComponent spline{};
    spline.SplineDataIndex = fixture.Handle.Index();
    spline.SplineDataGeneration = fixture.Handle.Generation();
    entity.Set(spline);
    world->ProcessCommands();

    fixture.SplineEntity = entity.GetHandle();
    return fixture;
}

void SelectKnot(SplineInteractionState& state, GameEngine::ECS::EntityHandle entity, int32_t index)
{
    auto& selection = state.Selection();
    selection = {};
    selection.Entity = entity;
    selection.PointIndex = index;
    selection.Kind = 1;
    selection.KnotIndices = {index};
    selection.Controls = {{entity, index, 1}};
}

TEST_F(SplineToolTests, DeleteSelectedControlPointErasesOnlyThatKnot)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    const Vector3 p0 = fixture.Data->Points[0].Position;
    const Vector3 p2 = fixture.Data->Points[2].Position;
    const Vector3 p3 = fixture.Data->Points[3].Position;

    SplineTool tool(*GetWorld());
    SelectKnot(*State, fixture.SplineEntity, 1);

    EXPECT_TRUE(tool.DeleteSelectedControlPoints());

    ASSERT_EQ(fixture.Data->Points.size(), 3u);
    EXPECT_FLOAT_EQ(fixture.Data->Points[0].Position.x, p0.x);
    EXPECT_FLOAT_EQ(fixture.Data->Points[1].Position.x, p2.x);
    EXPECT_FLOAT_EQ(fixture.Data->Points[2].Position.x, p3.x);
}

// The whole point of the feature: with no knot selected the tool must decline
// the key so the scene view can still delete the entity.
TEST_F(SplineToolTests, DeleteWithNoKnotSelectedIsDeclined)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    SplineTool tool(*GetWorld());
    State->Selection() = {};

    EXPECT_FALSE(tool.DeleteSelectedControlPoints());
    EXPECT_EQ(fixture.Data->Points.size(), 4u);
}

// A Bezier handle is a property of its point, not a deletable control.
TEST_F(SplineToolTests, DeleteWithOnlyAHandleSelectedIsDeclined)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    SplineTool tool(*GetWorld());
    auto& selection = State->Selection();
    selection = {};
    selection.Entity = fixture.SplineEntity;
    selection.PointIndex = 1;
    selection.Kind = 2; // handle in
    selection.Controls = {{fixture.SplineEntity, 1, 2}};

    EXPECT_FALSE(tool.DeleteSelectedControlPoints());
    EXPECT_EQ(fixture.Data->Points.size(), 4u);
}

// At the two-point floor the removal is refused — but the key is still
// reported as handled, because falling through would delete the entity, which
// is precisely the behaviour this path exists to prevent.
TEST_F(SplineToolTests, DeleteAtTwoPointFloorIsRefusedYetHandled)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 2);

    SplineTool tool(*GetWorld());
    SelectKnot(*State, fixture.SplineEntity, 0);

    EXPECT_TRUE(tool.DeleteSelectedControlPoints());
    EXPECT_EQ(fixture.Data->Points.size(), 2u);
}

TEST_F(SplineToolTests, DeleteMovesSelectionToASurvivingNeighbour)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    SplineTool tool(*GetWorld());

    // Erasing a middle knot leaves the selection on the point that took its
    // index, so a repeated Delete keeps walking the spline.
    SelectKnot(*State, fixture.SplineEntity, 1);
    ASSERT_TRUE(tool.DeleteSelectedControlPoints());
    EXPECT_EQ(State->Selection().PointIndex, 1);
    EXPECT_EQ(State->Selection().Kind, 1);
    EXPECT_EQ(State->Selection().Entity, fixture.SplineEntity);

    // Erasing the tail clamps onto the new last point.
    ASSERT_EQ(fixture.Data->Points.size(), 3u);
    SelectKnot(*State, fixture.SplineEntity, 2);
    ASSERT_TRUE(tool.DeleteSelectedControlPoints());
    ASSERT_EQ(fixture.Data->Points.size(), 2u);
    EXPECT_EQ(State->Selection().PointIndex, 1);
}

TEST_F(SplineToolTests, DeleteErasesEverySelectedKnotInOneStep)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 5);

    const Vector3 p0 = fixture.Data->Points[0].Position;
    const Vector3 p2 = fixture.Data->Points[2].Position;
    const Vector3 p4 = fixture.Data->Points[4].Position;

    SplineTool tool(*GetWorld());
    auto& selection = State->Selection();
    selection = {};
    selection.Entity = fixture.SplineEntity;
    selection.PointIndex = 3;
    selection.Kind = 1;
    selection.KnotIndices = {1, 3};
    selection.Controls = {{fixture.SplineEntity, 1, 1},
                          {fixture.SplineEntity, 3, 1}};

    EXPECT_TRUE(tool.DeleteSelectedControlPoints());

    ASSERT_EQ(fixture.Data->Points.size(), 3u);
    EXPECT_FLOAT_EQ(fixture.Data->Points[0].Position.x, p0.x);
    EXPECT_FLOAT_EQ(fixture.Data->Points[1].Position.x, p2.x);
    EXPECT_FLOAT_EQ(fixture.Data->Points[2].Position.x, p4.x);
}

// Deleting more knots than the floor allows is all-or-nothing: a partial erase
// would leave the author guessing which knots survived.
TEST_F(SplineToolTests, DeleteIsRefusedWhenTheSelectionWouldBreachTheFloor)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 3);

    SplineTool tool(*GetWorld());
    auto& selection = State->Selection();
    selection = {};
    selection.Entity = fixture.SplineEntity;
    selection.PointIndex = 2;
    selection.Kind = 1;
    selection.KnotIndices = {0, 1, 2};
    selection.Controls = {{fixture.SplineEntity, 0, 1},
                          {fixture.SplineEntity, 1, 1},
                          {fixture.SplineEntity, 2, 1}};

    EXPECT_TRUE(tool.DeleteSelectedControlPoints());
    EXPECT_EQ(fixture.Data->Points.size(), 3u);
}

// Undo must put the point back at its index with every authored channel
// intact, not merely restore the count.
TEST_F(SplineToolTests, UndoRestoresTheDeletedPointExactly)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    const std::vector<GameEngine::Spline::SplineControlPoint> before = fixture.Data->Points;

    GameEngine::Editor::UndoRedoService undo;
    SplineTool tool(*GetWorld());
    tool.SetUndoRedoService(&undo);
    SelectKnot(*State, fixture.SplineEntity, 1);

    ASSERT_TRUE(tool.DeleteSelectedControlPoints());
    ASSERT_EQ(fixture.Data->Points.size(), 3u);
    ASSERT_TRUE(undo.CanUndo());

    undo.Undo();

    ASSERT_EQ(fixture.Data->Points.size(), before.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].Position.x, before[i].Position.x) << "Position " << i;
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].Position.y, before[i].Position.y) << "Position " << i;
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].Radius, before[i].Radius) << "Radius " << i;
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].Roll, before[i].Roll) << "Roll " << i;
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].TangentIn.x, before[i].TangentIn.x) << "TangentIn " << i;
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].TangentOut.x, before[i].TangentOut.x) << "TangentOut " << i;
    }

    undo.Redo();
    EXPECT_EQ(fixture.Data->Points.size(), 3u);
}

// The spline tool the tool strip registers edits through its view's undo service, and
// takes the Delete key through the tool hook the view calls.
TEST_F(SplineToolTests, TheRegisteredSplineToolUndoesTheEditItsDeleteHookMade)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    GameEngine::Editor::UndoRedoService undo;
    GameEngine::Editor::SplineToolServices services;
    services.Undo = &undo;
    const std::unique_ptr<GameEngine::Editor::SceneTools::ISceneTool> tool =
        GameEngine::Editor::CreateSplineTool(*world, std::move(services));
    SelectKnot(*State, fixture.SplineEntity, 1);

    ASSERT_TRUE(tool->DeleteSelection());
    ASSERT_EQ(fixture.Data->Points.size(), 3u);
    ASSERT_TRUE(undo.CanUndo());
    undo.Undo();
    EXPECT_EQ(fixture.Data->Points.size(), 4u);
}

// Selecting a spline makes the spline entry claim the view, and a selected knot is what
// Frame Selected centres on.
TEST_F(SplineToolTests, TheSplineEntryClaimsASplineSelectionAndFramesItsKnot)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);
    const GameEngine::ECS::EntityHandle plain = world->Create().GetHandle();
    const GameEngine::Editor::SceneViewToolStripEntry entry = GameEngine::Editor::MakeSplineToolStripEntry();
    ASSERT_TRUE(entry.ActivatesForSelection);
    ASSERT_TRUE(entry.FrameTarget);

    EXPECT_FALSE(entry.ActivatesForSelection(*world, plain, {plain}));
    EXPECT_TRUE(entry.ActivatesForSelection(*world, plain, {plain, fixture.SplineEntity}));

    Vector3 target{};
    EXPECT_FALSE(entry.FrameTarget(*world, target));
    SelectKnot(*State, fixture.SplineEntity, 2);
    ASSERT_TRUE(entry.FrameTarget(*world, target));
    EXPECT_FLOAT_EQ(target.x, fixture.Data->Points[2].Position.x);
}

// A spline another feature owns as its shape (a mark-up region's outline) is left out of "show
// all controls": a road's edit never offers its knots, its own edit offers no road's, and a
// hidden owner's knots never show, while the selected region still offers its own.
TEST_F(SplineToolTests, AClaimedSplineOffersItsKnotsOnlyWhileSelectedAndShown)
{
    World* world = GetWorld();
    auto& settings = GameEngine::Editor::SplineEditorSettings::Get();
    const bool showAllBefore = settings.GetShowAllControls();
    settings.SetShowAllControls(true);
    const SplineFixture road = MakeSplineEntity(world, 2); // knots at x = 0 and 4
    const SplineFixture region = MakeSplineEntity(world, 3, /*closed=*/true);
    for (std::size_t i = 0; i < region.Data->Points.size(); ++i)
        region.Data->Points[i].Position.x += 20.0f; // knots at x = 20, 24 and 28
    GameEngine::SplineECS::SplineService::TryGet()->RebuildCache(region.Handle);
    bool regionHidden = false;
    GameEngine::Editor::SetSplineOwnerQuery(
        [&](const GameEngine::ECS::World&, GameEngine::ECS::EntityHandle entity) {
            return GameEngine::Editor::SplineOwnerClaim{entity == region.SplineEntity,
                                                         entity == region.SplineEntity && regionHidden};
        });

    SplineTool tool(*world);
    GameEngine::ECS::EntityHandle selected = road.SplineEntity;
    tool.SetSelectionListQuery([&selected] { return std::vector<GameEngine::ECS::EntityHandle>{selected}; });
    // The entity whose knot the pointer hovers when aimed straight at the knot at `x`.
    const auto hoveredAt = [&](float x) {
        ScenePointerEvent move = MakePointer(PointerPhase::Move, 100.0f, 100.0f, Vector3(x, 10.0f, 10.0f),
                                             Vector3(0.0f, 0.0f, -1.0f));
        move.button = PointerButton::None;
        tool.OnPointerEvent(move);
        return State->Hover().Entity;
    };

    EXPECT_EQ(hoveredAt(4.0f), road.SplineEntity) << "positive control: the selected road's own knot";
    EXPECT_FALSE(hoveredAt(24.0f) == region.SplineEntity) << "a road edit offered the region's knot";
    selected = region.SplineEntity;
    EXPECT_EQ(hoveredAt(24.0f), region.SplineEntity) << "the selected region did not offer its own knot";
    EXPECT_FALSE(hoveredAt(4.0f) == road.SplineEntity) << "a region edit offered the road's knot";
    regionHidden = true;
    EXPECT_FALSE(hoveredAt(24.0f).IsValid()) << "a hidden region offered its knot";

    GameEngine::Editor::SetSplineOwnerQuery({});
    settings.SetShowAllControls(showAllBefore);
}

// A generated spline (a paving's approach piece) is its generator's: claimed and hidden with no owner query.
TEST_F(SplineToolTests, AGeneratedSplineIsClaimedAndHidden)
{
    World* world = GetWorld();
    const SplineFixture authored = MakeSplineEntity(world, 2);
    const SplineFixture generated = MakeSplineEntity(world, 2);
    world->AddComponentImmediate(generated.SplineEntity, GameEngine::Components::RuntimeOnlyEntity{});

    EXPECT_FALSE(GameEngine::Editor::QuerySplineOwner(*world, authored.SplineEntity).Claimed);
    const GameEngine::Editor::SplineOwnerClaim claim = GameEngine::Editor::QuerySplineOwner(*world, generated.SplineEntity);
    EXPECT_TRUE(claim.Claimed);
    EXPECT_TRUE(claim.Hidden);
}

// The spline gizmo draws knots by the same rule: while a region is selected, "show all controls"
// draws no road's knots (the vertices drawn match the setting off), while a selected road's edit
// draws the other road's (the positive control: the count sees knots).
TEST_F(SplineToolTests, AClaimedSplinesEditDrawsNoOtherSplinesKnots)
{
    World* world = GetWorld();
    auto& settings = GameEngine::Editor::SplineEditorSettings::Get();
    const bool showAllBefore = settings.GetShowAllControls();
    const bool drapeBefore = settings.GetDrapeToSurface();
    settings.SetDrapeToSurface(false);
    const SplineFixture road = MakeSplineEntity(world, 2);    // knots at x = 0 and 4
    const SplineFixture otherRoad = MakeSplineEntity(world, 2);
    const SplineFixture region = MakeSplineEntity(world, 3, /*closed=*/true);
    // Creating a spline can move the service's spline storage: write through the handles.
    auto* splines = GameEngine::SplineECS::SplineService::TryGet();
    for (auto& point : splines->GetSplineData(otherRoad.Handle)->Points)
        point.Position.z += 20.0f;
    for (auto& point : splines->GetSplineData(region.Handle)->Points)
        point.Position.x += 20.0f;
    splines->RebuildCache(otherRoad.Handle);
    splines->RebuildCache(region.Handle);
    for (const GameEngine::ECS::EntityHandle entity : {road.SplineEntity, otherRoad.SplineEntity, region.SplineEntity})
        world->AddComponentImmediate(entity, WorldTransform{});
    GameEngine::Editor::SetSplineOwnerQuery([&](const GameEngine::ECS::World&, GameEngine::ECS::EntityHandle entity) {
        return GameEngine::Editor::SplineOwnerClaim{entity == region.SplineEntity, false};
    });

    constexpr GameEngine::Rendering::ViewId kView = 9081;
    GameEngine::Editor::SceneTools::SplineSceneGizmo gizmo(*world);
    GameEngine::ECS::EntityHandle selected = region.SplineEntity;
    gizmo.SetSelectionQuery([&selected] { return selected; });
    const auto drawnVertices = [&](bool showAll) {
        settings.SetShowAllControls(showAll);
        GameEngine::Editor::SceneTools::ResetGizmoLineGroups(kView);
        GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups(kView);
        const Vector3 camera(10.0f, 40.0f, -40.0f);
        GameEngine::Editor::SceneTools::GizmoRenderContext context(kView, kView, &camera, nullptr, world);
        gizmo.Render(context);
        std::size_t count = 0;
        if (const auto* lines = GameEngine::Editor::SceneTools::GetGizmoLineGroups(kView))
            for (const auto& group : *lines)
                count += group.vertices.size();
        if (const auto* triangles = GameEngine::Editor::SceneTools::GetGizmoTriangleGroups(kView))
            for (const auto& group : *triangles)
                count += group.vertices.size();
        return count;
    };

    const std::size_t regionAlone = drawnVertices(false);
    EXPECT_EQ(drawnVertices(true), regionAlone) << "a region edit drew a road's knots";
    selected = road.SplineEntity;
    const std::size_t roadAlone = drawnVertices(false);
    EXPECT_GT(drawnVertices(true), roadAlone) << "positive control: a road edit draws the other road's knots";

    GameEngine::Editor::SceneTools::ResetGizmoLineGroups(kView);
    GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups(kView);
    GameEngine::Editor::SetSplineOwnerQuery({});
    settings.SetShowAllControls(showAllBefore);
    settings.SetDrapeToSurface(drapeBefore);
}

// A stroke ending on a region's knot makes a spline of its own: the auto-connect never joins a
// region's outline (it skips closed splines), while the same stroke ending on a road's end joins it.
TEST_F(SplineToolTests, AStrokeEndingOnARegionsKnotNeverJoinsIt)
{
    World* world = GetWorld();
    auto* terrainService = GameEngine::TerrainECS::TerrainService::TryGet();
    ASSERT_NE(terrainService, nullptr);
    GameEngine::Terrain::TerrainConfig config{};
    config.HeightmapWidth = 9;
    config.HeightmapHeight = 9;
    config.WorldSizeX = 40.0f;
    config.WorldSizeZ = 40.0f;
    config.HeightScale = 1.0f;
    config.LODLevels = 1;
    config.PatchGridSize = 8;
    const auto terrainHandle = terrainService->CreateTerrain(config);
    ASSERT_NE(terrainHandle.Generation, 0u);
    Entity terrainEntity = world->Create();
    Terrain terrain{};
    terrain.SizeX = config.WorldSizeX;
    terrain.SizeZ = config.WorldSizeZ;
    terrain.HeightScale = config.HeightScale;
    terrain.TerrainDataHandle = terrainHandle.Index;
    terrain.TerrainDataGeneration = terrainHandle.Generation;
    terrainEntity.Set(terrain);
    terrainEntity.Set(WorldTransform{});
    world->ProcessCommands();

    // The defaults put the snap reach 0.75 m around an end seen from 10 m.
    ScopedSplineSnapSettings snapSettings("SplineStrokeRegionKnot");
    // Knots on the ground (y = 0): the region's first at x = 0, the road's last at x = 4 + 10.
    const SplineFixture region = MakeSplineEntity(world, 3, /*closed=*/true);
    const SplineFixture road = MakeSplineEntity(world, 2);
    // Creating a spline can move the service's spline storage: every read and write goes through
    // the handle, never a SplineData pointer taken before.
    auto* splines = GameEngine::SplineECS::SplineService::TryGet();
    for (auto& point : splines->GetSplineData(region.Handle)->Points)
        point.Position = Vector3(point.Position.x, 0.0f, point.Position.x * 0.5f - 8.0f);
    for (auto& point : splines->GetSplineData(road.Handle)->Points)
        point.Position = Vector3(point.Position.x + 10.0f, 0.0f, 0.0f);
    splines->RebuildCache(region.Handle);
    splines->RebuildCache(road.Handle);
    GameEngine::Editor::SetSplineOwnerQuery([&](const GameEngine::ECS::World&, GameEngine::ECS::EntityHandle entity) {
        return GameEngine::Editor::SplineOwnerClaim{entity == region.SplineEntity, false};
    });

    SplineTool tool(*world);
    // A brush stroke along z = `z` from x = -6 to `endX`, straight down onto the terrain.
    const auto stroke = [&tool](float z, float endX) {
        const auto pointer = [z](PointerPhase phase, float x) {
            return MakePointer(phase, 100.0f + x * 10.0f, 100.0f, Vector3(x, 10.0f, z), Vector3(0.0f, -1.0f, 0.0f));
        };
        tool.OnPointerEvent(pointer(PointerPhase::Down, -6.0f));
        tool.OnPointerEvent(pointer(PointerPhase::Move, (endX - 6.0f) * 0.5f));
        tool.OnPointerEvent(pointer(PointerPhase::Up, endX));
    };
    const auto knotsOf = [splines](const SplineFixture& fixture) -> std::size_t {
        const auto* data = splines->GetSplineData(fixture.Handle);
        return data ? data->Points.size() : 0u;
    };
    const auto splineCount = [world] {
        int count = 0;
        world->Query<GameEngine::ECS::Read<SplineComponent>>().Each(
            [&count](GameEngine::ECS::EntityHandle, const SplineComponent&) { ++count; });
        return count;
    };

    stroke(-8.0f, -0.5f); // ends 0.5 m from the region's knot at (0, 0, -8)
    EXPECT_EQ(knotsOf(region), 3u) << "the stroke joined the region's outline";
    EXPECT_EQ(splineCount(), 3) << "the stroke made no spline of its own";

    const std::size_t roadKnots = knotsOf(road);
    stroke(0.0f, 9.5f); // ends 0.5 m from the road's first knot at (10, 0, 0)
    EXPECT_GT(knotsOf(road), roadKnots) << "positive control: the stroke did not join the road";
    EXPECT_EQ(splineCount(), 3);

    GameEngine::Editor::SetSplineOwnerQuery({});
}

// Picking sizes a knot's hit radius with PickScreenScale and drawing sizes the
// marker with ComputeSplineScreenScale; from the same eye at the same distance
// the two must agree, or the pick area drifts from the drawn circle.
TEST(SplineScreenScaleTests, PickAndDrawScaleAgreeFromTheSameEye)
{
    const Vector3 camera(0.0f, 2.0f, -10.0f);
    GizmoRenderContext context(ViewId{31}, CameraId{31}, &camera);
    GizmoRay ray;
    ray.origin = camera;
    const Vector3 knot(1.0f, 0.0f, 3.0f);

    for (const bool smartDistance : {false, true})
    {
        const float drawScale = ComputeSplineScreenScale(context, knot, true, smartDistance);
        EXPECT_NE(drawScale, 1.0f) << "smartDistance " << smartDistance;
        EXPECT_FLOAT_EQ(PickScreenScale(ray, knot, true, smartDistance), drawScale)
            << "smartDistance " << smartDistance;
    }
}

TEST(ColorUtilsTests, UnpackArgbReadsArgbChannelForChannel)
{
    const GameEngine::Color color = GameEngine::ColorUtils::UnpackArgb(0x80FF4000u);
    EXPECT_FLOAT_EQ(color.r, 1.0f);
    EXPECT_FLOAT_EQ(color.g, 64.0f / 255.0f);
    EXPECT_FLOAT_EQ(color.b, 0.0f);
    EXPECT_FLOAT_EQ(color.a, 128.0f / 255.0f);
}

} // namespace


TEST(SplineInteractionStateTests, CollidingHandlesKeepSelectionAndHoverInTheirWorld)
{
    World first;
    World second;
    const auto a = first.Create().GetHandle();
    const auto b = second.Create().GetHandle();
    ASSERT_EQ(a, b);
    const auto stateA = AcquireSplineInteractionState(first);
    const auto stateB = AcquireSplineInteractionState(second);
    stateA->Selection().Entity = a;
    stateA->Selection().PointIndex = 3;
    stateA->Hover().Entity = a;
    stateA->Hover().PointIndex = 2;
    EXPECT_FALSE(stateB->Selection().Entity.IsValid());
    EXPECT_FALSE(stateB->Hover().Entity.IsValid());
    stateB->Selection().Entity = b;
    stateB->Selection().PointIndex = 7;
    EXPECT_EQ(stateA->Selection().PointIndex, 3);
    EXPECT_EQ(AcquireSplineInteractionState(first), stateA);
    first.Clear();
    EXPECT_FALSE(stateA->Selection().Entity.IsValid());
    EXPECT_FALSE(stateA->Hover().Entity.IsValid());
    EXPECT_EQ(stateB->Selection().PointIndex, 7);
}

TEST_F(SplineToolTests, TwoDimensionalBrushDrawsInXYWithoutGroundHit)
{
    ScopedSplineStickToMeshSetting stickToMesh(false);
    SplineTool tool(*GetWorld());
    for (const auto phase : {PointerPhase::Down, PointerPhase::Move, PointerPhase::Up})
    {
        const float x = phase == PointerPhase::Down ? -2.0f : phase == PointerPhase::Move ? 0.0f : 2.0f;
        auto event = MakePointer(phase, 100.0f + x * 20.0f, 100.0f, {x, 3.0f, -10.0f}, {0, 0, 1});
        event.IsEditor2D = true;
        event.orthoHeight = 10.0f;
        event.tanHalfFovY = 0.0f;
        tool.OnPointerEvent(event);
    }
    auto* service = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(service, nullptr);
    size_t count = 0;
    GetWorld()->Query<GameEngine::ECS::Read<SplineComponent>, GameEngine::ECS::Read<Transform>>()
        .Each([&](const SplineComponent& reference, const Transform& transform)
        {
            ++count;
            const auto* spline = service->GetSplineData({reference.SplineDataIndex, reference.SplineDataGeneration});
            ASSERT_NE(spline, nullptr);
            ASSERT_GE(spline->Points.size(), 2u);
            for (const auto& point : spline->Points)
            {
                EXPECT_NEAR(point.Position.y + transform.GetPosition().y, 3.0f, 0.001f);
                EXPECT_NEAR(point.Position.z + transform.GetPosition().z, 0.0f, 0.001f);
            }
        });
    EXPECT_EQ(count, 1u);
}

TEST_F(SplineToolTests, TwoDimensionalBrushDrawsInTheTiltedPlaneOfTheSelection)
{
    ScopedSplineStickToMeshSetting stickToMesh(false);
    const Vector3 ownerPosition(1.0f, 2.0f, 5.0f);
    const Quaternion ownerRotation = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 0.5f);
    WorldTransform ownerWorld{};
    const Transform ownerPose = Transform::FromTRS(ownerPosition, ownerRotation, Vector3(1.0f, 1.0f, 1.0f));
    std::copy(std::begin(ownerPose.matrix), std::end(ownerPose.matrix), std::begin(ownerWorld.matrix));
    const GameEngine::ECS::EntityHandle owner = GetWorld()->CreateEntity();
    GetWorld()->AddComponentImmediate(owner, ownerWorld);

    SplineTool tool(*GetWorld());
    tool.SetSelectionQuery([owner] { return owner; });
    for (const auto phase : {PointerPhase::Down, PointerPhase::Move, PointerPhase::Up})
    {
        const float x = phase == PointerPhase::Down ? -2.0f : phase == PointerPhase::Move ? 0.0f : 2.0f;
        auto event = MakePointer(phase, 100.0f + x * 20.0f, 100.0f, {x, 3.0f, -10.0f}, {0, 0, 1});
        event.IsEditor2D = true;
        event.orthoHeight = 10.0f;
        event.tanHalfFovY = 0.0f;
        tool.OnPointerEvent(event);
    }

    // Every world point lies in the selected entity's local XY plane.
    const Vector3 ownerNormal = ownerRotation.Rotate(Vector3(0.0f, 0.0f, 1.0f));
    auto* service = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(service, nullptr);
    size_t count = 0;
    GetWorld()->Query<GameEngine::ECS::Read<SplineComponent>, GameEngine::ECS::Read<Transform>>()
        .Each([&](const SplineComponent& reference, const Transform& transform)
        {
            ++count;
            const auto* spline = service->GetSplineData({reference.SplineDataIndex, reference.SplineDataGeneration});
            ASSERT_NE(spline, nullptr);
            ASSERT_GE(spline->Points.size(), 2u);
            for (const auto& point : spline->Points)
            {
                const Vector3 world = point.Position + transform.GetPosition();
                EXPECT_NEAR(Vector3::Dot(world - ownerPosition, ownerNormal), 0.0f, 0.001f);
                EXPECT_NEAR(world.y, 3.0f, 0.001f);
            }
        });
    EXPECT_EQ(count, 1u);
}

// The lowest world y of every line and triangle vertex the gizmo draws in one frame.
float LowestDrawnGizmoY(GameEngine::Editor::SceneTools::SplineSceneGizmo& gizmo, World* world, ViewId view, bool is2D)
{
    GameEngine::Editor::SceneTools::ResetGizmoLineGroups(view);
    GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups(view);
    const Vector3 camera(2.0f, 10.0f, -40.0f);
    GameEngine::Editor::SceneTools::GizmoRenderContext context(view, view, &camera, nullptr, world);
    context.SetEditor2DMode(is2D);
    gizmo.Render(context);
    float lowest = std::numeric_limits<float>::max();
    const auto lowerTo = [&lowest](const std::vector<float>& xyz) {
        for (std::size_t yIndex = 1; yIndex < xyz.size(); yIndex += 3)
            lowest = std::min(lowest, xyz[yIndex]);
    };
    if (const auto* lines = GameEngine::Editor::SceneTools::GetGizmoLineGroups(view))
        for (const auto& group : *lines)
            lowerTo(group.vertices);
    if (const auto* triangles = GameEngine::Editor::SceneTools::GetGizmoTriangleGroups(view))
        for (const auto& group : *triangles)
            lowerTo(group.vertices);
    return lowest;
}

// A 2D view shows a spline at its authored height: nothing draped on the ground draws there.
TEST_F(SplineToolTests, TwoDimensionalViewDrawsTheSplineWithoutTheGroundDrape)
{
    World* world = GetWorld();
    CreateFlatTerrain(world, 40.0f);
    auto& settings = GameEngine::Editor::SplineEditorSettings::Get();
    const bool drapeBefore = settings.GetDrapeToSurface();
    settings.SetDrapeToSurface(true);
    const bool envelopeBefore = settings.GetShowWidthEnvelope();
    settings.SetShowWidthEnvelope(true);
    const SplineFixture spline = MakeSplineEntity(world, 2); // knots at y = 10
    world->AddComponentImmediate(spline.SplineEntity, WorldTransform{});

    constexpr ViewId kView = 9082;
    GameEngine::Editor::SceneTools::SplineSceneGizmo gizmo(*world);
    gizmo.SetSelectionQuery([&spline] { return spline.SplineEntity; });

    EXPECT_LT(LowestDrawnGizmoY(gizmo, world, kView, false), 1.0f)
        << "positive control: the 3D view drapes the spline onto the terrain";
    EXPECT_GT(LowestDrawnGizmoY(gizmo, world, kView, true), 5.0f) << "the 2D view drew the ground drape";

    GameEngine::Editor::SceneTools::ResetGizmoLineGroups(kView);
    GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups(kView);
    settings.SetDrapeToSurface(drapeBefore);
    settings.SetShowWidthEnvelope(envelopeBefore);
}

// One Scene View frame of the spline gizmo: the view's gizmo groups reset, then one Render.
void RenderSplineGizmoFrame(GameEngine::Editor::SceneTools::SplineSceneGizmo& gizmo, World* world, ViewId view)
{
    GameEngine::Editor::SceneTools::ResetGizmoLineGroups(view);
    GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups(view);
    const Vector3 camera(4.0f, 20.0f, -30.0f);
    GameEngine::Editor::SceneTools::GizmoRenderContext context(view, view, &camera, nullptr, world);
    gizmo.Render(context);
}

// The xyz streams of every line group, then every triangle group, in the order the view draws them.
std::vector<float> DrawnGizmoStreams(ViewId view)
{
    std::vector<float> streams;
    if (const auto* lines = GameEngine::Editor::SceneTools::GetGizmoLineGroups(view))
        for (const auto& group : *lines)
            streams.insert(streams.end(), group.vertices.begin(), group.vertices.end());
    if (const auto* triangles = GameEngine::Editor::SceneTools::GetGizmoTriangleGroups(view))
        for (const auto& group : *triangles)
            streams.insert(streams.end(), group.vertices.begin(), group.vertices.end());
    return streams;
}

// A 2 m cube on the ground at (x, 0), in the scene TLAS when the call returns.
void AddCubeOnTheGround(World* world, float x)
{
    Entity mesh = world->Create();
    GameEngine::Components::MeshRenderer renderer{};
    renderer.modelAssetGuid.Set(GameEngine::Engine::Renderer::PrimitiveGenerator::CubeGuid());
    mesh.Set(renderer);
    GameEngine::Components::LocalBounds bounds{};
    bounds.Box.halfExtents = {0.5f, 0.5f, 0.5f};
    mesh.Set(bounds);
    WorldTransform transform{};
    transform.matrix[0] = 2.0f;
    transform.matrix[5] = 2.0f;
    transform.matrix[10] = 2.0f;
    transform.matrix[12] = x;
    transform.matrix[13] = 1.0f;
    transform.Version = 1u;
    mesh.Set(transform);
    world->ProcessCommands();
    GameEngine::Scene::GetSceneTlas(*world).WaitCurrent(*world);
}

// A Scene View that shows the same splines again allocates nothing: the gizmo's batches and the
// view's vertex streams keep their capacity from the frame before. The test covers each knot shape,
// the draped passive lines, the selected spline's width band and its Bezier handles.
TEST_F(SplineToolTests, ASteadySplineGizmoFrameAllocatesNothingAfterWarmup)
{
    World* world = GetWorld();
    CreateFlatTerrain(world, 40.0f);
    auto& settings = GameEngine::Editor::SplineEditorSettings::Get();
    const bool drapeBefore = settings.GetDrapeToSurface();
    const bool envelopeBefore = settings.GetShowWidthEnvelope();
    const bool showAllBefore = settings.GetShowAllControls();
    const GameEngine::Editor::SplineControlRenderShape shapeBefore = settings.GetControlRenderShape();
    settings.SetDrapeToSurface(true);
    settings.SetShowWidthEnvelope(true);
    const SplineFixture selected = MakeSplineEntity(world, 4);
    const SplineFixture bezier = MakeSplineEntity(world, 3);
    const SplineFixture linear = MakeSplineEntity(world, 3);
    // Creating a spline can move the service's spline storage: write through the handles.
    auto* splines = GameEngine::SplineECS::SplineService::TryGet();
    struct Placement
    {
        SplineFixture Fixture;
        float Z = 0.0f;
        GameEngine::Spline::SplineType Type = GameEngine::Spline::SplineType::Linear;
    };
    const Placement placements[] = {{selected, 0.0f, GameEngine::Spline::SplineType::CubicBezier},
                                    {bezier, 3.0f, GameEngine::Spline::SplineType::CubicBezier},
                                    {linear, 6.0f, GameEngine::Spline::SplineType::Linear}};
    for (const Placement& placement : placements)
    {
        GameEngine::Spline::SplineData* data = splines->GetSplineData(placement.Fixture.Handle);
        data->Type = placement.Type;
        for (auto& point : data->Points)
            point.Position.z += placement.Z;
        splines->RebuildCache(placement.Fixture.Handle);
        world->AddComponentImmediate(placement.Fixture.SplineEntity, WorldTransform{});
    }

    constexpr ViewId kView = 9083;
    GameEngine::Editor::SceneTools::SplineSceneGizmo gizmo(*world);
    gizmo.SetSelectionQuery([&selected] { return selected.SplineEntity; });
    using GameEngine::Editor::SplineControlRenderShape;
    for (const bool showAll : {false, true})
    {
        for (const SplineControlRenderShape shape :
             {SplineControlRenderShape::Ring, SplineControlRenderShape::Sphere, SplineControlRenderShape::Cube})
        {
            settings.SetShowAllControls(showAll);
            settings.SetControlRenderShape(shape);
            // Two warmup frames: the second frame of a new world still builds state once.
            RenderSplineGizmoFrame(gizmo, world, kView);
            RenderSplineGizmoFrame(gizmo, world, kView);
            const std::vector<float> warmup = DrawnGizmoStreams(kView);
            std::uint64_t allocations = 0;
            {
                GameEngine::Memory::AllocationCountScope scope(GameEngine::Memory::CountWindow::ThisThread);
                RenderSplineGizmoFrame(gizmo, world, kView);
                allocations = scope.Count();
            }
            EXPECT_EQ(allocations, 0u) << "show all " << showAll << ", shape " << static_cast<int>(shape);
            EXPECT_FALSE(warmup.empty()) << "positive control: the gizmo drew the splines";
            EXPECT_EQ(DrawnGizmoStreams(kView), warmup) << "the second frame drew other geometry";
        }
    }

    GameEngine::Editor::SceneTools::ResetGizmoLineGroups(kView);
    GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups(kView);
    settings.SetShowAllControls(showAllBefore);
    settings.SetControlRenderShape(shapeBefore);
    settings.SetDrapeToSurface(drapeBefore);
    settings.SetShowWidthEnvelope(envelopeBefore);
}

// The drape lands on meshes too. A mesh that arrives under a spline after its first drape is
// on the drape the next frame, as a gizmo that drapes the spline for the first time draws it.
TEST_F(SplineToolTests, TheDrapeRidesAMeshThatArrivesUnderTheSplineAfterTheFirstDrape)
{
    World* world = GetWorld();
    CreateFlatTerrain(world, 40.0f);
    auto& settings = GameEngine::Editor::SplineEditorSettings::Get();
    const bool drapeBefore = settings.GetDrapeToSurface();
    settings.SetDrapeToSurface(true);
    const SplineFixture spline = MakeSplineEntity(world, 2); // knots at x = 0 and 4, y = 10
    world->AddComponentImmediate(spline.SplineEntity, WorldTransform{});

    constexpr ViewId kView = 9084;
    GameEngine::Editor::SceneTools::SplineSceneGizmo gizmo(*world);
    RenderSplineGizmoFrame(gizmo, world, kView);
    const std::vector<float> beforeTheMesh = DrawnGizmoStreams(kView);

    // A 2 m cube on the ground under the middle of the spline.
    AddCubeOnTheGround(world, 2.0f);

    RenderSplineGizmoFrame(gizmo, world, kView);
    const std::vector<float> afterTheMesh = DrawnGizmoStreams(kView);
    GameEngine::Editor::SceneTools::SplineSceneGizmo firstDrape(*world);
    RenderSplineGizmoFrame(firstDrape, world, kView);
    const std::vector<float> drapedWithTheMesh = DrawnGizmoStreams(kView);
    EXPECT_NE(drapedWithTheMesh, beforeTheMesh) << "positive control: the drape does not reach the mesh";
    EXPECT_EQ(afterTheMesh, drapedWithTheMesh) << "the drape kept the ground it measured before the mesh arrived";

    GameEngine::Editor::SceneTools::ResetGizmoLineGroups(kView);
    GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups(kView);
    settings.SetDrapeToSurface(drapeBefore);
}

// A steady frame reads none of the meshes under the draped splines. A change in the scene makes
// each draped spline read them once, and a moved spline reads them once for itself.
TEST_F(SplineToolTests, TheDrapeReadsTheMeshesUnderASplineOnlyWhenTheSceneOrTheSplineChanges)
{
    constexpr int kSteadyFrames = 8;
    World* world = GetWorld();
    CreateFlatTerrain(world, 40.0f);
    auto& settings = GameEngine::Editor::SplineEditorSettings::Get();
    const bool drapeBefore = settings.GetDrapeToSurface();
    settings.SetDrapeToSurface(true);
    const SplineFixture still = MakeSplineEntity(world, 2);
    const SplineFixture moved = MakeSplineEntity(world, 2);
    world->AddComponentImmediate(still.SplineEntity, WorldTransform{});
    world->AddComponentImmediate(moved.SplineEntity, WorldTransform{});

    constexpr ViewId kView = 9085;
    GameEngine::Editor::SceneTools::SplineSceneGizmo gizmo(*world);
    RenderSplineGizmoFrame(gizmo, world, kView);
    RenderSplineGizmoFrame(gizmo, world, kView);
    const uint64_t warm = gizmo.GetSceneDigestRefreshCountForTesting();
    EXPECT_EQ(warm, 2u) << "the first frames did not read the meshes once per draped spline";
    for (int frame = 0; frame < kSteadyFrames; ++frame)
        RenderSplineGizmoFrame(gizmo, world, kView);
    EXPECT_EQ(gizmo.GetSceneDigestRefreshCountForTesting(), warm) << "a steady frame read the meshes again";

    AddCubeOnTheGround(world, 2.0f);
    RenderSplineGizmoFrame(gizmo, world, kView);
    RenderSplineGizmoFrame(gizmo, world, kView);
    EXPECT_EQ(gizmo.GetSceneDigestRefreshCountForTesting(), warm + 2u)
        << "a mesh that arrived was not read once per draped spline";

    auto* transform = world->GetComponentForWrite<WorldTransform>(moved.SplineEntity);
    transform->matrix[12] = 1.0f;
    ++transform->Version;
    RenderSplineGizmoFrame(gizmo, world, kView);
    RenderSplineGizmoFrame(gizmo, world, kView);
    EXPECT_EQ(gizmo.GetSceneDigestRefreshCountForTesting(), warm + 3u)
        << "the moved spline did not read the meshes under it once";

    GameEngine::Editor::SceneTools::ResetGizmoLineGroups(kView);
    GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups(kView);
    settings.SetDrapeToSurface(drapeBefore);
}

TEST(SplineInteractionStateTests, IndexDoesNotOwnTheStateAfterItsConsumersClose)
{
    World world;
    std::weak_ptr<SplineInteractionState> weak;
    {
        const auto state = AcquireSplineInteractionState(world);
        state->Selection().PointIndex = 4;
        weak = state;
    }
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(AcquireSplineInteractionState(world)->Selection().PointIndex, -1);
}

namespace
{

using GameEngine::Components::Name;
using GameEngine::Components::SplineNetworkMember;
using GameEngine::Components::SplineNetworkMemberKind;
using GameEngine::Editor::SplineEditorSettings;
using GameEngine::SplineECS::SplineNetwork;

// The reach at the defaults (knot size 0.3, Constant Screen Size on, smart distance off) for an
// end `height` meters below the eye: twice the knot's pick radius, 0.3 times PickScreenScale.
float ReachAtHeight(float height)
{
    return 2.0f * 0.3f * (height * 0.1875f / 1.5f);
}

// A pointer straight down from `height` above `ground`, the camera looking down.
ScenePointerEvent PointerAbove(PointerPhase phase, const Vector3& ground, float height, float viewX)
{
    ScenePointerEvent event =
        MakePointer(phase, viewX, 100.0f, ground + Vector3(0.0f, height, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
    event.cameraForward = Vector3(0.0f, -1.0f, 0.0f);
    event.cameraUp = Vector3(0.0f, 0.0f, 1.0f);
    return event;
}

// A Linear spline with world points on an identity transform, a network member when `kind` is set.
GameEngine::ECS::EntityHandle MakeNamedSpline(World* world, const char* name, const std::vector<Vector3>& points,
                                              bool closed, std::optional<SplineNetworkMemberKind> kind)
{
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    const auto handle = splineService->CreateSpline(GameEngine::Spline::SplineType::Linear, closed);
    auto* data = splineService->GetSplineData(handle);
    for (const Vector3& point : points)
        data->AddPoint(point, 1.0f);
    splineService->RebuildCache(handle);
    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(Vector3(0.0f, 0.0f, 0.0f), Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
                                  Vector3(1.0f, 1.0f, 1.0f)));
    entity.Set(WorldTransform{});
    SplineComponent spline{};
    spline.SplineDataIndex = handle.Index();
    spline.SplineDataGeneration = handle.Generation();
    entity.Set(spline);
    Name entityName{};
    std::strncpy(entityName.value, name, sizeof(entityName.value) - 1);
    entity.Set(entityName);
    if (kind)
        entity.Set(SplineNetworkMember{*kind});
    world->ProcessCommands();
    return entity.GetHandle();
}

GameEngine::Spline::SplineData* DataOf(World* world, GameEngine::ECS::EntityHandle entity)
{
    const auto* spline = world->GetComponent<SplineComponent>(entity);
    return GameEngine::SplineECS::SplineService::TryGet()->GetSplineData(
        GameEngine::SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
}

void DeriveNetwork(World* world)
{
    if (!SplineNetwork::IsInitialized())
        SplineNetwork::Initialize();
    SplineNetwork::Get().Update(*world);
}

// Drags the knot at `from` to `to` with the eye `height` above each, and releases it there.
void DragKnot(SplineTool& tool, const Vector3& from, const Vector3& to, float height)
{
    tool.OnPointerEvent(PointerAbove(PointerPhase::Down, from, height, 100.0f));
    tool.OnPointerEvent(PointerAbove(PointerPhase::Move, to, height, 140.0f));
    tool.OnPointerEvent(PointerAbove(PointerPhase::Up, to, height, 140.0f));
}

// A brush stroke through `points` on the ground, the eye `height` above each sample.
void Stroke(SplineTool& tool, const std::vector<Vector3>& points, float height)
{
    tool.OnPointerEvent(PointerAbove(PointerPhase::Down, points.front(), height, 100.0f));
    for (size_t i = 1; i + 1 < points.size(); ++i)
        tool.OnPointerEvent(PointerAbove(PointerPhase::Move, points[i], height, 100.0f + 10.0f * static_cast<float>(i)));
    tool.OnPointerEvent(PointerAbove(PointerPhase::Move, points.back(), height, 300.0f));
    tool.OnPointerEvent(PointerAbove(PointerPhase::Up, points.back(), height, 300.0f));
}

std::vector<GameEngine::ECS::EntityHandle> Splines(World* world)
{
    std::vector<GameEngine::ECS::EntityHandle> entities;
    world->Query<GameEngine::ECS::Read<SplineComponent>>().Each(
        [&entities](GameEngine::ECS::EntityHandle entity, const SplineComponent&) { entities.push_back(entity); });
    return entities;
}

// Market Square, a 20 m closed area member on the ground, its north edge at z = 20.
GameEngine::ECS::EntityHandle MakeSquare(World* world)
{
    return MakeNamedSpline(world, "Market Square",
                           {{0.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 20.0f}, {0.0f, 0.0f, 20.0f}},
                           true, SplineNetworkMemberKind::Area);
}

// One camera case of the snap: the eye height and whether the knot keeps a constant screen size.
struct SnapCase
{
    float Height = 10.0f;
    bool ConstantScreenSize = true;
    float Reach = 0.0f;
};

std::vector<SnapCase> SnapCases()
{
    // Ten times farther, the reach grows ten times; with Constant Screen Size off it is twice the
    // knot size, 0.6 m, at any distance.
    return {{10.0f, true, ReachAtHeight(10.0f)}, {100.0f, true, ReachAtHeight(100.0f)}, {100.0f, false, 0.6f}};
}

std::string Describe(const SnapCase& snapCase)
{
    return "height " + std::to_string(snapCase.Height) +
           (snapCase.ConstantScreenSize ? ", constant screen size" : ", world size");
}

// A paved slab as a generator makes it: the unit cube scaled to `halfExtents` around `center`, tagged
// RuntimeOnlyEntity, and in the scene TLAS so the spline tool's ray hits it.
void MakeGeneratedSlab(World* world, const Vector3& center, const Vector3& halfExtents)
{
    using GameEngine::Engine::Renderer::RenderExtractionSystem;
    Entity slab = world->Create();
    GameEngine::Components::MeshRenderer renderer{};
    renderer.modelAssetGuid.Set(GameEngine::Engine::Renderer::PrimitiveGenerator::CubeGuid());
    slab.Set(renderer);
    GameEngine::Components::LocalBounds bounds{};
    bounds.Box.halfExtents = {0.5f, 0.5f, 0.5f};
    slab.Set(bounds);
    WorldTransform transform{};
    transform.matrix[0] = 2.0f * halfExtents.x;
    transform.matrix[5] = 2.0f * halfExtents.y;
    transform.matrix[10] = 2.0f * halfExtents.z;
    transform.matrix[12] = center.x;
    transform.matrix[13] = center.y;
    transform.matrix[14] = center.z;
    slab.Set(transform);
    slab.Set(GameEngine::Components::RuntimeOnlyEntity{});
    world->ProcessCommands();

    RenderExtractionSystem::WorldRenderableRecord record{};
    record.entity = slab.GetHandle();
    record.worldTransform = transform;
    record.worldTransform.Version = 1u;
    record.hasBounds = true;
    record.bounds.Box.center = center;
    record.bounds.Box.halfExtents = halfExtents;
    std::vector<RenderExtractionSystem::WorldRenderableRecord> records{record};
    GameEngine::Scene::GetSceneTlas(*world).SyncFromRecords(records);
}

// The world height of every knot of `spline`.
std::vector<float> KnotHeights(World* world, GameEngine::ECS::EntityHandle spline)
{
    const auto* origin = world->GetComponent<Transform>(spline);
    const float originY = origin ? origin->matrix[13] : 0.0f;
    std::vector<float> heights;
    for (const auto& point : DataOf(world, spline)->Points)
        heights.push_back(point.Position.y + originY);
    return heights;
}

// A large grid model under a fresh GUID and one renderer of it at `center`, in the scene TLAS. Its world
// box is 1 m tall. The first ray onto it finds its BVH pending: the cache starts the build on a job.
GameEngine::SharedPtr<GameEngine::ModelAsset> AddGridWithAPendingBvh(World* world, const Vector3& center)
{
    const GameEngine::GUID guid = GameEngine::GUID::Generate();
    auto model = GameEngine::MakeShared<GameEngine::ModelAsset>(guid, "stroke_pending_bvh_grid.glb");
    model->SetMeshesForTest({PendingBvhGrid::MakeGrid()});
    EngineCore::GetInstance().GetAssetManager().RegisterLoadedAsset(guid, model);

    Entity mesh = world->Create();
    GameEngine::Components::MeshRenderer renderer{};
    renderer.modelAssetGuid.Set(guid);
    mesh.Set(renderer);
    GameEngine::Components::LocalBounds bounds{};
    bounds.Box.halfExtents = {0.5f * PendingBvhGrid::kGridSize, 0.5f, 0.5f * PendingBvhGrid::kGridSize};
    mesh.Set(bounds);
    WorldTransform transform{};
    transform.matrix[12] = center.x;
    transform.matrix[13] = center.y;
    transform.matrix[14] = center.z;
    transform.Version = 1u;
    mesh.Set(transform);
    world->ProcessCommands();
    GameEngine::Scene::GetSceneTlas(*world).WaitCurrent(*world);
    return model;
}

} // namespace

// A dragged end released within the reach of a rim lands on it and the network records the
// junction; one released beyond the reach stays where it is dropped and no junction forms.
TEST_F(SplineToolTests, ADraggedEndWithinTheReachOfARimLandsOnItAndOneBeyondItStays)
{
    ScopedSplineSnapSettings scope("SplineSnapDrag");
    World* world = GetWorld();
    for (const SnapCase& snapCase : SnapCases())
    {
        for (const float fraction : {0.5f, 1.5f})
        {
            world->Clear();
            SplineEditorSettings::Get().SetConstantScreenSize(snapCase.ConstantScreenSize);
            const auto square = MakeSquare(world);
            const auto road = MakeNamedSpline(world, "Church Walk", {{10.0f, 0.0f, 40.0f}, {10.0f, 0.0f, 25.0f}},
                                              false, SplineNetworkMemberKind::Road);
            DeriveNetwork(world);
            SplineTool tool(*world);
            tool.SetSelectionListQuery([road] { return std::vector<GameEngine::ECS::EntityHandle>{road}; });

            const float dropZ = 20.0f + snapCase.Reach * fraction;
            DragKnot(tool, {10.0f, 0.0f, 25.0f}, {10.0f, 0.0f, dropZ}, snapCase.Height);
            DeriveNetwork(world);
            const Vector3 end = DataOf(world, road)->Points.back().Position;
            const auto nodes = SplineNetwork::Get().GetNodes(*world);
            const std::string what = Describe(snapCase) + ", dropped " + std::to_string(fraction) + " reach out";
            if (fraction < 1.0f)
            {
                EXPECT_NEAR(end.z, 20.0f, 1.0e-3f) << what;
                ASSERT_EQ(nodes.size(), 1u) << what;
                EXPECT_EQ(nodes[0].Members[1], square) << what;
            }
            else
            {
                EXPECT_NEAR(end.z, dropZ, 1.0e-3f) << what;
                EXPECT_TRUE(nodes.empty()) << what;
            }
        }
    }
}

// A stroke whose last point is placed within the reach of a rim ends on the rim.
TEST_F(SplineToolTests, AStrokeWhoseLastPointIsWithinTheReachOfARimEndsOnIt)
{
    ScopedSplineSnapSettings scope("SplineSnapStroke");
    World* world = GetWorld();
    for (const SnapCase& snapCase : SnapCases())
    {
        for (const float fraction : {0.5f, 1.5f})
        {
            world->Clear();
            CreateFlatTerrain(world, 400.0f);
            SplineEditorSettings::Get().SetConstantScreenSize(snapCase.ConstantScreenSize);
            const auto square = MakeSquare(world);
            DeriveNetwork(world);
            SplineTool tool(*world);
            const float endZ = 20.0f + snapCase.Reach * fraction;
            Stroke(tool, {{10.0f, 0.0f, 60.0f}, {10.0f, 0.0f, 40.0f}, {10.0f, 0.0f, endZ}}, snapCase.Height);

            const auto splines = Splines(world);
            ASSERT_EQ(splines.size(), 2u) << Describe(snapCase);
            const auto drawn = splines[0] == square ? splines[1] : splines[0];
            const auto* origin = world->GetComponent<Transform>(drawn);
            ASSERT_NE(origin, nullptr);
            const float lastZ = DataOf(world, drawn)->Points.back().Position.z + origin->matrix[14];
            EXPECT_NEAR(lastZ, fraction < 1.0f ? 20.0f : endZ, 1.0e-3f)
                << Describe(snapCase) << ", ended " << fraction << " reach out";
        }
    }
}

// The tool menu's Snap Ends to Nearby Splines toggle turns the snap off: the end stays where it is dropped.
TEST_F(SplineToolTests, WithAutoConnectOffADraggedEndNeverSnaps)
{
    ScopedSplineSnapSettings scope("SplineSnapOff");
    SplineEditorSettings::Get().SetAutoConnect(false);
    World* world = GetWorld();
    MakeSquare(world);
    const auto road = MakeNamedSpline(world, "Church Walk", {{10.0f, 0.0f, 40.0f}, {10.0f, 0.0f, 25.0f}}, false,
                                      SplineNetworkMemberKind::Road);
    DeriveNetwork(world);
    SplineTool tool(*world);
    tool.SetSelectionListQuery([road] { return std::vector<GameEngine::ECS::EntityHandle>{road}; });
    const float dropZ = 20.0f + 0.25f * ReachAtHeight(10.0f);
    DragKnot(tool, {10.0f, 0.0f, 25.0f}, {10.0f, 0.0f, dropZ}, 10.0f);
    DeriveNetwork(world);
    EXPECT_NEAR(DataOf(world, road)->Points.back().Position.z, dropZ, 1.0e-3f);
    EXPECT_TRUE(SplineNetwork::Get().GetNodes(*world).empty());
}

// A stroke ending within the reach of another spline's end joins it, at two camera distances and
// with Constant Screen Size off; beyond the reach it makes a spline of its own.
TEST_F(SplineToolTests, AStrokeEndingWithinTheReachOfASplinesEndJoinsIt)
{
    ScopedSplineSnapSettings scope("SplineStrokeMerge");
    World* world = GetWorld();
    for (const SnapCase& snapCase : SnapCases())
    {
        for (const float fraction : {0.9f, 1.1f})
        {
            world->Clear();
            CreateFlatTerrain(world, 400.0f);
            SplineEditorSettings::Get().SetConstantScreenSize(snapCase.ConstantScreenSize);
            const auto lane = MakeNamedSpline(world, "Mill Lane", {{-40.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}, false,
                                              std::nullopt);
            SplineTool tool(*world);
            const float endX = snapCase.Reach * fraction;
            Stroke(tool, {{40.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, {endX, 0.0f, 0.0f}}, snapCase.Height);
            const std::string what = Describe(snapCase) + ", ended " + std::to_string(fraction) + " reach out";
            if (fraction < 1.0f)
            {
                EXPECT_EQ(Splines(world).size(), 1u) << what;
                EXPECT_GT(DataOf(world, lane)->Points.size(), 2u) << what;
            }
            else
            {
                EXPECT_EQ(Splines(world).size(), 2u) << what;
            }
        }
    }
}

// The stroke's loop close and the end drag's loop close reach as far as the stroke merge; with
// Auto-Close Loop off no loop closes.
TEST_F(SplineToolTests, TheLoopCloseReachesAsFarAsTheMergeAndAutoCloseLoopOffClosesNone)
{
    ScopedSplineSnapSettings scope("SplineLoopClose");
    World* world = GetWorld();
    const float height = 100.0f;
    const float reach = ReachAtHeight(height);
    struct LoopCase
    {
        float Fraction;
        bool AutoCloseLoop;
        bool Closes;
    };
    for (const LoopCase loop : {LoopCase{0.9f, true, true}, LoopCase{1.1f, true, false}, LoopCase{0.5f, false, false}})
    {
        const std::string what = std::to_string(loop.Fraction) + " reach out, Auto-Close Loop " +
                                 (loop.AutoCloseLoop ? "on" : "off");
        SplineEditorSettings::Get().SetAutoCloseLoop(loop.AutoCloseLoop);

        // A stroke around a 40 m square that ends short of its start.
        world->Clear();
        CreateFlatTerrain(world, 400.0f);
        {
            SplineTool tool(*world);
            Stroke(tool,
                   {{0.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f},
                    {0.0f, 0.0f, reach * loop.Fraction}},
                   height);
        }
        const auto strokes = Splines(world);
        ASSERT_EQ(strokes.size(), 1u) << what;
        EXPECT_EQ(DataOf(world, strokes[0])->Closed, loop.Closes) << "stroke, " << what;

        // An open U whose last end is dragged to the same distance from its first.
        world->Clear();
        const auto u = MakeNamedSpline(world, "Loop",
                                       {{0.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f}},
                                       false, std::nullopt);
        {
            SplineTool tool(*world);
            tool.SetSelectionListQuery([u] { return std::vector<GameEngine::ECS::EntityHandle>{u}; });
            DragKnot(tool, {0.0f, 0.0f, 40.0f}, {0.0f, 0.0f, reach * loop.Fraction}, height);
        }
        EXPECT_EQ(DataOf(world, u)->Closed, loop.Closes) << "drag, " << what;
    }
}

// A stroke that starts on generator output (a paved slab) and runs onto open ground saves an open spline
// that ends on the ground: the stroke does not stick to the slab, and its first samples, all within the
// loop-close reach of its start, do not latch the close.
TEST_F(SplineToolTests, AStrokeFromAGeneratedMeshOntoTheGroundStaysOpen)
{
    ScopedSplineSnapSettings scope("SplineStrokeFromGenerated");
    ScopedSplineStickToMeshSetting stickToMesh(true);
    World* world = GetWorld();
    world->Clear();
    CreateFlatTerrain(world, 400.0f);
    MakeGeneratedSlab(world, Vector3(0.0f, 0.0f, 0.0f), Vector3(5.0f, 0.5f, 5.0f));

    // One sample a meter from the slab's center to 20 m out; the reach at 100 m is 7.5 m.
    std::vector<Vector3> points;
    for (int i = 0; i <= 20; ++i)
        points.push_back(Vector3(static_cast<float>(i), 0.0f, 0.0f));
    {
        SplineTool tool(*world);
        Stroke(tool, points, 100.0f);
    }

    const auto strokes = Splines(world);
    ASSERT_EQ(strokes.size(), 1u);
    const auto* data = DataOf(world, strokes[0]);
    EXPECT_FALSE(data->Closed);
    const auto* origin = world->GetComponent<Transform>(strokes[0]);
    ASSERT_NE(origin, nullptr);
    EXPECT_NEAR(data->Points.back().Position.x + origin->matrix[12], 20.0f, 1.0e-3f);
    EXPECT_NEAR(data->Points.back().Position.y + origin->matrix[13], 0.0f, 1.0e-3f);
}

// A stroke drawn over generator output (a paved slab) lands on the ground under it: the slab is rebuilt from
// its spline, so it is never the ground of a new stroke.
TEST_F(SplineToolTests, AStrokeOverAGeneratedMeshLandsOnTheGroundUnderIt)
{
    ScopedSplineSnapSettings scope("SplineStrokeOverGenerated");
    World* world = GetWorld();
    world->Clear();
    CreateFlatTerrain(world, 400.0f);
    MakeGeneratedSlab(world, Vector3(0.0f, 0.0f, 0.0f), Vector3(5.0f, 0.5f, 5.0f));

    std::vector<Vector3> points;
    for (int i = -4; i <= 4; ++i)
        points.push_back(Vector3(static_cast<float>(i), 0.0f, 1.0f));
    {
        SplineTool tool(*world);
        Stroke(tool, points, 100.0f);
    }

    const auto strokes = Splines(world);
    ASSERT_EQ(strokes.size(), 1u);
    const std::vector<float> heights = KnotHeights(world, strokes[0]);
    ASSERT_GE(heights.size(), 2u);
    for (size_t i = 0; i < heights.size(); ++i)
        EXPECT_NEAR(heights[i], 0.0f, 1.0e-3f) << "knot " << i << " is not on the ground under the slab";
}

// A stroke that starts over a mesh whose BVH is still building lands on the ground, not on the top of the
// mesh's box: a box is not a surface.
TEST_F(SplineToolTests, AStrokeOverAMeshWithAPendingBvhLandsOnTheGroundNotOnItsBox)
{
    ScopedSplineSnapSettings scope("SplineStrokeOverPendingBvh");
    World* world = GetWorld();
    world->Clear();
    CreateFlatTerrain(world, 400.0f);
    // The grid covers x from -19 to 1: only the first sample is over it, the one ray sure to find it pending.
    const auto grid = AddGridWithAPendingBvh(world, Vector3(-9.0f, 0.0f, 0.0f));

    std::vector<Vector3> points;
    for (int i = 0; i <= 10; ++i)
        points.push_back(Vector3(2.0f * static_cast<float>(i), 0.0f, 1.0f));
    {
        SplineTool tool(*world);
        Stroke(tool, points, 100.0f);
    }
    // Waits for the build job, so none outlives the test.
    (void)GameEngine::Scene::MeshBvhCache::Instance().GetOrBuild(grid->GetGUID(), 0, grid->GetMesh(0));
    GameEngine::Scene::MeshBvhCache::Instance().InvalidateAsset(grid->GetGUID());

    const auto strokes = Splines(world);
    ASSERT_EQ(strokes.size(), 1u);
    const std::vector<float> heights = KnotHeights(world, strokes[0]);
    ASSERT_GE(heights.size(), 2u);
    EXPECT_NEAR(heights.front(), 0.0f, 1.0e-3f) << "the first knot is not on the ground under the grid";
    for (size_t i = 0; i < heights.size(); ++i)
        EXPECT_LT(heights[i], 0.25f) << "knot " << i << " is on the top of the grid's box";
}

// A stroke that crosses the loop-close reach, steps back inside it, then turns and runs 20 m away saves an
// open spline: the close arms only after a sample is twice the reach from the start.
TEST_F(SplineToolTests, AStrokeThatStepsBackAtTheLoopCloseReachStaysOpen)
{
    ScopedSplineSnapSettings scope("SplineStrokeStepBack");
    World* world = GetWorld();
    world->Clear();
    CreateFlatTerrain(world, 400.0f);
    const float reach = ReachAtHeight(100.0f);

    // One sample a meter out to 8 m, one back at 7.4 m (the reach at 100 m is 7.5 m), then 20 m north.
    std::vector<Vector3> points;
    for (int i = 0; i <= 8; ++i)
        points.push_back(Vector3(static_cast<float>(i), 0.0f, 0.0f));
    points.push_back(Vector3(reach - 0.1f, 0.0f, 0.0f));
    for (int i = 1; i <= 20; ++i)
        points.push_back(Vector3(9.0f, 0.0f, static_cast<float>(i)));
    {
        SplineTool tool(*world);
        Stroke(tool, points, 100.0f);
    }

    const auto strokes = Splines(world);
    ASSERT_EQ(strokes.size(), 1u);
    EXPECT_FALSE(DataOf(world, strokes[0])->Closed);
}

// The stored reach settings are gone: a save writes neither key.
TEST_F(SplineToolTests, TheSplineSettingsSaveWritesNoStoredReach)
{
    ScopedSplineSnapSettings scope("SplineSettingsKeys");
    SplineEditorSettings::Get().SetAutoConnect(true);
    auto prefs = GameEngine::Editor::OpenEditorPreferences();
    std::string error;
    ASSERT_TRUE(prefs.Load(&error)) << error;
    EXPECT_TRUE(prefs.Contains("spline.autoConnect"));
    EXPECT_FALSE(prefs.Contains("spline.autoConnectTolerance"));
    EXPECT_FALSE(prefs.Contains("spline.closeLoopTolerance"));
}

TEST_F(SplineToolTests, TransformingControlsOnTwoSplinesIsOneUndoStep)
{
    auto* world = GetWorld();
    const auto first = MakeSplineEntity(world, 3);
    const auto second = MakeSplineEntity(world, 3);
    auto* service = GameEngine::SplineECS::SplineService::TryGet();
    const auto firstReference = *world->GetComponent<SplineComponent>(first.SplineEntity);
    const auto secondReference = *world->GetComponent<SplineComponent>(second.SplineEntity);
    const GameEngine::SplineECS::SplineHandle firstHandle{firstReference.SplineDataIndex, firstReference.SplineDataGeneration};
    const GameEngine::SplineECS::SplineHandle secondHandle{secondReference.SplineDataIndex, secondReference.SplineDataGeneration};
    const auto before = service->GetSplineData(firstHandle)->Points[0].Position;
    auto& selection = State->Selection();
    selection.Entity = first.SplineEntity;
    selection.PointIndex = 0;
    selection.Kind = 1;
    selection.Controls = {{first.SplineEntity, 0, 1}, {second.SplineEntity, 0, 1}};
    GameEngine::Editor::UndoRedoService undo;
    GameEngine::Editor::SceneTools::TransformTool transform(*world);
    SplineTool spline(*world);
    spline.SetUndoRedoService(&undo);
    spline.SetTransformTool(&transform);
    GameEngine::Editor::SceneTools::GizmoCollector collector;
    spline.GatherGizmos(collector);
    GameEngine::Editor::SceneTools::TransformTranslateGizmo* gizmo = nullptr;
    for (auto* candidate : collector.GetGizmos())
        if (auto* translation = dynamic_cast<GameEngine::Editor::SceneTools::TransformTranslateGizmo*>(candidate))
            gizmo = translation;
    ASSERT_NE(gizmo, nullptr);
    GameEngine::Editor::SceneTools::GizmoHit hit{};
    hit.kind = GameEngine::Editor::SceneTools::GizmoHitKind::Axis;
    hit.handleId = 0;
    auto down = MakePointer(PointerPhase::Down, 400, 300, before + Vector3{0, 0, 5}, {0, 0, -1});
    ASSERT_TRUE(gizmo->HandlePointerEvent(down, hit));
    auto move = down;
    move.phase = PointerPhase::Move;
    move.ray.origin.x += 2;
    ASSERT_TRUE(gizmo->HandlePointerEvent(move, hit));
    auto up = move;
    up.phase = PointerPhase::Up;
    ASSERT_TRUE(gizmo->HandlePointerEvent(up, hit));
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_FLOAT_EQ(service->GetSplineData(firstHandle)->Points[0].Position.x, before.x + 2);
    EXPECT_FLOAT_EQ(service->GetSplineData(secondHandle)->Points[0].Position.x, before.x + 2);
    undo.Undo();
    EXPECT_FLOAT_EQ(service->GetSplineData(firstHandle)->Points[0].Position.x, before.x);
    EXPECT_FLOAT_EQ(service->GetSplineData(secondHandle)->Points[0].Position.x, before.x);
    undo.Redo();
    EXPECT_FLOAT_EQ(service->GetSplineData(firstHandle)->Points[0].Position.x, before.x + 2);
    EXPECT_FLOAT_EQ(service->GetSplineData(secondHandle)->Points[0].Position.x, before.x + 2);
}

TEST_F(SplineToolTests, SnapshotFollowsTheOwnersRecreatedSplineWithoutRestoringRuntimeHandles)
{
    auto* world = GetWorld();
    const auto created = MakeSplineEntity(world, 3);
    auto* service = GameEngine::SplineECS::SplineService::TryGet();
    auto* component = world->GetComponentForWrite<SplineComponent>(created.SplineEntity);
    const GameEngine::SplineECS::SplineHandle original{component->SplineDataIndex, component->SplineDataGeneration};
    component->DefaultRadius = 3;
    service->GetSplineData(original)->SetPointPosition(0, {4, 5, 0});
    auto target = GameEngine::Editor::SplineUndo::MakeSplineEditableSnapshotTarget(
        service, original, world, created.SplineEntity, nullptr, "Edit spline");
    GameEngine::Editor::UndoRedoService::SnapshotTarget::Snapshot snapshot;
    ASSERT_TRUE(target.Capture(snapshot));
    service->DestroySpline(original);
    const auto replacement = service->CreateSpline();
    component->SplineDataIndex = replacement.Index();
    component->SplineDataGeneration = replacement.Generation();
    component->DefaultRadius = 1;
    ASSERT_TRUE(target.Apply(snapshot));
    EXPECT_EQ(component->SplineDataGeneration, replacement.Generation());
    EXPECT_FLOAT_EQ(component->DefaultRadius, 1);
    EXPECT_FLOAT_EQ(service->GetSplineData(replacement)->Points[0].Position.x, 4);
    world->DestroyEntityImmediate(created.SplineEntity);
    EXPECT_FALSE(target.Apply(snapshot));
    service->DestroySpline(replacement);
}
