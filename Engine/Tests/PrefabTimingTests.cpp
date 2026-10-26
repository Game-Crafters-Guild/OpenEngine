// The prefab timing harness: the measurements H1 (scene load), H2a (spawn burst), H2b (spawn rate)
// and H5 (library memory) of the prefab design. Every test is disabled: a timing driver runs one
// by name per process with --gtest_also_run_disabled_tests, in Release, under the timing lock.
// GE_PREFAB_TIMING_JSON names the result file and GE_PREFAB_TIMING_ARM the arm it records; the
// test skips when either is unset. Each test boots a headless engine in a temporary workspace and
// generates its prefabs and scenes there.

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/Light.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/PrefabLibrary.h"
#include "Scene/PrefabSpawn.h"
#include "Scene/SceneIO.h"
#include "Scripting/ScriptsConfig.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr int kH1Instances = 10000;
constexpr int kH1PrefabEntities = 20;
constexpr int kH2PrefabEntities = 10;
constexpr int kH2aBurst = 1000;
constexpr int kH2bSpawnsPerFrame = 200;
constexpr int kH2bFrames = 60;
constexpr int kH5Prefabs = 170;
constexpr int kH5PrefabEntities = 20;

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The parent of entity `index` of a generated prefab: a binary tree under the root.
int ParentIndex(int index)
{
    return (index - 1) / 2;
}

// The id of entity `index` of a generated prefab.
std::string PrefabEntityId(int index)
{
    return index == 0 ? std::string("root") : std::format("e{}", index);
}

// The component set of a generated entity, which decides its archetype.
enum class Parts
{
    TransformAndName,
    WithLight,
    WithCollider,
    WithLightAndCollider,
};

// An entity's components: at `x` on the x axis, named after its index in the prefab.
std::string EntityBody(int index, int x, Parts parts)
{
    std::string body = std::format("Transform.position = ({}, 0, 0)\nName.value = \"Part {}\"\n", x, index);
    if (parts == Parts::WithLight || parts == Parts::WithLightAndCollider)
        body += "Light.type = 1\nLight.intensity = 100\n";
    if (parts == Parts::WithCollider || parts == Parts::WithLightAndCollider)
        body += "PhysicsCollider.enabled = true\nPhysicsCollider.layer = 1\n";
    return body;
}

std::string PrefabText(int entities, Parts parts)
{
    std::string text = "[prefab version=1]\n";
    for (int index = 0; index < entities; ++index)
    {
        text += index == 0 ? std::string("[entity id=\"root\"]\n")
                           : std::format("[entity id=\"e{}\" parent=\"{}\"]\n", index, PrefabEntityId(ParentIndex(index)));
        text += EntityBody(index, index, parts);
    }
    return text;
}

// H1's instanced scene: kH1Instances instances of `prefabFile`, each root moved along x.
std::string InstancedSceneText(const std::string& prefabFile)
{
    std::string text = std::format("[scene name=\"Instanced\" version=1]\n[resource id=\"p\" path=\"{}\"]\n", prefabFile);
    for (int instance = 0; instance < kH1Instances; ++instance)
        text += std::format("[prefab id=\"i{}\" source=\"p\"]\nTransform.position = ({}, 0, 0)\n", instance, instance);
    return text;
}

// H1's flattened scene: the same entities, components and parent links as plain entity sections.
std::string FlattenedSceneText()
{
    std::string text = "[scene name=\"Flattened\" version=1]\n";
    for (int instance = 0; instance < kH1Instances; ++instance)
    {
        text += std::format("[entity id=\"i{}_e0\"]\n", instance);
        text += EntityBody(0, instance, Parts::WithLightAndCollider);
        for (int index = 1; index < kH1PrefabEntities; ++index)
        {
            text += std::format("[entity id=\"i{}_e{}\" parent=\"i{}_e{}\"]\n", instance, index, instance, ParentIndex(index));
            text += EntityBody(index, index, Parts::WithLightAndCollider);
        }
    }
    return text;
}

