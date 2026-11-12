// The engine's prefab library: what makes a prefab resident, the spawn of one that is not, the
// generations of a rebuild and the cleanup of a failed build. Each test boots a headless engine,
// whose library the scene loads and the spawns use.

#include "EngineLogCapture.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/AssetRef.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/PrefabInstance.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ClonePlan.h"
#include "ECS/Component/ComponentFactory.h"
#include "ECS/Component/ComponentFieldRegistry.h"
#include "ECS/Component/ComponentTypeName.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/Reflection.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/KeptComponentResolver.h"
#include "Scene/PrefabAuthoring.h"
#include "Scene/PrefabLibrary.h"
#include "Scene/PrefabSpawn.h"
#include "Scene/ReflectionSceneSchema.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scripting/ScriptsConfig.h"
#include "Types/StringUtils.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
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

// A component with an entity field, as a trigger that opens a gate would author one.
struct PrefabLinkProbe
{
    GameEngine::ECS::EntityHandle Target;
};
GE_REFLECT(PrefabLinkProbe, Target);

// Components of a module that registers after a scene loads: no test registers them before their test.
// The tests that register them run once per process, because a type registration is process-wide.
struct LateSpawnerProbe
{
    GameEngine::Components::PrefabRef Prefab;
};
GE_REFLECT(LateSpawnerProbe, Prefab);

struct LateMarkerProbe
{
    std::int32_t Value = 0;
};
GE_REFLECT(LateMarkerProbe, Value);

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

// Joins the registries a scene load and save read `Probe` through, under `schemaName`.
template <typename Probe>
void RegisterProbe(const char* schemaName)
{
    const auto type = ECS::GetComponentTypeId<Probe>();
    ECS::ComponentFieldRegistry::Register(type, ECS::Reflection<Probe>::Fields, ECS::ComponentTypeName<Probe>());
    const Probe defaults{};
    ECS::ComponentFactory::RegisterDefaultBytes(type, &defaults, sizeof(defaults), true);
    Scene::SceneSchemaRegistry::Register(std::make_unique<Scene::ReflectionSceneSchema>(type, schemaName));
}

// The registries are process-wide, so the probes join them once.
void EnsureProbeRegistered()
{
    static const bool registered = []
    {
        RegisterProbe<PrefabSpawnerProbe>("PrefabSpawnerProbe");
        RegisterProbe<PrefabLinkProbe>("PrefabLinkProbe");
        return true;
    }();
    ASSERT_TRUE(registered);
}

// Registers `Probe` as a module that loads after a scene does: the registries and the component handler.
template <typename Probe>
void RegisterLateProbe(const char* schemaName)
{
    RegisterProbe<Probe>(schemaName);
    ECS::World registration;
    registration.AddComponentImmediate(registration.CreateEntity(), Probe{});
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
        registration.AddComponentImmediate(registration.CreateEntity(), PrefabLinkProbe{});
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

    Scene::SceneInstantiateResult Load(ECS::World& world, const std::string& file, std::string_view text)
    {
        Scene::SceneReadOptions options{};
        options.AssetResolver = &*m_Resolver;
        options.AssetRootOverride = m_Resolver->GetAssetRoot();
        return Scene::ReadSceneInto(world, Write(file, text), options);
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

    const auto readResult = Load(world, "Barracks.scene",
                     "[scene name=\"Barracks\" version=1]\n"
                     "[entity id=\"spawner\"]\n"
                     "PrefabSpawnerProbe.Prefab = [path=\"Soldier.prefab\" guid=\"" + soldier.ToString() + "\"]\n");
    ASSERT_TRUE(readResult)
        << readResult.Error->message;
    ASSERT_EQ(m_Library->GetResidentCount(), 1u) << Log();

    const std::size_t before = world.GetEntityCount();
    const Components::Transform at = Components::Transform::FromTRS(
        Mathematics::WorldPosition{3.0, 0.0, 0.0}, Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f});
    const ECS::EntityHandle root = Scene::SpawnPrefab(world, soldier, at, {});
    ASSERT_TRUE(root.IsValid());
    EXPECT_EQ(world.GetEntityCount(), before + 2u);
    const auto* marker = world.GetComponent<Components::PrefabInstance>(root);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->Source, soldier);
    const auto* transform = world.GetComponent<Components::Transform>(root);
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(transform->GetPosition().X, 3.0);
}

// The editor's Place: a prefab no scene places yet is read from its file, held for the world, and
// its instance carries scene identities, so a save writes one [prefab] section and no child entity.
TEST_F(PrefabLibraryTests, PlaceReadsAnUnreferencedPrefabAndTheSaveWritesTheInstanceAsAReference)
{
    const GUID soldier = Register(Write("Soldier.prefab", kSoldierPrefab));
    ASSERT_FALSE(soldier.IsNull());

    ECS::World world;
    const ECS::EntityHandle first = Scene::PlacePrefab(world, soldier, Components::Transform{}, {});
    const ECS::EntityHandle second = Scene::PlacePrefab(world, soldier, Components::Transform{}, {});
    ASSERT_TRUE(first.IsValid()) << Log();
    ASSERT_TRUE(second.IsValid()) << Log();
    EXPECT_EQ(world.GetEntityCount(), 4u);
    EXPECT_EQ(m_Library->GetReferenceCount(m_Library->FindByGuid(soldier)->Path), 1u);

    const auto* firstTag = world.GetComponent<Components::SceneEntityTag>(first);
    const auto* secondTag = world.GetComponent<Components::SceneEntityTag>(second);
    ASSERT_NE(firstTag, nullptr);
    ASSERT_NE(secondTag, nullptr);
    EXPECT_EQ(firstTag->View(), "soldier");
    EXPECT_NE(firstTag->View(), secondTag->View()) << "each instance has its own scene identity";
    std::vector<std::string> childTags;
    world.Query<ECS::Read<Components::Parent>, ECS::Read<Components::SceneEntityTag>>().Each(
        [&childTags](ECS::EntityHandle, const Components::Parent&, const Components::SceneEntityTag& tag)
        { childTags.emplace_back(tag.View()); });
    EXPECT_EQ(childTags.size(), 2u);
    EXPECT_NE(std::find(childTags.begin(), childTags.end(), "soldier.rifle"), childTags.end());

    Scene::SceneWriteOptions options;
    options.assetResolver = &*m_Resolver;
    const std::filesystem::path scenePath = EngineCore::GetInstance().GetResolvedAssetRoot() / "Placed.scene";
    const auto writeResult = Scene::WriteSceneFile(world, scenePath, options);
    ASSERT_TRUE(writeResult) << writeResult.Error->message;
    std::ifstream file(scenePath, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("[prefab id=\"soldier\""), std::string::npos) << text;
    EXPECT_EQ(text.find("rifle"), std::string::npos) << "a member is not saved as an entity\n" << text;
}

// Creates an entity named `name` under `parent` (none when invalid) with a Transform.
ECS::EntityHandle CreateNamed(ECS::World& world, std::string_view name, ECS::EntityHandle parent)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::Name component{};
    name.copy(component.value, sizeof(component.value) - 1);
    world.AddComponentImmediate(entity, component);
    world.AddComponentImmediate(entity, Components::Transform{});
    if (parent.IsValid())
        world.AddComponentImmediate(entity, Components::Parent{parent});
    return entity;
}

// The first entity whose parent is `parent`.
ECS::EntityHandle FindChild(ECS::World& world, ECS::EntityHandle parent)
{
    ECS::EntityHandle found{};
    world.Query<ECS::Read<Components::Parent>>().Each(
        [&found, parent](ECS::EntityHandle entity, const Components::Parent& link)
        {
            if (link.Entity == parent)
                found = entity;
        });
    return found;
}

constexpr std::string_view kCratePrefab = "[prefab version=1]\n"
                                          "[entity id=\"root\"]\n"
                                          "Name.value = \"Crate\"\n"
                                          "[entity id=\"lid\" parent=\"root\"]\n"
                                          "Name.value = \"Lid\"\n"
                                          "Transform.position = (0, 1, 0)\n"
                                          "[entity id=\"handle\" parent=\"lid\"]\n"
                                          "Name.value = \"Handle\"\n";

// The description of the entity `entity` in `entities`; a default one, with the failure, when it
// is not there.
Scene::PrefabInstanceEntity FindDescribed(const std::vector<Scene::PrefabInstanceEntity>& entities,
                                          ECS::EntityHandle entity)
{
    const auto it = std::find_if(entities.begin(), entities.end(),
                                 [entity](const Scene::PrefabInstanceEntity& e) { return e.Entity == entity; });
    EXPECT_NE(it, entities.end()) << "entity " << entity.id << " is not described";
    return it == entities.end() ? Scene::PrefabInstanceEntity{} : *it;
}

