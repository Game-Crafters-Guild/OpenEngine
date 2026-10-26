// The engine's prefab library: what makes a prefab resident, the spawn of one that is not, the
// generations of a rebuild and the cleanup of a failed build. Each test boots a headless engine,
// whose library the scene loads and the spawns use.

#include "EngineLogCapture.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/AssetRef.h"
#include "Components/Hierarchy.h"
#include "Components/PrefabInstance.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ClonePlan.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentTypeName.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Reflection.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/PrefabLibrary.h"
#include "Scene/PrefabSpawn.h"
#include "Scene/ReflectionSceneSchema.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scripting/ScriptsConfig.h"
#include "Types/StringUtils.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// A component with a PrefabRef field, as a game's spawner would author one.
struct PrefabSpawnerProbe
{
    GameEngine::Components::PrefabRef Prefab;
};
GE_REFLECT(PrefabSpawnerProbe, Prefab);

namespace GameEngine
{
namespace
{

constexpr std::string_view kNotResidentMessage =
    "Prefab Soldier.prefab is not loaded. Reference it from a PrefabRef field of a component in the scene, or "
    "place an instance in the scene.";

constexpr std::string_view kSoldierPrefab = "[prefab version=1]\n"
                                            "[entity id=\"root\"]\n"
                                            "[entity id=\"rifle\" parent=\"root\"]\n";

// The registries are process-wide, so the probe joins them once.
void EnsureProbeRegistered()
{
    static const bool registered = []
    {
        const auto type = ECS::GetComponentTypeId<PrefabSpawnerProbe>();
        ECS::ComponentFieldRegistry::Register(type, ECS::Reflection<PrefabSpawnerProbe>::Fields,
                                              ECS::ComponentTypeName<PrefabSpawnerProbe>());
        const PrefabSpawnerProbe defaults{};
        ECS::ComponentFactory::RegisterDefaultBytes(type, &defaults, sizeof(defaults), true);
        Scene::SceneSchemaRegistry::Register(std::make_unique<Scene::ReflectionSceneSchema>(type, "PrefabSpawnerProbe"));
        return true;
    }();
    ASSERT_TRUE(registered);
}

class PrefabLibraryTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        EnsureProbeRegistered();
        m_Capture.emplace(&m_Lines, Logger::LogLevel::Info);
        m_OriginalWorkingDirectory = std::filesystem::current_path();
        m_Workspace = TestUtils::MakeUniqueTempDirectory("PrefabLibraryTests");

        ApplicationConfig config{};
        config.WorkspaceDirectory = m_Workspace.string();
        config.AssetDirectory = "Assets";
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
        ASSERT_TRUE(EngineCore::GetInstance().Initialize(config));
        // The component handler a scene load reads the probe through, registered by a first add.
        ECS::World registration;
        registration.AddComponentImmediate(registration.CreateEntity(), PrefabSpawnerProbe{});
        m_Library = EngineCore::GetInstance().TryGetPrefabLibrary();
        ASSERT_NE(m_Library, nullptr);
        m_Resolver.emplace(EngineCore::GetInstance().GetAssetManager(), EngineCore::GetInstance().GetResolvedAssetRoot(),
                           Scene::AssetManagerSceneResolver::UnknownFile::Register);
    }

    void TearDown() override
    {
        m_Resolver.reset();
        EngineCore::GetInstance().Shutdown();
        std::error_code ec;
        std::filesystem::current_path(m_OriginalWorkingDirectory, ec);
        std::filesystem::remove_all(m_Workspace, ec);
    }

    std::filesystem::path Write(const std::string& file, std::string_view text)
    {
        const auto path = (EngineCore::GetInstance().GetResolvedAssetRoot() / file).lexically_normal();
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
        return path;
    }

    GUID Register(const std::filesystem::path& path)
    {
        const auto result = EngineCore::GetInstance().GetAssetManager().GetRegistry().RegisterAssetByPath(path);
        EXPECT_TRUE(result.IsOk()) << path.string();
        return result.IsOk() ? result.Value() : GUID::Null();
    }

    bool Load(ECS::World& world, const std::string& file, std::string_view text)
    {
        Scene::LoadOptions options{Scene::LoadMode::Replace};
        options.assetResolver = &*m_Resolver;
        options.assetRootOverride = m_Resolver->GetAssetRoot();
        return Scene::LoadSceneFromFile(world, Write(file, text), options);
    }