Components::Name MakeName(int index)
{
    Components::Name name{};
    std::snprintf(name.value, sizeof(name.value), "Part %d", index);
    return name;
}

Components::Light MakeLight()
{
    Components::Light light{};
    light.Type = Components::LightType::Point;
    light.Intensity = 100.0f;
    return light;
}

Components::Transform MakeTransform(int index)
{
    return Components::Transform::FromTRS(Mathematics::Vector3{static_cast<float>(index), 0.0f, 0.0f},
                                          Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f});
}

// The H2 baselines' template: kH2PrefabEntities entities with the components and parent links of
// the generated prefab, created in `world` in template order.
std::vector<ECS::EntityHandle> CreateTemplateInWorld(ECS::World& world)
{
    std::vector<ECS::EntityHandle> entities;
    for (int index = 0; index < kH2PrefabEntities; ++index)
    {
        const ECS::EntityHandle entity = world.CreateEntity();
        world.AddComponentImmediate(entity, MakeTransform(index));
        world.AddComponentImmediate(entity, MakeName(index));
        world.AddComponentImmediate(entity, MakeLight());
        world.AddComponentImmediate(entity, Components::PhysicsCollider{});
        if (index > 0)
            world.AddComponentImmediate(entity, Components::Parent{entities[static_cast<std::size_t>(ParentIndex(index))]});
        entities.push_back(entity);
    }
    return entities;
}

// One instance as the H2a baseline makes it: a CloneEntity per template entity, then a Parent set
// on each clone (the root's to no parent).
void CloneInstance(ECS::World& world, const std::vector<ECS::EntityHandle>& prefab, std::vector<ECS::EntityHandle>& clones)
{
    clones.clear();
    for (const ECS::EntityHandle source : prefab)
        clones.push_back(world.CloneEntity(source));
    for (std::size_t index = 0; index < clones.size(); ++index)
    {
        const ECS::EntityHandle parent = index == 0 ? ECS::EntityHandle{} : clones[static_cast<std::size_t>(ParentIndex(static_cast<int>(index)))];
        world.AddComponentImmediate(clones[index], Components::Parent{parent});
    }
}

class PrefabTimingTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const char* json = std::getenv("GE_PREFAB_TIMING_JSON");
        const char* arm = std::getenv("GE_PREFAB_TIMING_ARM");
        if (!json || !arm)
            GTEST_SKIP() << "set GE_PREFAB_TIMING_JSON and GE_PREFAB_TIMING_ARM to run a prefab timing";
        m_JsonPath = json;
        m_Arm = arm;
        m_OriginalWorkingDirectory = std::filesystem::current_path();
        m_Workspace = TestUtils::MakeUniqueTempDirectory("PrefabTimingTests");

        ApplicationConfig config{};
        config.WorkspaceDirectory = m_Workspace.string();
        config.AssetDirectory = "Assets";
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
        ASSERT_TRUE(EngineCore::GetInstance().Initialize(config));
        m_Booted = true;
    }

    void TearDown() override
    {
        m_Resolver.reset();
        if (m_Booted)
            EngineCore::GetInstance().Shutdown();
        std::error_code ec;
        if (!m_OriginalWorkingDirectory.empty())
            std::filesystem::current_path(m_OriginalWorkingDirectory, ec);
        if (!m_Workspace.empty())
            std::filesystem::remove_all(m_Workspace, ec);
    }

    std::filesystem::path Write(const std::string& file, const std::string& text)
    {
        const auto path = (EngineCore::GetInstance().GetResolvedAssetRoot() / file).lexically_normal();
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
        return path;
    }

    // Loads `path` into `world` (Replace). With `resolved`, prefab sources resolve through the
    // engine's asset manager, so the templates carry their GUIDs.
    bool Load(ECS::World& world, const std::filesystem::path& path, bool resolved,
              Scene::SceneLoadDegradation* report = nullptr)
    {
        Scene::LoadOptions options{Scene::LoadMode::Replace};
        options.outDegradation = report;
        if (resolved)
        {
            if (!m_Resolver)
                m_Resolver.emplace(EngineCore::GetInstance().GetAssetManager(),
                                   EngineCore::GetInstance().GetResolvedAssetRoot(),
                                   Scene::AssetManagerSceneResolver::UnknownFile::Register);
            options.assetResolver = &*m_Resolver;
            options.assetRootOverride = m_Resolver->GetAssetRoot();
        }
        return Scene::LoadSceneFromFile(world, path, options);
    }

    GUID Register(const std::filesystem::path& path)
    {
        const auto result = EngineCore::GetInstance().GetAssetManager().GetRegistry().RegisterAssetByPath(path);
        EXPECT_TRUE(result.IsOk()) << path.string();
        return result.IsOk() ? result.Value() : GUID::Null();
    }

    // Writes {"test", "arm", "config", <fields>} to the result file.
    void WriteResult(const std::string& fields)
    {
#ifdef NDEBUG
        constexpr const char* kConfig = "optimized";
#else
        constexpr const char* kConfig = "debug";
#endif
        const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::ofstream out(m_JsonPath, std::ios::binary);
        out << std::format("{{\"test\": \"{}\", \"arm\": \"{}\", \"config\": \"{}\", {}}}\n", test, m_Arm, kConfig, fields);
        ASSERT_TRUE(out.good()) << m_JsonPath;
    }

    // H1: one untimed load, a clear and a frame boundary, then one timed load of the same scene.
    void TimeLoad(const std::filesystem::path& scene)
    {
        ECS::World world;
        ASSERT_TRUE(Load(world, scene, false)) << Scene::GetLastSceneIOError().message;
        world.Clear();
        EngineCore::GetInstance().TryGetPrefabLibrary()->CollectAtFrameBoundary();

        Scene::SceneLoadDegradation report;
        const Clock::time_point start = Clock::now();
        const bool loaded = Load(world, scene, false, &report);
        const double ms = ElapsedMs(start);
        ASSERT_TRUE(loaded) << Scene::GetLastSceneIOError().message;
        EXPECT_EQ(world.GetEntityCount(), static_cast<std::size_t>(kH1Instances * kH1PrefabEntities));
        EXPECT_TRUE(report.skips.empty()) << report.skips.front().message;
        WriteResult(std::format("\"loadMs\": {:.4f}, \"entities\": {}", ms, world.GetEntityCount()));
    }

    // Writes the H2 prefab, registers it and makes it resident through one placed instance in
    // m_Holder. Returns its GUID.
    GUID MakeH2PrefabResident()
    {
        const GUID prefab = Register(Write("Spawned.prefab", PrefabText(kH2PrefabEntities, Parts::WithLightAndCollider)));
        const auto scene = Write("Holder.scene", "[scene name=\"Holder\" version=1]\n[resource id=\"p\" path=\"Spawned.prefab\"]\n"
                                                 "[prefab id=\"held\" source=\"p\"]\n");
        EXPECT_TRUE(Load(m_Holder, scene, true)) << Scene::GetLastSceneIOError().message;
        EXPECT_NE(EngineCore::GetInstance().TryGetPrefabLibrary()->FindByGuid(prefab), nullptr);
        return prefab;
    }

    std::string m_JsonPath;
    std::string m_Arm;
    std::filesystem::path m_OriginalWorkingDirectory;
    std::filesystem::path m_Workspace;
    bool m_Booted = false;
    std::optional<Scene::AssetManagerSceneResolver> m_Resolver;
    ECS::World m_Holder;
};

std::string JoinMs(const std::vector<double>& values)
{
    std::string text;
    for (const double value : values)
        text += std::format("{}{:.4f}", text.empty() ? "" : ", ", value);
    return text;
}

// H1, the candidate: kH1Instances instances of one kH1PrefabEntities-entity prefab.
TEST_F(PrefabTimingTests, DISABLED_H1LoadInstanced)
{
    Write("Part20.prefab", PrefabText(kH1PrefabEntities, Parts::WithLightAndCollider));
    TimeLoad(Write("Instanced.scene", InstancedSceneText("Part20.prefab")));
}