// Design 4.3: the Hierarchy marks an entity with overrides and an entity added on the instance. A
// placed instance has neither, its root's placement included; a changed child field marks that
// child only; an entity parented under a member is added; a removed child marks its parent.
TEST_F(PrefabLibraryTests, DescribePrefabInstanceReportsOverridesAndAddedEntitiesAsASaveWritesThem)
{
    const GUID crate = Register(Write("Crate.prefab", kCratePrefab));
    ECS::World world;
    const Components::Transform at =
        Components::Transform::FromTRS(Mathematics::WorldPosition{4.0, 0.0, 2.0}, Quaternion::Identity(), Vector3(1.0f, 1.0f, 1.0f));
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, at, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle lid = FindChild(world, root);
    const ECS::EntityHandle handle = FindChild(world, lid);
    ASSERT_TRUE(handle.IsValid());

    std::vector<Scene::PrefabInstanceEntity> entities;
    ASSERT_TRUE(Scene::DescribePrefabInstance(world, root, entities));
    ASSERT_EQ(entities.size(), 3u);
    EXPECT_EQ(entities[0].Entity, root) << "the root first";
    for (const auto& entity : entities)
    {
        EXPECT_FALSE(entity.HasOverrides()) << "a placed instance has no overrides";
        EXPECT_FALSE(entity.AddedOnInstance);
    }

    Components::Name* handleName = world.GetComponentForWrite<Components::Name>(handle);
    ASSERT_NE(handleName, nullptr);
    *handleName = Components::Name{};
    std::string_view("Grip").copy(handleName->value, sizeof(handleName->value) - 1);
    const ECS::EntityHandle sign = world.CreateEntity();
    world.AddComponentImmediate(sign, Components::Parent{lid});
    ASSERT_TRUE(Scene::DescribePrefabInstance(world, root, entities));
    ASSERT_EQ(entities.size(), 4u);
    EXPECT_FALSE(FindDescribed(entities, root).HasOverrides());
    EXPECT_FALSE(FindDescribed(entities, lid).HasOverrides()) << "an added child is no override of its parent";
    EXPECT_TRUE(FindDescribed(entities, handle).HasOverrides());
    EXPECT_TRUE(FindDescribed(entities, sign).AddedOnInstance);
    EXPECT_FALSE(FindDescribed(entities, handle).AddedOnInstance);

    world.DestroyEntityImmediate(handle);
    ASSERT_TRUE(Scene::DescribePrefabInstance(world, root, entities));
    EXPECT_TRUE(FindDescribed(entities, lid).HasOverrides()) << "a removed child is an override of its parent";

    EXPECT_FALSE(Scene::DescribePrefabInstance(world, lid, entities)) << "a member is no instance root";
    EXPECT_TRUE(entities.empty());
}

// Design 4.4: the Inspector marks each changed field, added and removed component and removed child.
// A field of an added component is part of the addition, not a changed field of its own.
TEST_F(PrefabLibraryTests, DescribePrefabInstanceListsEachOverrideAsASaveWritesIt)
{
    const GUID crate = Register(Write("Crate.prefab", kCratePrefab));
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle lid = FindChild(world, root);
    const ECS::EntityHandle handle = FindChild(world, lid);
    ASSERT_TRUE(handle.IsValid());

    Components::Name* lidName = world.GetComponentForWrite<Components::Name>(lid);
    ASSERT_NE(lidName, nullptr);
    *lidName = Components::Name{};
    std::string_view("Top").copy(lidName->value, sizeof(lidName->value) - 1);
    world.RemoveComponentImmediate<Components::Name>(handle);
    world.AddComponentImmediate(handle, Components::Transform{});

    std::vector<Scene::PrefabInstanceEntity> entities;
    ASSERT_TRUE(Scene::DescribePrefabInstance(world, root, entities));
    const std::vector<Scene::PrefabOverride> lidOverrides = FindDescribed(entities, lid).Overrides;
    ASSERT_EQ(lidOverrides.size(), 1u);
    EXPECT_EQ(lidOverrides[0].Kind, Scene::PrefabOverrideKind::Field);
    EXPECT_EQ(lidOverrides[0].Component, "Name");
    EXPECT_EQ(lidOverrides[0].Field, "value");
    const std::vector<Scene::PrefabOverride> handleOverrides = FindDescribed(entities, handle).Overrides;
    ASSERT_EQ(handleOverrides.size(), 2u) << "the added Transform's fields are part of the addition";
    EXPECT_EQ(handleOverrides[0].Kind, Scene::PrefabOverrideKind::AddedComponent);
    EXPECT_EQ(handleOverrides[0].Component, "Transform");
    EXPECT_EQ(handleOverrides[1].Kind, Scene::PrefabOverrideKind::RemovedComponent);
    EXPECT_EQ(handleOverrides[1].Component, "Name");
    EXPECT_TRUE(FindDescribed(entities, root).Overrides.empty()) << "the root's placement is no override";

    world.DestroyEntityImmediate(handle);
    ASSERT_TRUE(Scene::DescribePrefabInstance(world, root, entities));
    const std::vector<Scene::PrefabOverride> afterRemove = FindDescribed(entities, lid).Overrides;
    ASSERT_EQ(afterRemove.size(), 2u);
    EXPECT_EQ(afterRemove[1].Kind, Scene::PrefabOverrideKind::RemovedChild);
    EXPECT_EQ(afterRemove[1].Field, "lid.handle");
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Gives `entity` the name `text`.
void Rename(ECS::World& world, ECS::EntityHandle entity, std::string_view text)
{
    Components::Name* name = world.GetComponentForWrite<Components::Name>(entity);
    ASSERT_NE(name, nullptr);
    *name = Components::Name{};
    text.copy(name->value, sizeof(name->value) - 1);
}

std::string NameOf(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* name = world.GetComponent<Components::Name>(entity);
    return name ? std::string(name->View()) : std::string("(no Name)");
}

// The name the directory of `file` holds for it, matched without case.
std::string NameOnDisk(const std::filesystem::path& file)
{
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(file.parent_path()))
        if (EqualsIgnoreCase(entry.path().filename().string(), file.filename().string()))
            return entry.path().filename().string();
    return std::string();
}

// The overrides DescribePrefabInstance reports on `entity` of the instance at `root`.
std::vector<Scene::PrefabOverride> OverridesOf(ECS::World& world, ECS::EntityHandle root, ECS::EntityHandle entity)
{
    std::vector<Scene::PrefabInstanceEntity> entities;
    EXPECT_TRUE(Scene::DescribePrefabInstance(world, root, entities));
    return FindDescribed(entities, entity).Overrides;
}

// Design 4.4 item 3, 2.6 and 4.11: Apply writes the field into the prefab file, under the file's own
// name, and every other instance in the world shows it; the applier has no override left, and an
// instance with its own override keeps it. Writing the text from before (the undo) gives the
// instances the old value back.
TEST_F(PrefabLibraryTests, AnApplyWritesTheFieldIntoThePrefabAndTheOtherInstancesFollowItAndItsUndo)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    const ECS::EntityHandle first = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    const ECS::EntityHandle second = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    const ECS::EntityHandle third = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(first.IsValid() && second.IsValid() && third.IsValid()) << Log();
    const ECS::EntityHandle firstLid = FindChild(world, first);
    const ECS::EntityHandle secondLid = FindChild(world, second);
    const ECS::EntityHandle thirdLid = FindChild(world, third);
    Rename(world, firstLid, "Top");
    Rename(world, thirdLid, "Mine");
    const std::vector<Scene::PrefabOverride> before = OverridesOf(world, first, firstLid);
    ASSERT_EQ(before.size(), 1u);

    std::vector<Scene::PrefabFileEdit> edits;
    std::string error;
    ASSERT_TRUE(Scene::ApplyPrefabOverrides(world, std::vector<Scene::PrefabOverrideTarget>{{firstLid, before[0]}},
                                            edits, error))
        << error;
    EXPECT_NE(ReadText(file).find("Name.value = \"Top\""), std::string::npos) << ReadText(file);
    EXPECT_EQ(ReadText(file).find("Name.value = \"Lid\""), std::string::npos) << ReadText(file);
    EXPECT_EQ(NameOnDisk(file), "Crate.prefab");
    EXPECT_EQ(NameOf(world, secondLid), "Top");
    EXPECT_EQ(NameOf(world, thirdLid), "Mine");
    EXPECT_EQ(OverridesOf(world, third, thirdLid).size(), 1u);
    EXPECT_TRUE(OverridesOf(world, first, firstLid).empty());
    EXPECT_TRUE(OverridesOf(world, second, secondLid).empty());

    ASSERT_TRUE(Scene::WritePrefabFileText(edits.at(0).File, edits.at(0).Before, error)) << error;
    EXPECT_EQ(ReadText(file), std::string(kCratePrefab));
    EXPECT_EQ(NameOnDisk(file), "Crate.prefab");
    EXPECT_EQ(NameOf(world, secondLid), "Lid");
    EXPECT_EQ(NameOf(world, thirdLid), "Mine");
    // The applier holds the prefab's value, so it follows too; the editor's Apply step restores it.
    EXPECT_EQ(NameOf(world, firstLid), "Lid");
}