    std::size_t CountLines(std::string_view needle)
    {
        Logger::Log::Flush();
        return TestLog::CountLinesContaining(m_Lines, needle);
    }

    // The captured log, for a failure message.
    std::string Log() const
    {
        std::string text;
        for (const std::string& line : m_Lines)
            text += line + "\n";
        return text;
    }

    std::vector<std::string> m_Lines;
    std::optional<TestLog::ScopedEngineLogCapture> m_Capture;
    std::filesystem::path m_OriginalWorkingDirectory;
    std::filesystem::path m_Workspace;
    Scene::PrefabLibrary* m_Library = nullptr;
    std::optional<Scene::AssetManagerSceneResolver> m_Resolver;
};

// Design 3.1: a spawn of a prefab nothing references fails with the fix, logged once per prefab; a
// spawn after a scene with a PrefabRef to it loads succeeds, as a copy of the template.
TEST_F(PrefabLibraryTests, SpawnOfAnUnreferencedPrefabFailsWithTheFixAndSucceedsOnceASceneReferencesIt)
{
    const GUID soldier = Register(Write("Soldier.prefab", kSoldierPrefab));
    ASSERT_FALSE(soldier.IsNull());

    ECS::World world;
    EXPECT_FALSE(Scene::SpawnPrefab(world, soldier, Components::Transform{}, {}).IsValid());
    EXPECT_FALSE(Scene::SpawnPrefab(world, soldier, Components::Transform{}, {}).IsValid());
    EXPECT_EQ(CountLines(kNotResidentMessage), 1u) << "logged once per prefab\n" << Log();
    EXPECT_EQ(world.GetEntityCount(), 0u);

    ASSERT_TRUE(Load(world, "Barracks.scene",
                     "[scene name=\"Barracks\" version=1]\n"
                     "[entity id=\"spawner\"]\n"
                     "PrefabSpawnerProbe.Prefab = [path=\"Soldier.prefab\" guid=\"" + soldier.ToString() + "\"]\n"))
        << Scene::GetLastSceneIOError().message;
    ASSERT_EQ(m_Library->GetResidentCount(), 1u) << Log();

    const std::size_t before = world.GetEntityCount();
    const Components::Transform at = Components::Transform::FromTRS(
        Mathematics::Vector3{3.0f, 0.0f, 0.0f}, Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f});
    const ECS::EntityHandle root = Scene::SpawnPrefab(world, soldier, at, {});
    ASSERT_TRUE(root.IsValid());
    EXPECT_EQ(world.GetEntityCount(), before + 2u);
    const auto* marker = world.GetComponent<Components::PrefabInstance>(root);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->Source, soldier);
    const auto* transform = world.GetComponent<Components::Transform>(root);
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(transform->GetPosition().x, 3.0f);
}

// A prefab stays resident while a world holds it and is evicted at the frame boundary after the
// world is cleared or destroyed.
TEST_F(PrefabLibraryTests, APrefabNoWorldHoldsIsEvictedAtTheFrameBoundary)
{
    const GUID soldier = Register(Write("Soldier.prefab", kSoldierPrefab));
    const std::string scene = "[scene name=\"S\" version=1]\n[resource id=\"p\" path=\"Soldier.prefab\"]\n"
                              "[prefab id=\"a\" source=\"p\"]\n[prefab id=\"b\" source=\"p\"]\n";
    ECS::World cleared;
    ASSERT_TRUE(Load(cleared, "A.scene", scene)) << Scene::GetLastSceneIOError().message;
    auto destroyed = std::make_unique<ECS::World>();
    ASSERT_TRUE(Load(*destroyed, "B.scene", scene)) << Scene::GetLastSceneIOError().message;
    const auto* prefab = m_Library->FindByGuid(soldier);
    ASSERT_NE(prefab, nullptr);
    EXPECT_EQ(m_Library->GetReferenceCount(prefab->Path), 2u);

    m_Library->CollectAtFrameBoundary();
    EXPECT_EQ(m_Library->GetResidentCount(), 1u);
    cleared.Clear();
    m_Library->CollectAtFrameBoundary();
    ASSERT_EQ(m_Library->GetResidentCount(), 1u);
    EXPECT_EQ(m_Library->GetReferenceCount(m_Library->FindByGuid(soldier)->Path), 1u);
    destroyed.reset();
    m_Library->CollectAtFrameBoundary();
    EXPECT_EQ(m_Library->GetResidentCount(), 0u);
    EXPECT_EQ(m_Library->GetWorld().GetEntityCount(), 0u);
}

