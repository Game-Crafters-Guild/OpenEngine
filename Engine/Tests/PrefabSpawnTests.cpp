// Prefab instances at runtime: the entity order of a spawn and a subscene that takes its instances
// with it. Each test boots a headless engine, whose prefab library the scene loads and the spawns use.

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/Name.h"
#include "Components/PrefabInstance.h"
#include "Components/SceneSubsceneInstance.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/PrefabSpawn.h"
#include "Scene/SceneIO.h"
#include "Scripting/ScriptsConfig.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

// Four entities in file order whose archetypes interleave: "arm" and "hand" share one, so a copy
// grouped by archetype creates them out of file order.
constexpr std::string_view kSquadPrefab = "[prefab version=1]\n"
                                          "[entity id=\"root\"]\n"
                                          "Name.value = \"0\"\n"
                                          "[entity id=\"arm\" parent=\"root\"]\n"
                                          "Name.value = \"1\"\n"
                                          "Light.type = 1\n"
                                          "[entity id=\"pack\" parent=\"root\"]\n"
                                          "Name.value = \"2\"\n"
                                          "[entity id=\"hand\" parent=\"arm\"]\n"
                                          "Name.value = \"3\"\n"
                                          "Light.type = 1\n";

class PrefabSpawnTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_OriginalWorkingDirectory = std::filesystem::current_path();
        m_Workspace = TestUtils::MakeUniqueTempDirectory("PrefabSpawnTests");

        ApplicationConfig config{};
        config.WorkspaceDirectory = m_Workspace.string();
        config.AssetDirectory = "Assets";
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
        ASSERT_TRUE(EngineCore::GetInstance().Initialize(config));
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

    bool Load(ECS::World& world, const std::string& file, std::string_view text)
    {
        Scene::SceneReadOptions options{};
        options.assetResolver = &*m_Resolver;
        options.assetRootOverride = m_Resolver->GetAssetRoot();
        return Scene::ReadSceneInto(world, Write(file, text), options);
    }

    std::filesystem::path m_OriginalWorkingDirectory;
    std::filesystem::path m_Workspace;
    std::optional<Scene::AssetManagerSceneResolver> m_Resolver;
};

// Every named entity of `world` in creation order (handle index), as its Name.
std::vector<std::string> NamesInCreationOrder(ECS::World& world)
{
    std::vector<std::pair<std::uint32_t, std::string>> named;
    world.Query<ECS::Read<Components::Name>>().Each(
        [&named](ECS::EntityHandle entity, const Components::Name& name)
        { named.emplace_back(static_cast<std::uint32_t>(entity.index), std::string(name.View())); });
    std::sort(named.begin(), named.end());
    std::vector<std::string> names;
    for (auto& [index, name] : named)
        names.push_back(std::move(name));
    return names;
}

// Design 3.3: the k-th entity a spawn creates is the prefab's k-th entity in file order, so a save
// game or a network layer can key a member on (root, ordinal).
TEST_F(PrefabSpawnTests, TheKthEntityOfASpawnIsThePrefabsKthEntityInFileOrder)
{
    const auto prefabPath = Write("Squad.prefab", kSquadPrefab);
    const auto registered = EngineCore::GetInstance().GetAssetManager().GetRegistry().RegisterAssetByPath(prefabPath);
    ASSERT_TRUE(registered.IsOk());
    const GUID squad = registered.Value();
    ECS::World holder;
    ASSERT_TRUE(Load(holder, "Holder.scene",
                     "[scene name=\"Holder\" version=1]\n[resource id=\"p\" path=\"Squad.prefab\"]\n"
                     "[prefab id=\"held\" source=\"p\"]\n"))
        << Scene::GetLastSceneIOError().message;

    ECS::World world;
    const ECS::EntityHandle root = Scene::SpawnPrefab(world, squad, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid());
    EXPECT_EQ(NamesInCreationOrder(world), (std::vector<std::string>{"0", "1", "2", "3"}));
}

// Design 3.2: a subscene's prefab instances hang under the subscene root, so destroying that root
// unloads the instances with the subscene and leaves the rest of the scene.
TEST_F(PrefabSpawnTests, ASubsceneUnloadDestroysItsInstancesAndNothingElse)
{
    Write("Squad.prefab", kSquadPrefab);
    Write("Cell.scene", "[scene name=\"Cell\" version=1]\n"
                        "[resource id=\"p\" path=\"Squad.prefab\"]\n"
                        "[entity id=\"rock\"]\n"
                        "Name.value = \"Rock\"\n"
                        "[prefab id=\"a\" source=\"p\"]\n"
                        "[prefab id=\"b\" source=\"p\"]\n");
    ECS::World world;
    ASSERT_TRUE(Load(world, "World.scene",
                     "[scene name=\"World\" version=1]\n"
                     "[resource id=\"cell\" path=\"Cell.scene\"]\n"
                     "[entity id=\"sun\"]\n"
                     "Name.value = \"Sun\"\n"
                     "[subscene id=\"cell0\" source=\"cell\"]\n"))
        << Scene::GetLastSceneIOError().message;

    ECS::EntityHandle cell{};
    world.Query<ECS::Read<Components::SceneSubsceneInstance>>().Each(
        [&cell](ECS::EntityHandle entity, const Components::SceneSubsceneInstance&) { cell = entity; });
    ASSERT_TRUE(cell.IsValid());
    std::vector<ECS::EntityHandle> instanceRoots;
    world.Query<ECS::Read<Components::PrefabInstance>>().Each(
        [&instanceRoots](ECS::EntityHandle entity, const Components::PrefabInstance&) { instanceRoots.push_back(entity); });
    ASSERT_EQ(instanceRoots.size(), 2u);
    std::vector<ECS::EntityHandle> members;
    for (const ECS::EntityHandle instanceRoot : instanceRoots)
    {
        members.push_back(instanceRoot);
        std::vector<ECS::EntityHandle> below;
        Components::DescendantsOf(world, instanceRoot, below);
        members.insert(members.end(), below.begin(), below.end());
    }
    ASSERT_EQ(members.size(), 8u);
    ASSERT_EQ(world.GetEntityCount(), 11u) << "the sun, the subscene root, the rock and two instances of four";

    Components::DestroySubtree(world, cell);
    world.ProcessCommands();

    for (const ECS::EntityHandle member : members)
        EXPECT_FALSE(world.IsValid(member)) << "entity " << member.id << " outlived its subscene";
    EXPECT_EQ(NamesInCreationOrder(world), (std::vector<std::string>{"Sun"}));
    EXPECT_EQ(world.GetEntityCount(), 1u);
}

} // namespace
} // namespace GameEngine