// Design 4.4 item 3: Revert gives the field the prefab's value and the override is gone; a stale
// override is refused with the design's words, and nothing changes.
TEST_F(PrefabLibraryTests, ARevertGivesTheFieldThePrefabValueAndAStaleOverrideIsRefused)
{
    const GUID crate = Register(Write("Crate.prefab", kCratePrefab));
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle lid = FindChild(world, root);
    Rename(world, lid, "Top");

    std::string error;
    const Scene::PrefabOverride stale{Scene::PrefabOverrideKind::Field, "Name", "nickname"};
    EXPECT_FALSE(Scene::RevertPrefabOverride(world, lid, stale, error));
    EXPECT_EQ(error, "Name Nickname is not in the prefab.");
    EXPECT_EQ(NameOf(world, lid), "Top");

    const std::vector<Scene::PrefabOverride> overrides = OverridesOf(world, root, lid);
    ASSERT_EQ(overrides.size(), 1u);
    ASSERT_TRUE(Scene::RevertPrefabOverride(world, lid, overrides[0], error)) << error;
    EXPECT_EQ(NameOf(world, lid), "Lid");
    EXPECT_TRUE(OverridesOf(world, root, lid).empty());
}

// Design 2.2: every prefab entity has a name, so the removal of Name does not apply to the prefab.
TEST_F(PrefabLibraryTests, AnApplyOfARemovedNameIsRefusedAndTheFileStaysAsItWas)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle lid = FindChild(world, root);
    world.RemoveComponentImmediate<Components::Name>(lid);
    const std::vector<Scene::PrefabOverride> overrides = OverridesOf(world, root, lid);
    ASSERT_EQ(overrides.size(), 1u);
    ASSERT_EQ(overrides[0].Kind, Scene::PrefabOverrideKind::RemovedComponent);

    std::vector<Scene::PrefabFileEdit> edits;
    std::string error;
    EXPECT_FALSE(
        Scene::ApplyPrefabOverrides(world, std::vector<Scene::PrefabOverrideTarget>{{lid, overrides[0]}}, edits, error));
    EXPECT_EQ(error, "Every prefab entity has a name, so the prefab cannot remove Name. Use Revert to restore the name.");
    EXPECT_EQ(ReadText(file), std::string(kCratePrefab));
}

// Design 4.4 item 4: an added component's Apply adds it to the prefab and every instance; a removed
// component's Revert brings it back with the prefab's values, and its Apply removes it in the prefab
// and every instance.
TEST_F(PrefabLibraryTests, AddedAndRemovedComponentsApplyToThePrefabAndRevertOnTheInstance)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    const ECS::EntityHandle first = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    const ECS::EntityHandle second = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(first.IsValid() && second.IsValid()) << Log();
    const ECS::EntityHandle firstHandle = FindChild(world, FindChild(world, first));
    const ECS::EntityHandle secondHandle = FindChild(world, FindChild(world, second));
    ASSERT_FALSE(world.HasComponent<Components::Transform>(secondHandle));

    world.AddComponentImmediate(firstHandle, Components::Transform{});
    std::vector<Scene::PrefabOverride> overrides = OverridesOf(world, first, firstHandle);
    ASSERT_EQ(overrides.size(), 1u);
    ASSERT_EQ(overrides[0].Kind, Scene::PrefabOverrideKind::AddedComponent);
    std::vector<Scene::PrefabFileEdit> edits;
    std::string error;
    ASSERT_TRUE(Scene::ApplyPrefabOverrides(world, std::vector<Scene::PrefabOverrideTarget>{{firstHandle, overrides[0]}},
                                            edits, error))
        << error;
    EXPECT_TRUE(world.HasComponent<Components::Transform>(secondHandle));
    EXPECT_TRUE(OverridesOf(world, first, firstHandle).empty());
    EXPECT_TRUE(OverridesOf(world, second, secondHandle).empty());

    world.RemoveComponentImmediate<Components::Transform>(firstHandle);
    overrides = OverridesOf(world, first, firstHandle);
    ASSERT_EQ(overrides.size(), 1u);
    ASSERT_EQ(overrides[0].Kind, Scene::PrefabOverrideKind::RemovedComponent);
    ASSERT_TRUE(Scene::RevertPrefabOverride(world, firstHandle, overrides[0], error)) << error;
    EXPECT_TRUE(world.HasComponent<Components::Transform>(firstHandle));
    EXPECT_TRUE(OverridesOf(world, first, firstHandle).empty());

    world.RemoveComponentImmediate<Components::Transform>(firstHandle);
    const std::vector<Scene::PrefabOverrideTarget> targets = {{firstHandle, OverridesOf(world, first, firstHandle)[0]}};
    ASSERT_TRUE(Scene::ApplyPrefabOverrides(world, targets, edits, error)) << error;
    EXPECT_EQ(ReadText(file), std::string(kCratePrefab));
    EXPECT_FALSE(world.HasComponent<Components::Transform>(secondHandle));
    EXPECT_TRUE(OverridesOf(world, second, secondHandle).empty());
}

// Design 4.4 item 5: Apply all writes the prefab file once and rebuilds it once. The lid's removed Transform
// takes a line out of the section above the handle's, so the handle's section still has to be found.
TEST_F(PrefabLibraryTests, ApplyingOverridesOfTwoEntitiesWritesTheirPrefabFileOnce)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    const ECS::EntityHandle first = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    const ECS::EntityHandle second = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(first.IsValid() && second.IsValid()) << Log();
    const ECS::EntityHandle firstLid = FindChild(world, first);
    const ECS::EntityHandle firstHandle = FindChild(world, firstLid);
    const ECS::EntityHandle secondLid = FindChild(world, second);
    const ECS::EntityHandle secondHandle = FindChild(world, secondLid);
    Rename(world, firstLid, "Top");
    world.RemoveComponentImmediate<Components::Transform>(firstLid);
    Rename(world, firstHandle, "Grip");
    std::vector<Scene::PrefabOverrideTarget> targets;
    for (const ECS::EntityHandle entity : {firstLid, firstHandle})
        for (const Scene::PrefabOverride& prefabOverride : OverridesOf(world, first, entity))
            targets.push_back({entity, prefabOverride});
    ASSERT_EQ(targets.size(), 3u);
    const std::uint64_t generationBefore = m_Library->FindByGuid(crate)->Generation;

    std::vector<Scene::PrefabFileEdit> edits;
    std::string error;
    ASSERT_TRUE(Scene::ApplyPrefabOverrides(world, targets, edits, error)) << error;
    ASSERT_EQ(edits.size(), 1u);
    EXPECT_EQ(edits[0].Before, std::string(kCratePrefab));
    EXPECT_EQ(m_Library->FindByGuid(crate)->Generation, generationBefore + 1);
    EXPECT_EQ(NameOf(world, secondLid), "Top");
    EXPECT_EQ(NameOf(world, secondHandle), "Grip");
    EXPECT_FALSE(world.HasComponent<Components::Transform>(secondLid));
    EXPECT_TRUE(OverridesOf(world, first, firstLid).empty());
    EXPECT_TRUE(OverridesOf(world, first, firstHandle).empty());
}

