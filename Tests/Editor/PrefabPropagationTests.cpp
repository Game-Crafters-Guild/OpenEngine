// A prefab file change in the editor: Play mode holds it back until Stop, and the prefab feature tells
// the panels that the instances of a world changed.

#include <gtest/gtest.h>

#include "EditorChangeNotifications.h"
#include "PlayMode/PlayModeManager.h"
#include "Prefabs/PrefabHierarchyRows.h"
#include "Prefabs/PrefabService.h"
#include "Scene/SceneDocumentManager.h"
#include "UndoRedo/UndoRedoService.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/Name.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "Scene/PrefabAuthoring.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr std::string_view kCratePrefab = "[prefab version=1]\n"
                                          "[entity id=\"root\"]\n"
                                          "[entity id=\"lid\" parent=\"root\"]\n"
                                          "Name.value = \"Lid\"\n";
constexpr std::string_view kCrateTopPrefab = "[prefab version=1]\n"
                                             "[entity id=\"root\"]\n"
                                             "[entity id=\"lid\" parent=\"root\"]\n"
                                             "Name.value = \"Top\"\n";

class PrefabPropagationTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
        m_Folder = engine.GetAssetManager().GetAssetRoot() /
                   ("ge_prefab_propagation_" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        m_File = (m_Folder / "Crate.prefab").lexically_normal();
        std::filesystem::create_directories(m_Folder);
        std::ofstream(m_File, std::ios::binary) << kCratePrefab;
        ASSERT_TRUE(engine.GetAssetManager().GetRegistry().RegisterAssetByPath(m_File).IsOk());
        m_Prefab = engine.GetAssetManager().ResolveAssetGuid(m_File);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Folder, ec);
    }

    ECS::EntityHandle Place(ECS::World& world)
    {
        Components::Transform at{};
        at.SetIdentity();
        return Editor::PrefabService(EngineCore::GetInstance().GetAssetManager()).Place(world, m_File, at, {});
    }

    // Writes `text` to the prefab file and rebuilds it, as the file watcher's report does.
    void ChangeOnDisk(std::string_view text)
    {
        std::ofstream(m_File, std::ios::binary | std::ios::trunc) << text;
        Scene::RebuildPrefab(m_Prefab);
    }

    static std::string NameOf(const ECS::World& world, ECS::EntityHandle entity)
    {
        const auto* name = world.GetComponent<Components::Name>(entity);
        return name ? std::string(name->View()) : std::string();
    }

    std::filesystem::path m_Folder;
    std::filesystem::path m_File;
    GUID m_Prefab;
};

} // namespace

// Design 2.5: the play session is the scene as it was at Play, so a prefab file change made during Play
// does not reach it. Stop restores the scene, which then takes the change.
TEST_F(PrefabPropagationTests, AChangeDuringPlayReachesTheSceneAtStop)
{
    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);
    world->Clear();
    const ECS::EntityHandle root = Place(*world);
    ASSERT_TRUE(root.IsValid());
    const std::vector<ECS::EntityHandle> children = Components::ChildrenOf(*world, root);
    ASSERT_EQ(children.size(), 1u);
    const ECS::EntityHandle lid = children.front();

    Editor::UndoRedoService undo;
    Editor::EditorChangeNotifications notifications;
    Editor::PlayModeManager playMode;
    playMode.SetUndoRedo(&undo);
    playMode.SetChangeNotifications(&notifications);
    playMode.SetWorld(world);
    playMode.EnterPlayMode();
    ASSERT_EQ(playMode.GetState(), Editor::PlayModeState::Play);

    ChangeOnDisk(kCrateTopPrefab);
    EXPECT_EQ(NameOf(*world, lid), "Lid") << "the play session took the change";

    playMode.ExitPlayMode();
    ASSERT_EQ(playMode.GetState(), Editor::PlayModeState::Edit);
    ASSERT_TRUE(world->IsValid(lid));
    EXPECT_EQ(NameOf(*world, lid), "Top") << "the scene did not take the change at Stop";
    world->Clear();
}

// The prefab feature turns a change of a world's instances into a structure change of that world, so
// the Hierarchy and the Inspector read the world again.
TEST_F(PrefabPropagationTests, AFileChangeTellsThePanelsThatTheWorldChanged)
{
    // The registration keeps the channel for the rest of the process.
    static Editor::EditorChangeNotifications notifications;
    Editor::RegisterPrefabEditor(notifications, nullptr, {}, {});
    ECS::World world;
    ASSERT_TRUE(Place(world).IsValid());
    int changes = 0;
    const auto token = notifications.SubscribeWorldStructureChanged(
        [&changes, &world](const Editor::EditorChangeNotifications::WorldStructureChangedEvent& event)
        {
            if (event.world == &world)
                ++changes;
        });

    ChangeOnDisk(kCrateTopPrefab);

    EXPECT_EQ(changes, 1);
    notifications.Unsubscribe(token);
}

// A prefab file change is no edit of the scene: the scene stays clean after a rewrite that changes nothing,
// after a change that reaches the instances, and after a deferred world resumes and takes a change.
TEST_F(PrefabPropagationTests, AFileChangeLeavesTheSceneClean)
{
    // The registration keeps the channel for the rest of the process.
    static Editor::EditorChangeNotifications notifications;
    Editor::RegisterPrefabEditor(notifications, nullptr, {}, {});
    ECS::World world;
    ASSERT_TRUE(Place(world).IsValid());
    Editor::SceneDocumentManager document;
    document.SetChangeNotifications(&notifications);
    document.SetWorld(&world);
    document.MarkClean();

    ChangeOnDisk(kCratePrefab);
    EXPECT_FALSE(document.IsDirty()) << "a rewrite that changes nothing marked the scene dirty";
    ChangeOnDisk(kCrateTopPrefab);
    EXPECT_FALSE(document.IsDirty()) << "a change of the instances marked the scene dirty";
    Scene::SetPrefabPropagationDeferred(world, true);
    ChangeOnDisk(kCratePrefab);
    Scene::SetPrefabPropagationDeferred(world, false);
    EXPECT_FALSE(document.IsDirty()) << "the resume marked the scene dirty";
}

} // namespace GameEngine
