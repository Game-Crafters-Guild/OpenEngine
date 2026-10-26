// EntityCommands.Spawn through the ECS interface table: the call the C# EntityCommands makes
// (Managed/Scripting.ABI/EntityCommands.cs), on a prefab that a scene keeps loaded with a PrefabRef field.

#include "EngineLogCapture.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/AssetRef.h"
#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/PrefabInstance.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentTypeName.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Reflection.h"
#include "ECS/World.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/SceneIO.h"
#include "Scripting/ECSABI.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A reflected component with a PrefabRef field and no hand-written scene schema, as a game's spawner holds one.
struct AbiPrefabSpawnerProbe
{
    GameEngine::Components::PrefabRef Prefab;
};
GE_REFLECT(AbiPrefabSpawnerProbe, Prefab);

namespace GameEngine
{
namespace
{

constexpr std::string_view kSquadPrefab = "[prefab version=1]\n"
                                          "[entity id=\"root\"]\n"
                                          "[entity id=\"rifle\" parent=\"root\"]\n"
                                          "[entity id=\"scout\" parent=\"root\"]\n";

// The registries are process-wide, so the probe joins them once.
void EnsureProbeRegistered()
{
    static const bool registered = []
    {
        const auto type = ECS::GetComponentTypeId<AbiPrefabSpawnerProbe>();
        ECS::ComponentFieldRegistry::Register(type, ECS::Reflection<AbiPrefabSpawnerProbe>::Fields,
                                              ECS::ComponentTypeName<AbiPrefabSpawnerProbe>());
        const AbiPrefabSpawnerProbe defaults{};
        ECS::ComponentFactory::RegisterDefaultBytes(type, &defaults, sizeof(defaults), true);
        // The component handler a scene load reads the probe through, registered by a first add.
        ECS::World registration;
        registration.AddComponentImmediate(registration.CreateEntity(), AbiPrefabSpawnerProbe{});
        return true;
    }();
    ASSERT_TRUE(registered);
}

const GE_ECS_Interface_v2& EcsTable()
{
    const void* table = nullptr;
    uint32_t size = 0;
    EXPECT_EQ(GE_ECS_GetInterface(GE_ECS_ABI_VERSION_CURRENT, &table, &size), GE_Result_Ok);
    EXPECT_GE(size, sizeof(GE_ECS_Interface_v2));
    return *static_cast<const GE_ECS_Interface_v2*>(table);
}

GE_Handle HandleOf(ECS::World& world)
{
    return static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(&world));
}

GE_ECS_Transform AbiTransform(const Components::Transform& transform)
{
    GE_ECS_Transform out{};
    std::memcpy(out.matrix, transform.matrix, sizeof(out.matrix));
    return out;
}

class ScriptingAbiPrefabSpawnTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config;
            config.AssetDirectory = "Assets";
            ScriptsConfig scriptsConfig{};
            scriptsConfig.enableHotReload = false;
            engine.SetScriptsConfig(scriptsConfig);
            ASSERT_TRUE(engine.Initialize(config));
        }
        EnsureProbeRegistered();
        m_Capture.emplace(&m_Lines, Logger::LogLevel::Info);
        m_Folder = engine.GetResolvedAssetRoot() / ("ScriptingAbiPrefabSpawn_" + GUID::Generate().ToString());
        std::filesystem::create_directories(m_Folder);
        m_Resolver.emplace(engine.GetAssetManager(), engine.GetResolvedAssetRoot(),
                           Scene::AssetManagerSceneResolver::UnknownFile::Register);
    }

    void TearDown() override
    {
        m_Capture.reset();
        m_Resolver.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_Folder, ec);
    }

    std::filesystem::path Write(const std::string& file, std::string_view text)
    {
        const std::filesystem::path path = m_Folder / file;
        std::ofstream(path, std::ios::binary) << text;
        return path;
    }

    // Registers the squad prefab and loads a scene into `world` whose spawner names it, so it is resident.
    GUID LoadSceneHoldingTheSquad(ECS::World& world)
    {
        const auto registered =
            EngineCore::GetInstance().GetAssetManager().GetRegistry().RegisterAssetByPath(Write("Squad.prefab", kSquadPrefab));
        EXPECT_TRUE(registered.IsOk());
        if (!registered.IsOk())
            return GUID::Null();
        const GUID squad = registered.Value();
        const std::string relative =
            std::filesystem::relative(m_Folder / "Squad.prefab", m_Resolver->GetAssetRoot()).generic_string();
        Scene::LoadOptions options{Scene::LoadMode::Replace};
        options.assetResolver = &*m_Resolver;
        options.assetRootOverride = m_Resolver->GetAssetRoot();
        const bool loaded = Scene::LoadSceneFromFile(
            world,
            Write("Barracks.scene", "[scene name=\"Barracks\" version=1]\n"
                                    "[entity id=\"spawner\"]\n"
                                    "AbiPrefabSpawnerProbe.Prefab = [path=\"" + relative + "\" guid=\"" +
                                        squad.ToString() + "\"]\n"),
            options);
        EXPECT_TRUE(loaded) << Scene::GetLastSceneIOError().message;
        return loaded ? squad : GUID::Null();
    }

    // The captured log, for a failure message.
    std::string Log()
    {
        Logger::Log::Flush();
        std::string text;
        for (const std::string& line : m_Lines)
            text += line + '\n';
        return text;
    }

    std::filesystem::path m_Folder;
    std::optional<Scene::AssetManagerSceneResolver> m_Resolver;
    std::vector<std::string> m_Lines;
    std::optional<TestLog::ScopedEngineLogCapture> m_Capture;
};