// Design 4.2: Create Prefab writes the subtree as a file (an outgoing entity field cleared, an inner
// instance kept) and makes the subtree an instance in place, with the same handles; a save writes one
// [prefab] section with the outgoing field as its override.
TEST_F(PrefabLibraryTests, CreatePrefabWritesTheSubtreeAndMakesItAnInstanceInPlace)
{
    const GUID soldier = Register(Write("Soldier.prefab", kSoldierPrefab));
    ECS::World world;
    const ECS::EntityHandle gate = CreateNamed(world, "Gate", {});
    const ECS::EntityHandle crate = CreateNamed(world, "Crate", {});
    world.AddComponentImmediate(crate, Components::Transform::FromTRS(Mathematics::WorldPosition{5.0, 0.0, 0.0},
                                                                      Mathematics::Quaternion{},
                                                                      Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
    const ECS::EntityHandle lid = CreateNamed(world, "Lid", crate);
    const ECS::EntityHandle trigger = CreateNamed(world, "Trigger", lid);
    world.AddComponentImmediate(trigger, PrefabLinkProbe{gate});
    const ECS::EntityHandle guard = Scene::PlacePrefab(world, soldier, Components::Transform{}, crate);
    ASSERT_TRUE(guard.IsValid()) << Log();
    const ECS::EntityHandle rifle = FindChild(world, guard);
    ASSERT_TRUE(rifle.IsValid());
    const std::size_t entityCount = world.GetEntityCount();

    const std::filesystem::path props = EngineCore::GetInstance().GetResolvedAssetRoot() / "Props";
    std::filesystem::create_directories(props);
    EXPECT_EQ(Scene::CountPrefabEntities(world, crate), 5u);
    const std::vector<Scene::OutgoingReference> outgoing = Scene::CollectOutgoingReferences(world, crate);
    ASSERT_EQ(outgoing.size(), 1u) << "the trigger names the gate";
    EXPECT_EQ(outgoing.front().Entity, trigger);
    EXPECT_EQ(outgoing.front().Component, ECS::GetComponentTypeId<PrefabLinkProbe>());
    EXPECT_EQ(outgoing.front().Field, "Target");
    EXPECT_EQ(outgoing.front().Target, gate);
    EXPECT_TRUE(Scene::CollectOutgoingReferences(world, gate).empty());
    const std::string rifleTagBefore(world.GetComponent<Components::SceneEntityTag>(rifle)->View());
    std::string error;
    const Scene::CreatedPrefab createdPrefab = Scene::CreatePrefab(world, crate, props / "Crate.prefab", error);
    const GUID created = createdPrefab.Prefab;
    ASSERT_FALSE(created.IsNull()) << error << "\n" << Log();
    ASSERT_EQ(createdPrefab.Members.size(), 5u);
    EXPECT_EQ(createdPrefab.Members.front().Entity, crate);
    EXPECT_FALSE(createdPrefab.Members.front().Tag.has_value()) << "the root had no scene identity";
    const auto rifleMember = std::find_if(createdPrefab.Members.begin(), createdPrefab.Members.end(),
                                          [rifle](const Scene::PrefabIdentityBefore& member)
                                          { return member.Entity == rifle; });
    ASSERT_NE(rifleMember, createdPrefab.Members.end());
    ASSERT_TRUE(rifleMember->Tag.has_value());
    EXPECT_EQ(rifleMember->Tag->View(), rifleTagBefore) << "a member keeps its identity from before";

    const std::string prefabText = ReadText(props / "Crate.prefab");
    EXPECT_EQ(prefabText.rfind("[prefab version=1]", 0), 0u) << prefabText;
    EXPECT_NE(prefabText.find("[entity id=\"root\""), std::string::npos) << prefabText;
    EXPECT_NE(prefabText.find("[entity id=\"trigger\" parent=\"lid\""), std::string::npos) << prefabText;
    EXPECT_NE(prefabText.find("[prefab id=\"soldier\""), std::string::npos) << prefabText;
    EXPECT_EQ(prefabText.find("gate"), std::string::npos) << "the outgoing field is cleared\n" << prefabText;
    EXPECT_EQ(prefabText.find("5.0"), std::string::npos) << "the root is at the identity\n" << prefabText;

    EXPECT_EQ(world.GetEntityCount(), entityCount);
    const auto* marker = world.GetComponent<Components::PrefabInstance>(crate);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->Source, created);
    EXPECT_EQ(world.GetComponent<Components::PrefabInstance>(guard)->Source, soldier);
    EXPECT_EQ(world.GetComponent<PrefabLinkProbe>(trigger)->Target, gate) << "the instance keeps the field";
    EXPECT_EQ(world.GetComponent<Components::SceneEntityTag>(crate)->View(), "crate");
    EXPECT_EQ(world.GetComponent<Components::SceneEntityTag>(trigger)->View(), "crate.lid.trigger");
    EXPECT_EQ(world.GetComponent<Components::SceneEntityTag>(rifle)->View(), "crate.soldier.rifle");

    std::string refused;
    EXPECT_TRUE(Scene::CreatePrefab(world, crate, props / "Again.prefab", refused).Prefab.IsNull());
    EXPECT_NE(refused.find("already a prefab instance"), std::string::npos) << refused;
    EXPECT_TRUE(Scene::CreatePrefab(world, lid, props / "Lid.prefab", refused).Prefab.IsNull());
    EXPECT_NE(refused.find("inside the prefab instance \"Crate\""), std::string::npos) << refused;

    Scene::SceneWriteOptions options;
    options.assetResolver = &*m_Resolver;
    const std::filesystem::path scenePath = EngineCore::GetInstance().GetResolvedAssetRoot() / "Yard.scene";
    const auto writeResult = Scene::WriteSceneFile(world, scenePath, options);
    ASSERT_TRUE(writeResult) << writeResult.Error->message;
    const std::string sceneText = ReadText(scenePath);
    EXPECT_NE(sceneText.find("[prefab id=\"crate\""), std::string::npos) << sceneText;
    EXPECT_EQ(sceneText.find("[entity id=\"lid"), std::string::npos) << "a member is not an entity\n" << sceneText;
    EXPECT_NE(sceneText.find("PrefabLinkProbe.Target"), std::string::npos) << "the field is an override\n"
                                                                            << sceneText;

    ECS::World reloaded;
    Scene::SceneReadOptions loadOptions{};
    loadOptions.AssetResolver = &*m_Resolver;
    const auto readResult = Scene::ReadSceneInto(reloaded, scenePath, loadOptions);
    ASSERT_TRUE(readResult) << readResult.Error->message;
    EXPECT_EQ(reloaded.GetEntityCount(), entityCount);
}

constexpr std::string_view kCartPrefab = "[prefab version=1]\n"
                                         "[resource id=\"crate\" path=\"Crate.prefab\"]\n"
                                         "[entity id=\"root\"]\n"
                                         "Name.value = \"Cart\"\n"
                                         "[entity id=\"axle\" parent=\"root\"]\n"
                                         "Name.value = \"Axle\"\n"
                                         "[prefab id=\"load\" source=\"crate\" parent=\"root\"]\n";

// The scene identity of `entity`, empty when it has none.
std::string TagOf(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* tag = world.GetComponent<Components::SceneEntityTag>(entity);
    return tag ? std::string(tag->View()) : std::string();
}

// The child of `parent` named `name`.
ECS::EntityHandle FindChildNamed(ECS::World& world, ECS::EntityHandle parent, std::string_view name)
{
    ECS::EntityHandle found{};
    world.Query<ECS::Read<Components::Parent>, ECS::Read<Components::Name>>().Each(
        [&found, parent, name](ECS::EntityHandle entity, const Components::Parent& link, const Components::Name& entityName)
        {
            if (link.Entity == parent && entityName.View() == name)
                found = entity;
        });
    return found;
}

// A copy of the subtree under `root`, as the editor's Duplicate makes it: every component copied, the
// scene identities included, each copy under the copy of its parent.
ECS::EntityHandle CopySubtree(ECS::World& world, ECS::EntityHandle root)
{
    std::vector<std::pair<ECS::EntityHandle, ECS::EntityHandle>> queue{{root, {}}};
    ECS::EntityHandle copyRoot{};
    for (std::size_t index = 0; index < queue.size(); ++index)
    {
        const auto [original, copyParent] = queue[index];
        const ECS::EntityHandle copy = ECS::Entity(&world, original).Clone().GetHandle();
        if (copyParent.IsValid())
            world.AddComponentImmediate(copy, Components::Parent{copyParent});
        else
            copyRoot = copy;
        world.Query<ECS::Read<Components::Parent>>().Each(
            [&queue, original, copy](ECS::EntityHandle child, const Components::Parent& link)
            {
                if (link.Entity == original)
                    queue.emplace_back(child, copy);
            });
    }
    return copyRoot;
}

// Design 4.8: Unpack makes the instance plain entities with the values it shows. The root keeps its
// identity, every member gets a new plain one, and a save writes entities and no [prefab] section.
// RestorePrefabIdentities, the undo, gives the marker and the member identities back.
TEST_F(PrefabLibraryTests, UnpackGivesEveryMemberANewIdentityAndKeepsTheValues)
{
    const GUID crate = Register(Write("Crate.prefab", kCratePrefab));
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle lid = FindChild(world, root);
    const ECS::EntityHandle handle = FindChild(world, lid);
    ASSERT_TRUE(handle.IsValid());
    const Components::Transform moved =
        Components::Transform::FromTRS(Mathematics::WorldPosition{0.0, 3.0, 0.0}, Quaternion::Identity(), Vector3(1.0f, 1.0f, 1.0f));
    world.AddComponentImmediate(lid, moved);
    const std::string lidTag = TagOf(world, lid);
    const std::string handleTag = TagOf(world, handle);
    EXPECT_EQ(lidTag, "crate.lid");
    const std::size_t entityCount = world.GetEntityCount();

    std::string error;
    const std::vector<Scene::PrefabIdentityBefore> before =
        Scene::UnpackPrefabInstance(world, root, Scene::PrefabUnpackDepth::Outer, error);
    ASSERT_EQ(before.size(), 3u) << error;
    EXPECT_EQ(before.front().Entity, root);
    EXPECT_EQ(world.GetComponent<Components::PrefabInstance>(root), nullptr);
    EXPECT_EQ(TagOf(world, root), "crate") << "the root keeps its scene identity";
    EXPECT_EQ(TagOf(world, lid), "lid");
    EXPECT_EQ(TagOf(world, handle), "handle");
    EXPECT_EQ(world.GetEntityCount(), entityCount);
    EXPECT_DOUBLE_EQ(world.GetComponent<Components::Transform>(lid)->GetPosition().Y, 3.0) << "the values stay";
    EXPECT_EQ(world.GetComponent<Components::Name>(handle)->View(), "Handle");
    EXPECT_EQ(world.GetComponent<Components::Parent>(handle)->Entity, lid);

    Scene::SceneWriteOptions options;
    options.assetResolver = &*m_Resolver;
    const std::filesystem::path scenePath = EngineCore::GetInstance().GetResolvedAssetRoot() / "Unpacked.scene";
    const auto writeResult = Scene::WriteSceneFile(world, scenePath, options);
    ASSERT_TRUE(writeResult) << writeResult.Error->message;
    const std::string sceneText = ReadText(scenePath);
    EXPECT_EQ(sceneText.find("[prefab id="), std::string::npos) << sceneText;
    EXPECT_NE(sceneText.find("[entity id=\"handle\" parent=\"lid\""), std::string::npos) << sceneText;

    Scene::RestorePrefabIdentities(world, before);
    const auto* marker = world.GetComponent<Components::PrefabInstance>(root);
    ASSERT_NE(marker, nullptr) << "the undo gives the marker back";
    EXPECT_EQ(marker->Source, crate);
    EXPECT_EQ(TagOf(world, lid), lidTag);
    EXPECT_EQ(TagOf(world, handle), handleTag);
    std::vector<Scene::PrefabInstanceEntity> described;
    ASSERT_TRUE(Scene::DescribePrefabInstance(world, root, described));
    EXPECT_EQ(described.size(), 3u);
    EXPECT_TRUE(FindDescribed(described, lid).HasOverrides()) << "the moved lid is an override again";
}

// Design 4.8: Unpack keeps an instance inside the unpacked one an instance, with a new identity and its
// members under it; Unpack Completely makes it plain too. An entity inside an instance is refused.
TEST_F(PrefabLibraryTests, UnpackKeepsANestedInstanceAndUnpackCompletelyMakesItPlain)
{
    Register(Write("Crate.prefab", kCratePrefab));
    const GUID cart = Register(Write("Cart.prefab", kCartPrefab));
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, cart, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle load = FindChildNamed(world, root, "Crate");
    ASSERT_TRUE(load.IsValid());
    const ECS::EntityHandle lid = FindChild(world, load);
    ASSERT_TRUE(lid.IsValid());
    EXPECT_EQ(TagOf(world, load), "cart.load");
    EXPECT_EQ(TagOf(world, lid), "cart.load.lid");

    std::string error;
    EXPECT_TRUE(Scene::UnpackPrefabInstance(world, load, Scene::PrefabUnpackDepth::Outer, error).empty());
    EXPECT_NE(error.find("inside the prefab instance \"Cart\""), std::string::npos) << error;
    EXPECT_TRUE(Scene::UnpackPrefabInstance(world, lid, Scene::PrefabUnpackDepth::Outer, error).empty());
    EXPECT_NE(error.find("is not a prefab instance"), std::string::npos) << error;

    const std::vector<Scene::PrefabIdentityBefore> before =
        Scene::UnpackPrefabInstance(world, root, Scene::PrefabUnpackDepth::Outer, error);
    ASSERT_FALSE(before.empty()) << error;
    EXPECT_EQ(world.GetComponent<Components::PrefabInstance>(root), nullptr);
    ASSERT_NE(world.GetComponent<Components::PrefabInstance>(load), nullptr) << "the inner instance stays";
    EXPECT_EQ(TagOf(world, load), "crate");
    EXPECT_EQ(TagOf(world, lid), "crate.lid");
    std::vector<Scene::PrefabInstanceEntity> described;
    ASSERT_TRUE(Scene::DescribePrefabInstance(world, load, described));
    EXPECT_EQ(described.size(), 3u);
    for (const Scene::PrefabInstanceEntity& entity : described)
        EXPECT_FALSE(entity.HasOverrides() || entity.AddedOnInstance);

    Scene::SceneWriteOptions options;
    options.assetResolver = &*m_Resolver;
    const std::filesystem::path scenePath = EngineCore::GetInstance().GetResolvedAssetRoot() / "Cart.scene";
    const auto writeResult = Scene::WriteSceneFile(world, scenePath, options);
    ASSERT_TRUE(writeResult) << writeResult.Error->message;
    const std::string sceneText = ReadText(scenePath);
    EXPECT_NE(sceneText.find("[prefab id=\"crate\""), std::string::npos) << sceneText;
    EXPECT_NE(sceneText.find("[entity id=\"axle\""), std::string::npos) << sceneText;

    Scene::RestorePrefabIdentities(world, before);
    EXPECT_EQ(TagOf(world, load), "cart.load");
    EXPECT_EQ(TagOf(world, lid), "cart.load.lid");
    ASSERT_NE(world.GetComponent<Components::PrefabInstance>(root), nullptr);

    ASSERT_FALSE(Scene::UnpackPrefabInstance(world, root, Scene::PrefabUnpackDepth::Completely, error).empty())
        << error;
    std::size_t markers = 0;
    world.Query<ECS::Read<Components::PrefabInstance>>().Each([&markers](ECS::EntityHandle, const Components::PrefabInstance&) { ++markers; });
    EXPECT_EQ(markers, 0u);
    EXPECT_EQ(TagOf(world, load), "crate");
    EXPECT_EQ(TagOf(world, lid), "lid");
}

// Design 4.3: a duplicate of an instance is an instance of the same prefab with the same overrides and
// identities of its own; a save writes two [prefab] sections and the reload has both trees once.
TEST_F(PrefabLibraryTests, ACopyOfAnInstanceIsAnInstanceWithItsOverridesAndIdentitiesOfItsOwn)
{
    const GUID crate = Register(Write("Crate.prefab", kCratePrefab));
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle handle = FindChild(world, FindChild(world, root));
    Components::Name grip{};
    std::string_view("Grip").copy(grip.value, sizeof(grip.value) - 1);
    world.AddComponentImmediate(handle, grip);
    const std::size_t entityCount = world.GetEntityCount();

    const ECS::EntityHandle copy = CopySubtree(world, root);
    Components::Name copyName{};
    std::string_view("Crate (1)").copy(copyName.value, sizeof(copyName.value) - 1);
    world.AddComponentImmediate(copy, copyName);
    Scene::GiveCopySceneIdentities(world, std::span(&copy, 1));
    const ECS::EntityHandle copyLid = FindChild(world, copy);
    const ECS::EntityHandle copyHandle = FindChild(world, copyLid);
    ASSERT_TRUE(copyHandle.IsValid());
    const auto* marker = world.GetComponent<Components::PrefabInstance>(copy);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->Source, crate);
    const std::string copyId = TagOf(world, copy);
    EXPECT_FALSE(copyId.empty());
    EXPECT_NE(copyId, "crate");
    EXPECT_EQ(TagOf(world, copyLid), copyId + ".lid");
    EXPECT_EQ(TagOf(world, copyHandle), copyId + ".lid.handle");
    EXPECT_EQ(TagOf(world, root), "crate") << "the original keeps its identities";

    std::vector<Scene::PrefabInstanceEntity> described;
    ASSERT_TRUE(Scene::DescribePrefabInstance(world, copy, described));
    ASSERT_EQ(described.size(), 3u);
    EXPECT_TRUE(FindDescribed(described, copyHandle).HasOverrides()) << "the copy keeps the override";
    EXPECT_FALSE(FindDescribed(described, copyHandle).AddedOnInstance);

    Scene::SceneWriteOptions options;
    options.assetResolver = &*m_Resolver;
    const std::filesystem::path scenePath = EngineCore::GetInstance().GetResolvedAssetRoot() / "Copies.scene";
    const auto writeResult = Scene::WriteSceneFile(world, scenePath, options);
    ASSERT_TRUE(writeResult) << writeResult.Error->message;
    const std::string sceneText = ReadText(scenePath);
    EXPECT_NE(sceneText.find("[prefab id=\"" + copyId + "\""), std::string::npos) << sceneText;
    EXPECT_EQ(sceneText.find("[entity"), std::string::npos) << "no member is written as an entity\n" << sceneText;

    ECS::World reloaded;
    Scene::SceneReadOptions loadOptions{};
    loadOptions.AssetResolver = &*m_Resolver;
    const auto readResult = Scene::ReadSceneInto(reloaded, scenePath, loadOptions);
    ASSERT_TRUE(readResult) << readResult.Error->message;
    EXPECT_EQ(reloaded.GetEntityCount(), 2 * entityCount);
}