// Design 3.1, generations: a rebuild builds the new template beside the old one; the old one stays
// valid until the frame boundary, which frees it (no holder of a retired template exists).
TEST_F(PrefabLibraryTests, ARebuildKeepsBothGenerationsUntilTheFrameBoundary)
{
    const GUID soldier = Register(Write("Soldier.prefab", kSoldierPrefab));
    ECS::World world;
    ASSERT_TRUE(Load(world, "S.scene",
                     "[scene name=\"S\" version=1]\n[resource id=\"p\" path=\"Soldier.prefab\"]\n"
                     "[prefab id=\"a\" source=\"p\"]\n"))
        << Scene::GetLastSceneIOError().message;
    const auto* first = m_Library->FindByGuid(soldier);
    ASSERT_NE(first, nullptr);
    const std::vector<ECS::EntityHandle> firstEntities = first->Entities;
    EXPECT_EQ(first->Generation, 0u);

    Write("Soldier.prefab", std::string(kSoldierPrefab) + "[entity id=\"pack\" parent=\"root\"]\n");
    const GUID changed[] = {soldier};
    m_Library->RebuildChanged(changed);

    const auto* second = m_Library->FindByGuid(soldier);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->Generation, 1u);
    EXPECT_EQ(second->Entities.size(), 3u);
    EXPECT_EQ(m_Library->GetRetiredCount(), 1u);
    for (const ECS::EntityHandle entity : firstEntities)
        EXPECT_TRUE(m_Library->GetWorld().IsValid(entity)) << "the old generation is valid until the boundary";

    m_Library->CollectAtFrameBoundary();
    EXPECT_EQ(m_Library->GetRetiredCount(), 0u);
    for (const ECS::EntityHandle entity : firstEntities)
        EXPECT_FALSE(m_Library->GetWorld().IsValid(entity));
    EXPECT_EQ(m_Library->GetWorld().GetEntityCount(), 3u);
    EXPECT_EQ(m_Library->GetReferenceCount(second->Path), 1u) << "the world's hold survives the rebuild";
}

// A spawn copies through the clone plan its template's generation was inserted with; a rebuilt
// generation copies through a plan of its own.
TEST_F(PrefabLibraryTests, SpawnsReuseTheGenerationsClonePlanAndARebuildPlansAgain)
{
    const GUID soldier = Register(Write("Soldier.prefab", kSoldierPrefab));
    ECS::World holder;
    ASSERT_TRUE(Load(holder, "S.scene",
                     "[scene name=\"S\" version=1]\n[resource id=\"p\" path=\"Soldier.prefab\"]\n"
                     "[prefab id=\"a\" source=\"p\"]\n"))
        << Scene::GetLastSceneIOError().message;
    const auto* first = m_Library->FindByGuid(soldier);
    ASSERT_NE(first, nullptr);
    const ECS::ClonePlan* firstPlan = &first->Plan;

    ECS::World world;
    ASSERT_TRUE(Scene::SpawnPrefab(world, soldier, Components::Transform{}, {}).IsValid());
    ASSERT_TRUE(Scene::SpawnPrefab(world, soldier, Components::Transform{}, {}).IsValid());
    EXPECT_EQ(world.GetEntityCount(), 4u);
    ASSERT_EQ(m_Library->FindByGuid(soldier), first) << "a spawn builds no template";
    EXPECT_EQ(&first->Plan, firstPlan);

    Write("Soldier.prefab", std::string(kSoldierPrefab) + "[entity id=\"pack\" parent=\"root\"]\n");
    const GUID changed[] = {soldier};
    m_Library->RebuildChanged(changed);
    const auto* second = m_Library->FindByGuid(soldier);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(&second->Plan, firstPlan);
    ASSERT_TRUE(Scene::SpawnPrefab(world, soldier, Components::Transform{}, {}).IsValid());
    EXPECT_EQ(world.GetEntityCount(), 7u) << "the new generation's plan copies its three entities";
}