// Design 2.7: one call returns the root at once; the root carries its tree as the prefab has it.
TEST_F(ScriptingAbiPrefabSpawnTest, SpawnReturnsARootWhoseTreeMatchesThePrefab)
{
    ECS::World world;
    const GUID squad = LoadSceneHoldingTheSquad(world);
    ASSERT_FALSE(squad.IsNull());

    const std::size_t before = world.GetEntityCount();
    const GE_ECS_Transform at = AbiTransform(Components::Transform::FromTRS(
        Mathematics::Vector3{4.0f, 0.0f, 2.0f}, Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
    GE_ECS_Entity rootId = 0;
    ASSERT_EQ(EcsTable().SpawnPrefab(HandleOf(world), squad.GetData().data(), &at, 0, &rootId), GE_Result_Ok) << Log();
    ASSERT_NE(rootId, 0u);

    const ECS::EntityHandle root(rootId);
    EXPECT_EQ(world.GetEntityCount(), before + 3u);
    const auto* marker = world.GetComponent<Components::PrefabInstance>(root);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->Source, squad);
    EXPECT_EQ(world.GetComponent<Components::Parent>(root), nullptr);
    const auto* transform = world.GetComponent<Components::Transform>(root);
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(transform->GetPosition().x, 4.0f);
    EXPECT_EQ(transform->GetPosition().z, 2.0f);

    std::size_t children = 0;
    world.Query<ECS::Read<Components::Parent>>().Each(
        [&](const Components::Parent& parent)
        {
            if (parent.Entity == root)
                ++children;
        });
    EXPECT_EQ(children, 2u);
}

// A C# system runs with structural changes deferred: the spawn still creates the entities at once, and an
// entity that existed before keeps its storage.
TEST_F(ScriptingAbiPrefabSpawnTest, SpawnWhileChangesAreDeferredCreatesAtOnceAndMovesNoExistingEntity)
{
    ECS::World world;
    const GUID squad = LoadSceneHoldingTheSquad(world);
    ASSERT_FALSE(squad.IsNull());
    const GE_ECS_Transform at = AbiTransform(Components::Transform{});
    GE_ECS_Entity firstId = 0;
    ASSERT_EQ(EcsTable().SpawnPrefab(HandleOf(world), squad.GetData().data(), &at, 0, &firstId), GE_Result_Ok);
    const ECS::EntityHandle first(firstId);
    const Components::Transform* firstTransform = world.GetComponent<Components::Transform>(first);
    ASSERT_NE(firstTransform, nullptr);

    world.SetDeferStructuralChanges(true);
    const std::size_t before = world.GetEntityCount();
    GE_ECS_Entity secondId = 0;
    EXPECT_EQ(EcsTable().SpawnPrefab(HandleOf(world), squad.GetData().data(), &at, firstId, &secondId), GE_Result_Ok);
    EXPECT_EQ(world.GetEntityCount(), before + 3u);
    EXPECT_EQ(world.GetComponent<Components::Transform>(first), firstTransform);
    const auto* parent = world.GetComponent<Components::Parent>(ECS::EntityHandle(secondId));
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->Entity, first);
    world.SetDeferStructuralChanges(false);
}