// A Create Prefab at the path of a removed prefab file rebuilds the template from the new subtree
// under the same GUID, as the editor's Replace and a redo do; an existing file is refused.
TEST_F(PrefabLibraryTests, CreatePrefabAtThePathOfARemovedFileReplacesTheTemplate)
{
    ECS::World world;
    const ECS::EntityHandle crate = CreateNamed(world, "Crate", {});
    const ECS::EntityHandle barrel = CreateNamed(world, "Barrel", {});
    CreateNamed(world, "Hoop", barrel);
    CreateNamed(world, "Lid", barrel);

    const std::filesystem::path prefabPath = EngineCore::GetInstance().GetResolvedAssetRoot() / "Container.prefab";
    std::string error;
    const GUID first = Scene::CreatePrefab(world, crate, prefabPath, error).Prefab;
    ASSERT_FALSE(first.IsNull()) << error;
    EXPECT_EQ(m_Library->FindByGuid(first)->Addresses.size(), 1u);
    EXPECT_TRUE(Scene::CreatePrefab(world, barrel, prefabPath, error).Prefab.IsNull());
    EXPECT_NE(error.find("already exists"), std::string::npos) << error;

    std::filesystem::remove(prefabPath);
    const GUID second = Scene::CreatePrefab(world, barrel, prefabPath, error).Prefab;
    ASSERT_EQ(second, first) << error;
    const auto* rebuilt = m_Library->FindByGuid(second);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_EQ(rebuilt->Addresses.size(), 3u);
    EXPECT_NE(std::find(rebuilt->Addresses.begin(), rebuilt->Addresses.end(), "hoop"), rebuilt->Addresses.end());
}