// H1, the baseline: the same scene flattened to plain entity sections.
TEST_F(PrefabTimingTests, DISABLED_H1LoadFlattened)
{
    TimeLoad(Write("Flattened.scene", FlattenedSceneText()));
}

// H2a, the candidate: kH2aBurst spawns of a resident prefab into one world, after a warm-up burst
// into another.
TEST_F(PrefabTimingTests, DISABLED_H2aBurstSpawn)
{
    const GUID prefab = MakeH2PrefabResident();
    ASSERT_FALSE(HasFailure());
    ECS::World warmUp;
    ECS::World world;
    for (ECS::World* target : {&warmUp, &world})
    {
        const Clock::time_point start = Clock::now();
        for (int i = 0; i < kH2aBurst; ++i)
            Scene::SpawnPrefab(*target, prefab, MakeTransform(i), {});
        const double ms = ElapsedMs(start);
        ASSERT_EQ(target->GetEntityCount(), static_cast<std::size_t>(kH2aBurst * kH2PrefabEntities));
        if (target == &world)
            WriteResult(std::format("\"burstMs\": {:.4f}", ms));
    }
}

// H2a, the baseline: the same burst as CloneEntity calls from a template in the target world.
TEST_F(PrefabTimingTests, DISABLED_H2aBurstClone)
{
    ECS::World warmUp;
    ECS::World world;
    std::vector<ECS::EntityHandle> clones;
    for (ECS::World* target : {&warmUp, &world})
    {
        const std::vector<ECS::EntityHandle> prefab = CreateTemplateInWorld(*target);
        const Clock::time_point start = Clock::now();
        for (int i = 0; i < kH2aBurst; ++i)
            CloneInstance(*target, prefab, clones);
        const double ms = ElapsedMs(start);
        ASSERT_EQ(target->GetEntityCount(), static_cast<std::size_t>((kH2aBurst + 1) * kH2PrefabEntities));
        if (target == &world)
            WriteResult(std::format("\"burstMs\": {:.4f}", ms));
    }
}

// H2b, the candidate: kH2bSpawnsPerFrame spawns per frame for kH2bFrames frames.
TEST_F(PrefabTimingTests, DISABLED_H2bRateSpawn)
{
    const GUID prefab = MakeH2PrefabResident();
    ASSERT_FALSE(HasFailure());
    ECS::World world;
    std::vector<double> frames;
    for (int frame = 0; frame < kH2bFrames; ++frame)
    {
        const Clock::time_point start = Clock::now();
        for (int i = 0; i < kH2bSpawnsPerFrame; ++i)
            Scene::SpawnPrefab(world, prefab, MakeTransform(i), {});
        frames.push_back(ElapsedMs(start));
    }
    ASSERT_EQ(world.GetEntityCount(), static_cast<std::size_t>(kH2bFrames * kH2bSpawnsPerFrame * kH2PrefabEntities));
    WriteResult(std::format("\"frameMs\": [{}]", JoinMs(frames)));
}

// H2b, the baseline: the same entities per frame through CreateBatchFromBundle, one bundle per
// template archetype (the root and the children), then a Parent set on every child.
TEST_F(PrefabTimingTests, DISABLED_H2bRateBundle)
{
    ECS::World world;
    ECS::ComponentBundle root;
    root.Add(MakeTransform(0)).Add(MakeName(0)).Add(MakeLight()).Add(Components::PhysicsCollider{});
    ECS::ComponentBundle child;
    child.Add(MakeTransform(1)).Add(MakeName(1)).Add(MakeLight()).Add(Components::PhysicsCollider{}).Add(Components::Parent{});
    constexpr std::size_t kChildren = kH2PrefabEntities - 1;
    std::vector<double> frames;
    for (int frame = 0; frame < kH2bFrames; ++frame)
    {
        const Clock::time_point start = Clock::now();
        const std::vector<ECS::EntityHandle> roots = world.CreateBatchFromBundle(kH2bSpawnsPerFrame, root);
        const std::vector<ECS::EntityHandle> children = world.CreateBatchFromBundle(kH2bSpawnsPerFrame * kChildren, child);
        for (std::size_t instance = 0; instance < roots.size(); ++instance)
        {
            const ECS::EntityHandle* members = &children[instance * kChildren];
            for (std::size_t member = 1; member <= kChildren; ++member)
            {
                const std::size_t parent = static_cast<std::size_t>(ParentIndex(static_cast<int>(member)));
                world.AddComponentImmediate(members[member - 1], Components::Parent{parent == 0 ? roots[instance] : members[parent - 1]});
            }
        }
        frames.push_back(ElapsedMs(start));
    }
    ASSERT_EQ(world.GetEntityCount(), static_cast<std::size_t>(kH2bFrames * kH2bSpawnsPerFrame * kH2PrefabEntities));
    WriteResult(std::format("\"frameMs\": [{}]", JoinMs(frames)));
}