// A template build that fails part-way leaves none of its entities in the library world.
TEST_F(PrefabLibraryTests, ATemplateBuildThatFailsLeavesNothingInTheLibrary)
{
    Write("Rifle.prefab", "[prefab version=1]\n[entity id=\"root\"]\n[entity id=\"scope\" parent=\"root\"]\n");
    Write("Squad.prefab", "[prefab version=1]\n"
                          "[resource id=\"rifle\" path=\"Rifle.prefab\"]\n"
                          "[resource id=\"missing\" path=\"Missing.prefab\"]\n"
                          "[entity id=\"root\"]\n"
                          "[prefab id=\"first\" source=\"rifle\" parent=\"root\"]\n"
                          "[prefab id=\"second\" source=\"missing\" parent=\"root\"]\n");
    ECS::World world;
    EXPECT_FALSE(Load(world, "S.scene",
                      "[scene name=\"S\" version=1]\n[resource id=\"p\" path=\"Squad.prefab\"]\n"
                      "[prefab id=\"squad\" source=\"p\"]\n"));
    EXPECT_EQ(m_Library->GetResidentCount(), 0u);
    EXPECT_EQ(m_Library->GetWorld().GetEntityCount(), 0u);
}

// A PrefabRef to a prefab that does not parse logs one line: the file once, as it is spelled on disk
// and relative to the asset root, then the line and the parser's message. Nothing stays resident.
TEST_F(PrefabLibraryTests, APrefabRefToAPrefabThatDoesNotParseLogsItsFileOnceWithTheLine)
{
    const GUID broken = Register(Write("Broken.prefab", "[prefab version=1]\n[entity id=\"root\"]\n"
                                                        "[entity id=\"a.b\" parent=\"root\"]\n"));
    ECS::World world;
    ASSERT_TRUE(Load(world, "Barracks.scene",
                     "[scene name=\"Barracks\" version=1]\n"
                     "[entity id=\"spawner\"]\n"
                     "PrefabSpawnerProbe.Prefab = [path=\"Broken.prefab\" guid=\"" + broken.ToString() + "\"]\n"))
        << Scene::GetLastSceneIOError().message;

    EXPECT_EQ(CountLines("a PrefabRef names a prefab that could not be loaded: Broken.prefab:3: "), 1u) << Log();
    for (const std::string& line : m_Lines)
    {
        if (line.find("could not be loaded") == std::string::npos)
            continue;
        const std::string folded = ToLowerAscii(line);
        EXPECT_EQ(folded.find("broken.prefab"), folded.rfind("broken.prefab")) << "the file once: " << line;
    }
    EXPECT_EQ(m_Library->GetResidentCount(), 0u);
    EXPECT_EQ(m_Library->GetWorld().GetEntityCount(), 0u);
}

#if !defined(NDEBUG)
// A death-test child dies on a CRT assert; on Windows that must print to stderr instead of opening
// a dialog nobody can dismiss.
void SuppressCrtDialogsInDeathTestChild()
{
#if defined(_WIN32)
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
}

// Spawns in a job body that the main thread runs while it waits: the pool's one worker is busy.
void SpawnInAJobTheMainThreadRuns(ECS::World& world, const GUID& prefab)
{
    JobSystem::WorkStealingThreadPool pool(1);
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    JobSystem::JobCounter blocker;
    JobSystem::JobCounter spawn;
    pool.Run([&] { started = true; while (!release) std::this_thread::yield(); }, blocker);
    while (!started)
        std::this_thread::yield();
    pool.Run([&] { Scene::SpawnPrefab(world, prefab, Components::Transform{}, {}); }, spawn);
    pool.Wait(spawn);
    release = true;
    pool.Wait(blocker);
}

// A job body is not the main thread for a spawn, also when the main thread runs it while it waits.
TEST_F(PrefabLibraryTests, ASpawnInAJobBodyThatTheMainThreadRunsAsserts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const GUID soldier = Register(Write("Soldier.prefab", kSoldierPrefab));
    ECS::World world;
    ASSERT_TRUE(Load(world, "Barracks.scene",
                     "[scene name=\"Barracks\" version=1]\n"
                     "[entity id=\"spawner\"]\n"
                     "PrefabSpawnerProbe.Prefab = [path=\"Soldier.prefab\" guid=\"" + soldier.ToString() + "\"]\n"))
        << Scene::GetLastSceneIOError().message;
    EXPECT_DEATH(
        {
            SuppressCrtDialogsInDeathTestChild();
            SpawnInAJobTheMainThreadRuns(world, soldier);
        },
        "main thread");
}
#endif

} // namespace
} // namespace GameEngine