// The library keeps one template per file whatever the path's spelling: a Replace at a placed file
// rebuilds that template, so a save and a reload place the new content.
TEST_F(PrefabLibraryTests, AReplaceOfAPlacedFileKeepsOneTemplateAndTheReloadPlacesTheNewContent)
{
    const std::filesystem::path cratePath = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(cratePath);
    ECS::World world;
    ASSERT_TRUE(Scene::PlacePrefab(world, crate, Components::Transform{}, {}).IsValid()) << Log();
    const ECS::EntityHandle barrel = CreateNamed(world, "Barrel", {});
    CreateNamed(world, "Hoop", barrel);
    CreateNamed(world, "Lid", barrel);
    std::filesystem::remove(cratePath);
    std::string error;
    ASSERT_EQ(Scene::CreatePrefab(world, barrel, cratePath, error).Prefab, crate) << error;
    EXPECT_EQ(m_Library->GetResidentCount(), 1u);

    Scene::SceneWriteOptions options;
    options.assetResolver = &*m_Resolver;
    const std::filesystem::path scenePath = EngineCore::GetInstance().GetResolvedAssetRoot() / "Replaced.scene";
    const auto writeResult = Scene::WriteSceneFile(world, scenePath, options);
    ASSERT_TRUE(writeResult) << writeResult.Error->message;
    ECS::World reloaded;
    Scene::SceneReadOptions loadOptions{};
    loadOptions.AssetResolver = &*m_Resolver;
    const auto readResult = Scene::ReadSceneInto(reloaded, scenePath, loadOptions);
    ASSERT_TRUE(readResult) << readResult.Error->message;
    EXPECT_EQ(reloaded.GetEntityCount(), world.GetEntityCount());
    std::size_t hoops = 0;
    reloaded.Query<ECS::Read<Components::Name>>().Each(
        [&hoops](ECS::EntityHandle, const Components::Name& name) { hoops += name.View() == "Hoop" ? 1u : 0u; });
    EXPECT_EQ(hoops, 1u) << "the reload places the replaced content";
}

// A Create keeps the prefabs that the new instance's PrefabRef fields name, as a scene load does, so
// a game can spawn them at once.
TEST_F(PrefabLibraryTests, CreatePrefabOfASpawnerKeepsThePrefabItNamesSoASpawnSucceeds)
{
    const GUID soldier = Register(Write("Soldier.prefab", kSoldierPrefab));
    ECS::World world;
    const ECS::EntityHandle spawner = CreateNamed(world, "Spawner", {});
    const ECS::EntityHandle post = CreateNamed(world, "Post", spawner);
    PrefabSpawnerProbe probe{};
    probe.Prefab = Components::PrefabRef(soldier);
    world.AddComponentImmediate(post, probe);

    const std::filesystem::path path = EngineCore::GetInstance().GetResolvedAssetRoot() / "Spawner.prefab";
    std::string error;
    ASSERT_FALSE(Scene::CreatePrefab(world, spawner, path, error).Prefab.IsNull()) << error;
    const std::size_t before = world.GetEntityCount();
    EXPECT_TRUE(Scene::SpawnPrefab(world, soldier, Components::Transform{}, {}).IsValid()) << Log();
    EXPECT_EQ(world.GetEntityCount(), before + 2u);
    EXPECT_EQ(CountLines(kNotResidentMessage), 0u) << Log();
}

// A component type that registers after the scene loads: the apply of its kept component holds the
// prefab that its PrefabRef field names, as the load does, so a spawn succeeds.
TEST_F(PrefabLibraryTests, AKeptSpawnerAppliedAfterItsTypeRegistersHoldsThePrefabItNames)
{
    const GUID soldier = Register(Write("Soldier.prefab", kSoldierPrefab));
    ECS::World world;
    const auto readResult = Load(world, "LateBarracks.scene",
                                 "[scene name=\"LateBarracks\" version=1]\n"
                                 "[entity id=\"spawner\"]\n"
                                 "LateSpawnerProbe.Prefab = [path=\"Soldier.prefab\" guid=\"" +
                                     soldier.ToString() + "\"]\n");
    ASSERT_TRUE(readResult) << readResult.Error->message;
    ASSERT_EQ(m_Library->FindByGuid(soldier), nullptr) << "the load keeps the component of an unknown type";

    RegisterLateProbe<LateSpawnerProbe>("LateSpawnerProbe");
    EXPECT_EQ(Scene::ResolveKeptComponents(world), 1u) << Log();
    EXPECT_NE(m_Library->FindByGuid(soldier), nullptr) << Log();
    const std::size_t before = world.GetEntityCount();
    EXPECT_TRUE(Scene::SpawnPrefab(world, soldier, Components::Transform{}, {}).IsValid()) << Log();
    EXPECT_EQ(world.GetEntityCount(), before + 2u);
    EXPECT_EQ(CountLines(kNotResidentMessage), 0u) << Log();
}

// A template built while a component type of its prefab is not registered keeps that component
// aside; after the type registers, the next load of the scene places the instance with the component.
// The placed instance moves to the rebuilt template, so no retired template stays after the frame boundary.
TEST_F(PrefabLibraryTests, ATemplateBuiltBeforeAComponentTypeRegistersPlacesTheComponentAfterIt)
{
    const GUID outpost =
        Register(Write("Outpost.prefab", "[prefab version=1]\n[entity id=\"root\"]\nLateMarkerProbe.Value = 7\n"));
    constexpr std::string_view kFrontier = "[scene name=\"Frontier\" version=1]\n"
                                           "[resource id=\"outpost\" path=\"Outpost.prefab\"]\n"
                                           "[prefab id=\"outpost\" source=\"outpost\"]\n";
    ECS::World world;
    const auto first = Load(world, "Frontier.scene", kFrontier);
    ASSERT_TRUE(first) << first.Error->message;

    RegisterLateProbe<LateMarkerProbe>("LateMarkerProbe");
    EXPECT_EQ(Scene::ResolveKeptComponents(world), 1u) << Log();
    const Scene::PrefabTemplate* rebuilt = m_Library->FindByGuid(outpost);
    EXPECT_EQ(Scene::ResolveKeptComponents(world), 0u) << Log();
    EXPECT_EQ(m_Library->FindByGuid(outpost), rebuilt) << "a second call rebuilds nothing";
    m_Library->CollectAtFrameBoundary();
    EXPECT_EQ(m_Library->GetRetiredCount(), 0u) << "the placed instance shows the rebuilt template";

    ECS::World reopened;
    const auto second = Load(reopened, "Frontier.scene", kFrontier);
    ASSERT_TRUE(second) << second.Error->message;
    std::vector<std::int32_t> values;
    reopened.Query<ECS::Read<LateMarkerProbe>>().Each([&values](ECS::EntityHandle, const LateMarkerProbe& marker)
                                                      { values.push_back(marker.Value); });
    EXPECT_EQ(values, std::vector<std::int32_t>{7}) << Log();
}

// A Place keeps the prefabs that the instance's PrefabRef fields name, as a scene load does; two
// prefabs that name each other both stay resident and the hold ends.
TEST_F(PrefabLibraryTests, PlacePrefabOfASpawnerKeepsThePrefabsItNamesThroughACycle)
{
    const GUID soldier = Register(Write("Soldier.prefab", kSoldierPrefab));
    const GUID barracks = Register(Write("Barracks.prefab", "[prefab version=1]\n[entity id=\"root\"]\n"
                                                             "PrefabSpawnerProbe.Prefab = [path=\"Soldier.prefab\" "
                                                             "guid=\"" + soldier.ToString() + "\"]\n"));
    Write("Soldier.prefab", std::string(kSoldierPrefab) + "PrefabSpawnerProbe.Prefab = [path=\"Barracks.prefab\" "
                                                          "guid=\"" + barracks.ToString() + "\"]\n");
    ECS::World world;
    ASSERT_TRUE(Scene::PlacePrefab(world, barracks, Components::Transform{}, {}).IsValid()) << Log();
    EXPECT_EQ(m_Library->GetResidentCount(), 2u) << Log();
    const std::size_t before = world.GetEntityCount();
    EXPECT_TRUE(Scene::SpawnPrefab(world, soldier, Components::Transform{}, {}).IsValid()) << Log();
    EXPECT_EQ(world.GetEntityCount(), before + 2u);
    EXPECT_EQ(CountLines(kNotResidentMessage), 0u) << Log();
}