// EntityCommands.Despawn removes the whole instance: the root and every entity under it, at the point
// where queued commands run, as a DestroyEntity does.
TEST_F(ScriptingAbiPrefabSpawnTest, DespawnOfTheRootDestroysTheRootAndItsTree)
{
    ECS::World world;
    const GUID squad = LoadSceneHoldingTheSquad(world);
    ASSERT_FALSE(squad.IsNull());
    const std::size_t before = world.GetEntityCount();
    const GE_ECS_Transform at = AbiTransform(Components::Transform{});
    GE_ECS_Entity rootId = 0;
    ASSERT_EQ(EcsTable().SpawnPrefab(HandleOf(world), squad.GetData().data(), &at, 0, &rootId), GE_Result_Ok);
    const ECS::EntityHandle root(rootId);
    const std::vector<ECS::EntityHandle> children = Components::ChildrenOf(world, root);
    ASSERT_EQ(children.size(), 2u);

    world.SetDeferStructuralChanges(true);
    ASSERT_EQ(EcsTable().DespawnPrefab(HandleOf(world), rootId), GE_Result_Ok) << Log();
    EXPECT_TRUE(world.IsValid(root));
    world.SetDeferStructuralChanges(false);
    ASSERT_EQ(EcsTable().FlushDeferredCommands(HandleOf(world)), GE_Result_Ok);

    EXPECT_FALSE(world.IsValid(root));
    for (const ECS::EntityHandle child : children)
        EXPECT_FALSE(world.IsValid(child));
    EXPECT_EQ(world.GetEntityCount(), before);
}

// The instance is the hierarchy under the root at the despawn: a child moved out of the tree survives it.
TEST_F(ScriptingAbiPrefabSpawnTest, DespawnLeavesAChildThatWasMovedOutOfTheTree)
{
    ECS::World world;
    const GUID squad = LoadSceneHoldingTheSquad(world);
    ASSERT_FALSE(squad.IsNull());
    const GE_ECS_Transform at = AbiTransform(Components::Transform{});
    GE_ECS_Entity rootId = 0;
    ASSERT_EQ(EcsTable().SpawnPrefab(HandleOf(world), squad.GetData().data(), &at, 0, &rootId), GE_Result_Ok);
    const ECS::EntityHandle root(rootId);
    const std::vector<ECS::EntityHandle> children = Components::ChildrenOf(world, root);
    ASSERT_EQ(children.size(), 2u);
    const ECS::EntityHandle moved = children[0];
    const ECS::EntityHandle kept = children[1];
    world.RemoveComponentImmediate<Components::Parent>(moved);
    const std::size_t before = world.GetEntityCount();

    ASSERT_EQ(EcsTable().DespawnPrefab(HandleOf(world), rootId), GE_Result_Ok) << Log();
    ASSERT_EQ(EcsTable().FlushDeferredCommands(HandleOf(world)), GE_Result_Ok);

    EXPECT_FALSE(world.IsValid(root));
    EXPECT_FALSE(world.IsValid(kept));
    EXPECT_TRUE(world.IsValid(moved));
    EXPECT_EQ(world.GetEntityCount(), before - 2u);
}

// A prefab nothing keeps loaded spawns nothing: 0 and GE_Result_NotFound, as EntityCommands.Spawn returns 0.
TEST_F(ScriptingAbiPrefabSpawnTest, SpawnOfAPrefabThatIsNotLoadedReturnsNoRoot)
{
    ECS::World world;
    const GUID loose = GUID::Generate();
    const GE_ECS_Transform at = AbiTransform(Components::Transform{});
    GE_ECS_Entity rootId = 7;
    EXPECT_EQ(EcsTable().SpawnPrefab(HandleOf(world), loose.GetData().data(), &at, 0, &rootId), GE_Result_NotFound);
    EXPECT_EQ(rootId, 0u);
    EXPECT_EQ(world.GetEntityCount(), 0u);
}

} // namespace
} // namespace GameEngine