// H5: kH5Prefabs resident prefabs of kH5PrefabEntities entities among three component sets; the
// library world's chunk bytes plus the templates' address tables.
TEST_F(PrefabTimingTests, DISABLED_H5LibraryMemory)
{
    constexpr Parts kSets[] = {Parts::TransformAndName, Parts::WithLight, Parts::WithCollider};
    std::string scene = "[scene name=\"Library\" version=1]\n";
    std::vector<std::filesystem::path> paths;
    for (int prefabIndex = 0; prefabIndex < kH5Prefabs; ++prefabIndex)
    {
        const std::string file = std::format("Library/P{}.prefab", prefabIndex);
        paths.push_back(Write(file, PrefabText(kH5PrefabEntities, kSets[static_cast<std::size_t>(prefabIndex) % std::size(kSets)])));
        scene += std::format("[resource id=\"p{}\" path=\"{}\"]\n", prefabIndex, file);
    }
    for (int prefabIndex = 0; prefabIndex < kH5Prefabs; ++prefabIndex)
        scene += std::format("[prefab id=\"i{}\" source=\"p{}\"]\n", prefabIndex, prefabIndex);
    ECS::World world;
    ASSERT_TRUE(Load(world, Write("Library.scene", scene), false)) << Scene::GetLastSceneIOError().message;

    const Scene::PrefabLibrary& library = *EngineCore::GetInstance().TryGetPrefabLibrary();
    ASSERT_EQ(library.GetResidentCount(), static_cast<std::size_t>(kH5Prefabs));
    std::size_t chunks = 0;
    const std::vector<ECS::Archetype*> archetypes = library.GetWorld().GetAllArchetypes();
    std::size_t usedArchetypes = 0;
    for (const ECS::Archetype* archetype : archetypes)
    {
        chunks += archetype->GetChunkCount();
        usedArchetypes += archetype->GetChunkCount() > 0 ? 1 : 0;
    }
    std::size_t tables = 0;
    for (const std::filesystem::path& path : paths)
    {
        const Scene::PrefabTemplate* prefab = library.FindByPath(path);
        ASSERT_NE(prefab, nullptr) << path.string();
        tables += prefab->Addresses.capacity() * sizeof(std::string);
        for (const std::string& address : prefab->Addresses)
            tables += address.capacity();
        tables += prefab->Entities.capacity() * sizeof(ECS::EntityHandle);
        tables += prefab->Origins.capacity() * sizeof(Scene::PrefabTemplate::Origin);
        tables += prefab->Files.capacity() * sizeof(std::filesystem::path);
    }
    const std::size_t chunkBytes = chunks * ECS::kChunkSize;
    WriteResult(std::format("\"bytes\": {}, \"chunkBytes\": {}, \"chunks\": {}, \"archetypesWithChunks\": {}, "
                            "\"tableBytes\": {}, \"libraryEntities\": {}",
                            chunkBytes + tables, chunkBytes, chunks, usedArchetypes, tables,
                            library.GetWorld().GetEntityCount()));
}

} // namespace
} // namespace GameEngine