// A prefab stays resident while a world holds it and is evicted at the frame boundary after the
// world is cleared or destroyed.
TEST_F(PrefabLibraryTests, APrefabNoWorldHoldsIsEvictedAtTheFrameBoundary)
{
    const GUID soldier = Register(Write("Soldier.prefab", kSoldierPrefab));
    const std::string scene = "[scene name=\"S\" version=1]\n[resource id=\"p\" path=\"Soldier.prefab\"]\n"
                              "[prefab id=\"a\" source=\"p\"]\n[prefab id=\"b\" source=\"p\"]\n";
    ECS::World cleared;
    const auto readResult = Load(cleared, "A.scene", scene);
    ASSERT_TRUE(readResult) << readResult.Error->message;
    auto destroyed = std::make_unique<ECS::World>();
    const auto readResult2 = Load(*destroyed, "B.scene", scene);
    ASSERT_TRUE(readResult2) << readResult2.Error->message;
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
    const auto readResult = Load(world, "S.scene",
                     "[scene name=\"S\" version=1]\n[resource id=\"p\" path=\"Soldier.prefab\"]\n"
                     "[prefab id=\"a\" source=\"p\"]\n");
    ASSERT_TRUE(readResult)
        << readResult.Error->message;
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
    const auto readResult = Load(holder, "S.scene",
                     "[scene name=\"S\" version=1]\n[resource id=\"p\" path=\"Soldier.prefab\"]\n"
                     "[prefab id=\"a\" source=\"p\"]\n");
    ASSERT_TRUE(readResult)
        << readResult.Error->message;
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
    const auto readResult = Load(world, "Barracks.scene",
                     "[scene name=\"Barracks\" version=1]\n"
                     "[entity id=\"spawner\"]\n"
                     "PrefabSpawnerProbe.Prefab = [path=\"Broken.prefab\" guid=\"" + broken.ToString() + "\"]\n");
    ASSERT_TRUE(readResult)
        << readResult.Error->message;

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
    // Declared before the pool: the held job can still read it after the release.
    std::atomic<bool> release{false};
    JobSystem::WorkStealingThreadPool pool(1);
    std::atomic<bool> started{false};
    JobSystem::JobCounter blocker;
    JobSystem::JobCounter spawn;
    pool.Run([&] { started = true; while (!release.load()) std::this_thread::yield(); }, blocker);
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
    const auto readResult = Load(world, "Barracks.scene",
                     "[scene name=\"Barracks\" version=1]\n"
                     "[entity id=\"spawner\"]\n"
                     "PrefabSpawnerProbe.Prefab = [path=\"Soldier.prefab\" guid=\"" + soldier.ToString() + "\"]\n");
    ASSERT_TRUE(readResult)
        << readResult.Error->message;
    EXPECT_DEATH(
        {
            SuppressCrtDialogsInDeathTestChild();
            SpawnInAJobTheMainThreadRuns(world, soldier);
        },
        "main thread");
}
#endif

// Writes `text` to the prefab `file` and reports the change as the file watcher does.
void ChangeOnDisk(Scene::PrefabLibrary& library, const std::filesystem::path& file, const GUID& prefab,
                  std::string_view text)
{
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
    const GUID changed[] = {prefab};
    library.RebuildChanged(changed);
}

constexpr std::string_view kCrateTopPrefab = "[prefab version=1]\n"
                                             "[entity id=\"root\"]\n"
                                             "Name.value = \"Crate\"\n"
                                             "[entity id=\"lid\" parent=\"root\"]\n"
                                             "Name.value = \"Top\"\n"
                                             "Transform.position = (0, 2, 0)\n"
                                             "[entity id=\"handle\" parent=\"lid\"]\n"
                                             "Name.value = \"Handle\"\n";

// Design 2.5 and 2.6: a prefab file changed on disk reaches the instances of every world that holds the
// prefab. A field the instance does not override takes the new value; an override stays; no entity is
// made again, so every handle stays valid.
TEST_F(PrefabLibraryTests, AFileChangeReachesTheInstancesOfEveryWorldAndKeepsTheirOverrides)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World scene;
    ECS::World preview;
    const ECS::EntityHandle first = Scene::PlacePrefab(scene, crate, Components::Transform{}, {});
    const ECS::EntityHandle second = Scene::PlacePrefab(scene, crate, Components::Transform{}, {});
    const ECS::EntityHandle other = Scene::PlacePrefab(preview, crate, Components::Transform{}, {});
    ASSERT_TRUE(first.IsValid() && second.IsValid() && other.IsValid()) << Log();
    const ECS::EntityHandle firstLid = FindChild(scene, first);
    const ECS::EntityHandle secondLid = FindChild(scene, second);
    const ECS::EntityHandle otherLid = FindChild(preview, other);
    Rename(scene, secondLid, "Mine");

    ChangeOnDisk(*m_Library, file, crate, kCrateTopPrefab);

    EXPECT_EQ(NameOf(scene, firstLid), "Top");
    EXPECT_EQ(NameOf(preview, otherLid), "Top");
    EXPECT_EQ(NameOf(scene, secondLid), "Mine");
    const auto* moved = scene.GetComponent<Components::Transform>(firstLid);
    ASSERT_NE(moved, nullptr);
    EXPECT_DOUBLE_EQ(moved->GetPosition().Y, 2.0);
    EXPECT_TRUE(OverridesOf(scene, first, firstLid).empty()) << "the old value is no override";
    EXPECT_EQ(OverridesOf(scene, second, secondLid).size(), 1u);
    EXPECT_TRUE(OverridesOf(preview, other, otherLid).empty());
    EXPECT_TRUE(scene.IsValid(first) && scene.IsValid(firstLid) && preview.IsValid(otherLid));
}

// Design 2.6: a field the instance kept because it differed gets a record at the prefab change, so it stays an
// override when the prefab takes the same value, and the next change does not move it (Crate A's lid in #5858).
TEST_F(PrefabLibraryTests, AFieldThePrefabChangesToTheInstanceValueStaysAnOverrideAndKeepsItsValue)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle lid = FindChild(world, root);
    Rename(world, lid, "Top");

    ChangeOnDisk(*m_Library, file, crate, kCrateTopPrefab);
    const std::vector<Scene::PrefabOverride> kept = OverridesOf(world, root, lid);
    ASSERT_EQ(kept.size(), 1u) << "the value equal to the prefab's is an override";
    EXPECT_EQ(kept[0].Kind, Scene::PrefabOverrideKind::Field);
    EXPECT_EQ(kept[0].Field, "value");

    ChangeOnDisk(*m_Library, file, crate, kCratePrefab);
    EXPECT_EQ(NameOf(world, lid), "Top");
    EXPECT_EQ(OverridesOf(world, root, lid).size(), 1u);
}

// Design 2.6: Revert to prefab value drops the record and the field follows the next prefab change; the record
// calls of the editor's undo and redo give the record back and drop it again.
TEST_F(PrefabLibraryTests, ARevertDropsTheRecordAndTheFieldFollowsTheNextPrefabChange)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle lid = FindChild(world, root);
    Rename(world, lid, "Top");
    ChangeOnDisk(*m_Library, file, crate, kCrateTopPrefab);
    const std::vector<Scene::PrefabOverride> kept = OverridesOf(world, root, lid);
    ASSERT_EQ(kept.size(), 1u);

    std::string error;
    ASSERT_TRUE(Scene::RevertPrefabOverride(world, lid, kept[0], error)) << error;
    EXPECT_TRUE(OverridesOf(world, root, lid).empty());
    const std::vector<Scene::PrefabOverrideTarget> targets{{lid, kept[0]}};
    Scene::RecordPrefabOverrides(world, targets);
    EXPECT_EQ(OverridesOf(world, root, lid).size(), 1u);
    Scene::ForgetPrefabOverrides(world, targets);
    EXPECT_TRUE(OverridesOf(world, root, lid).empty());

    ChangeOnDisk(*m_Library, file, crate, kCratePrefab);
    EXPECT_EQ(NameOf(world, lid), "Lid");
}

// Design 2.6: each field line of a [prefab] section is a record, so a line equal to the prefab's value loads as
// an override and the save writes it again.
TEST_F(PrefabLibraryTests, AFieldLineEqualToThePrefabValueLoadsAsAnOverrideAndTheSaveKeepsIt)
{
    Register(Write("Crate.prefab", kCratePrefab));
    ECS::World world;
    const auto readResult = Load(world, "Yard.scene",
                                 "[scene name=\"Yard\" version=1]\n[resource id=\"crate\" path=\"Crate.prefab\"]\n"
                                 "[prefab id=\"crate\" source=\"crate\"]\nlid/Name.value = \"Lid\"\n");
    ASSERT_TRUE(readResult) << readResult.Error->message;
    ECS::EntityHandle root{};
    world.Query<ECS::Read<Components::PrefabInstance>>().Each(
        [&root](ECS::EntityHandle entity, const Components::PrefabInstance&) { root = entity; });
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    EXPECT_EQ(NameOf(world, lid), "Lid");
    EXPECT_EQ(OverridesOf(world, root, lid).size(), 1u);

    Scene::SceneWriteOptions options;
    options.assetResolver = &*m_Resolver;
    const std::filesystem::path scenePath = EngineCore::GetInstance().GetResolvedAssetRoot() / "Saved.scene";
    const auto writeResult = Scene::WriteSceneFile(world, scenePath, options);
    ASSERT_TRUE(writeResult) << writeResult.Error->message;
    EXPECT_NE(ReadText(scenePath).find("lid/Name.value = \"Lid\"\n"), std::string::npos) << ReadText(scenePath);
}

// Design 2.5: a child the prefab gains appears in each instance under its parent, as a member; a child the
// prefab loses leaves each instance. An entity the instance added under the lost child moves to the nearest
// entity that stays, and keeps its values.
TEST_F(PrefabLibraryTests, AChildThePrefabGainsOrLosesIsAddedToOrRemovedFromEachInstance)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle lid = FindChild(world, root);
    const ECS::EntityHandle handle = FindChild(world, lid);
    ASSERT_TRUE(handle.IsValid());
    const ECS::EntityHandle sticker = world.CreateEntity();
    Components::Name stickerName{};
    std::string_view("Sticker").copy(stickerName.value, sizeof(stickerName.value) - 1);
    world.AddComponentImmediate(sticker, stickerName);
    world.AddComponentImmediate(sticker, Components::Parent{handle});

    ChangeOnDisk(*m_Library, file, crate,
                 "[prefab version=1]\n[entity id=\"root\"]\nName.value = \"Crate\"\n"
                 "[entity id=\"lid\" parent=\"root\"]\nName.value = \"Lid\"\nTransform.position = (0, 1, 0)\n"
                 "[entity id=\"hinge\" parent=\"lid\"]\nName.value = \"Hinge\"\n");

    EXPECT_FALSE(world.IsValid(handle)) << "the prefab lost the handle";
    ASSERT_TRUE(world.IsValid(lid));
    const ECS::EntityHandle hinge = FindChildNamed(world, lid, "Hinge");
    ASSERT_TRUE(hinge.IsValid()) << "the prefab gained the hinge";
    ASSERT_TRUE(world.IsValid(sticker));
    const auto* stickerParent = world.GetComponent<Components::Parent>(sticker);
    ASSERT_NE(stickerParent, nullptr);
    EXPECT_EQ(stickerParent->Entity, lid);

    std::vector<Scene::PrefabInstanceEntity> entities;
    ASSERT_TRUE(Scene::DescribePrefabInstance(world, root, entities));
    for (const Scene::PrefabInstanceEntity& entity : entities)
    {
        EXPECT_FALSE(entity.HasOverrides()) << NameOf(world, entity.Entity) << " has an override";
        EXPECT_EQ(entity.AddedOnInstance, entity.Entity == sticker) << NameOf(world, entity.Entity);
    }
    EXPECT_EQ(entities.size(), 4u) << "the root, the lid, the hinge and the sticker";
}

// Design 2.5: an entity the instance added under a lost child keeps its world position when it moves to
// the nearest entity that stays.
TEST_F(PrefabLibraryTests, AnEntityAddedUnderALostChildKeepsItsWorldPosition)
{
    const std::filesystem::path file = Write("Crate.prefab", "[prefab version=1]\n[entity id=\"root\"]\n"
                                                             "[entity id=\"lid\" parent=\"root\"]\nTransform.position = (0, 1, 0)\n"
                                                             "[entity id=\"handle\" parent=\"lid\"]\nTransform.position = (2, 0, 0)\n");
    const GUID crate = Register(file);
    ECS::World world;
    Engine::Renderer::TransformHierarchySystem hierarchy;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle handle = FindChild(world, FindChild(world, root));
    ASSERT_TRUE(handle.IsValid());
    const ECS::EntityHandle sticker = world.CreateEntity();
    world.AddComponentImmediate(sticker, Components::Transform::FromTRS(Mathematics::WorldPosition{0.0, 0.0, 3.0}, Quaternion::Identity(),
                                                                        Vector3(1.0f, 1.0f, 1.0f)));
    world.AddComponentImmediate(sticker, Components::Parent{handle});
    hierarchy.Update(world, 0.0f);
    ASSERT_DOUBLE_EQ(world.GetComponent<Components::WorldTransform>(sticker)->GetPosition().X, 2.0);

    ChangeOnDisk(*m_Library, file, crate,
                 "[prefab version=1]\n[entity id=\"root\"]\n[entity id=\"lid\" parent=\"root\"]\nTransform.position = (0, 1, 0)\n");
    hierarchy.Update(world, 0.0f);

    ASSERT_FALSE(world.IsValid(handle)) << "the prefab lost the handle";
    const Mathematics::WorldPosition position = world.GetComponent<Components::WorldTransform>(sticker)->GetPosition();
    EXPECT_NEAR(position.X, 2.0, 1e-5);
    EXPECT_NEAR(position.Y, 1.0, 1e-5);
    EXPECT_NEAR(position.Z, 3.0, 1e-5);
}

// Design 2.5: a deferred world takes no prefab file change until it resumes, then takes every change made
// in the meantime. The generation its instances show stays alive while it is deferred.
TEST_F(PrefabLibraryTests, ADeferredWorldTakesEveryChangeWhenItResumes)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle lid = FindChild(world, root);
    m_Library->SetPropagationDeferred(world, true);

    ChangeOnDisk(*m_Library, file, crate, kCrateTopPrefab);
    m_Library->CollectAtFrameBoundary();
    std::string three(kCrateTopPrefab);
    three.replace(three.find("\"Top\""), 5, "\"Three\"");
    ChangeOnDisk(*m_Library, file, crate, three);
    m_Library->CollectAtFrameBoundary();

    EXPECT_EQ(NameOf(world, lid), "Lid") << "a deferred world took a change";
    EXPECT_EQ(m_Library->GetRetiredCount(), 1u) << "only the generation the deferred world shows stays";

    m_Library->SetPropagationDeferred(world, false);
    EXPECT_EQ(NameOf(world, lid), "Three");
    EXPECT_TRUE(OverridesOf(world, root, lid).empty());
    m_Library->CollectAtFrameBoundary();
    EXPECT_EQ(m_Library->GetRetiredCount(), 0u);
}

// Design 2.6: a rewrite of a prefab file that changes no instance, such as the file watcher's report of the
// editor's own Apply, reports no change.
TEST_F(PrefabLibraryTests, ARewriteThatChangesNoInstanceReportsNoChange)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    ASSERT_TRUE(Scene::PlacePrefab(world, crate, Components::Transform{}, {}).IsValid()) << Log();
    int changes = 0;
    const EventSubscription subscription =
        m_Library->InstancesChanged.Subscribe([&changes](ECS::World&) { ++changes; });

    ChangeOnDisk(*m_Library, file, crate, kCratePrefab);
    EXPECT_EQ(changes, 0) << "a rewrite with the same text reported a change";

    ChangeOnDisk(*m_Library, file, crate, kCrateTopPrefab);
    EXPECT_EQ(changes, 1);
}

// Design 2.5: a child the prefab gains keeps the values of its prefab entity as a placed instance does.
// The built-in material of a primitive has no registry record, so a read of it as text logs a degraded
// reference.
TEST_F(PrefabLibraryTests, AGainedPrimitiveKeepsTheBuiltInMaterialAndLogsNoDegradedReference)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();

    std::string gained(kCratePrefab);
    gained += "[entity id=\"hinge\" parent=\"root\"]\nName.value = \"Hinge\"\nMeshRenderer.meshPrimitive = Cube\n";
    ChangeOnDisk(*m_Library, file, crate, gained);

    const ECS::EntityHandle hinge = FindChildNamed(world, root, "Hinge");
    ASSERT_TRUE(hinge.IsValid()) << "the prefab gained the hinge";
    const auto* renderer = world.GetComponent<Components::MeshRenderer>(hinge);
    ASSERT_NE(renderer, nullptr);
    EXPECT_EQ(renderer->materialAssetGuid.ToGuid(), Engine::Renderer::PrimitiveGenerator::DefaultMaterialGuid());
    EXPECT_EQ(CountLines("loading as degraded reference"), 0u) << Log();
}

// Design 2.5: an entity field of a child the prefab gains names the member of the same instance.
TEST_F(PrefabLibraryTests, AnEntityFieldOfAGainedChildNamesTheMemberOfItsInstance)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle lid = FindChild(world, root);

    std::string gained(kCratePrefab);
    gained += "[entity id=\"hinge\" parent=\"root\"]\nName.value = \"Hinge\"\nPrefabLinkProbe.Target = \"lid\"\n";
    ChangeOnDisk(*m_Library, file, crate, gained);

    const ECS::EntityHandle hinge = FindChildNamed(world, root, "Hinge");
    ASSERT_TRUE(hinge.IsValid()) << "the prefab gained the hinge";
    const auto* link = world.GetComponent<PrefabLinkProbe>(hinge);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->Target, lid) << "the field does not name the lid of the instance";
}

// Design 2.5: a world stays deferred when it is cleared, so a scene that loads into it during Play takes no
// change until the world resumes.
TEST_F(PrefabLibraryTests, ADeferredWorldStaysDeferredWhenItIsCleared)
{
    const std::filesystem::path file = Write("Crate.prefab", kCratePrefab);
    const GUID crate = Register(file);
    ECS::World world;
    ASSERT_TRUE(Scene::PlacePrefab(world, crate, Components::Transform{}, {}).IsValid()) << Log();
    m_Library->SetPropagationDeferred(world, true);
    world.Clear();
    m_Library->CollectAtFrameBoundary();
    const ECS::EntityHandle root = Scene::PlacePrefab(world, crate, Components::Transform{}, {});
    ASSERT_TRUE(root.IsValid()) << Log();
    const ECS::EntityHandle lid = FindChild(world, root);

    ChangeOnDisk(*m_Library, file, crate, kCrateTopPrefab);
    EXPECT_EQ(NameOf(world, lid), "Lid") << "the cleared world took a change";

    m_Library->SetPropagationDeferred(world, false);
    EXPECT_EQ(NameOf(world, lid), "Top");
}

} // namespace
} // namespace GameEngine
