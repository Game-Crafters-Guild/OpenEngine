// The editor's terrain creation (create menu, create_terrain IPC) over the engine's
// TerrainECS::ProvisionTerrainEntity: the default surface-rules volume it spawns lands at the
// bottom of the hierarchy, a refusal reaches the caller as text, and the creation is one undo step
// that also switches the template's ground plane off.

#include <gtest/gtest.h>

#include "Terrain/TerrainEntityProvisioning.h"

#include "SceneViewNoticeRecorder.h"

#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/TemplateGroundPlaceholder.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "Terrain/TerrainInspector.h"
#include "TerrainECS/Systems/TerrainWorldHooks.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainService.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstring>
#include <string>
#include <string_view>

namespace
{

using namespace GameEngine;

TEST(EditorTerrainProvisioning, TheDefaultSurfaceRulesVolumeLandsBelowEverythingInTheScene)
{
    ECS::World world;
    const ECS::EntityHandle existing = world.CreateEntity();
    world.AddComponentImmediate(existing, Components::HierarchyOrder{7});

    const ECS::EntityHandle terrain = world.CreateEntity();
    const auto preset = CBTTerrainECS::MakeTerrainPreset(CBTTerrainECS::TerrainPreset::LargePlanar);
    EXPECT_EQ(Editor::ProvisionTerrainEntity(world, terrain, preset, Components::TerrainPlanetRelief{}), "");
    EXPECT_NE(world.GetComponent<Components::TerrainGrass>(terrain), nullptr);

    int volumes = 0;
    world.Query<ECS::Read<Components::TerrainSurfaceRulesEffect>>().Each(
        [&](ECS::EntityHandle volume, const Components::TerrainSurfaceRulesEffect&) {
            ++volumes;
            const auto* order = world.GetComponent<Components::HierarchyOrder>(volume);
            ASSERT_NE(order, nullptr) << "the volume takes a position in the hierarchy";
            EXPECT_GT(order->order, 7);
            EXPECT_EQ(world.GetComponent<Components::Name>(volume)->View(), "Terrain Surface Rules");
        });
    EXPECT_EQ(volumes, 1);
}

TEST(EditorTerrainProvisioning, ARefusedTerrainReturnsTheReasonAndAddsNothing)
{
    ECS::World world;
    const ECS::EntityHandle terrain = world.CreateEntity();
    auto config = CBTTerrainECS::MakeTerrainPreset(CBTTerrainECS::TerrainPreset::LargePlanar);
    config.SizeX = 1e30f;
    const std::string refusal = Editor::ProvisionTerrainEntity(world, terrain, config, Components::TerrainPlanetRelief{});
    EXPECT_NE(refusal.find("on each side"), std::string::npos) << refusal;
    EXPECT_EQ(world.GetComponent<Components::Terrain>(terrain), nullptr);
}

// A scene with a ground plane named `name`: a renderer and a collider, marked as the template's
// placeholder when `marked` is set. The terrain service stays up for the rest of the binary
// (TerrainEntityDuplicateTests shares it).
class EditorTerrainCreation : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!TerrainECS::TerrainService::IsInitialized())
            TerrainECS::TerrainService::Initialize();
        TerrainECS::RegisterTerrainWorldHooks(World);
        Editor::TestStubs::LastSceneViewNotice().clear();
        Editor::TestStubs::LastSceneViewNoticeAction() = {};
    }

    ECS::EntityHandle AddGround(bool marked, std::string_view name = "Floor")
    {
        const ECS::EntityHandle ground = World.CreateEntity();
        Components::Name nameComponent{};
        std::memcpy(nameComponent.value, name.data(), name.size());
        World.AddComponentImmediate(ground, nameComponent);
        World.AddComponentImmediate(ground, Components::MeshRenderer{});
        World.AddComponentImmediate(ground, Components::PhysicsCollider{});
        if (marked)
            World.AddComponentImmediate(ground, Components::TemplateGroundPlaceholder{});
        return ground;
    }

    ECS::EntityHandle CreateSmallTerrain()
    {
        return Editor::CreateTerrainEntity(World, &Undo, nullptr, CBTTerrainECS::TerrainPreset::SmallPlanar);
    }

    bool IsOn(ECS::EntityHandle entity) { return ECS::Entity(&World, entity).IsEnabled(); }

    ECS::World World;
    Editor::UndoRedoService Undo;
};

TEST_F(EditorTerrainCreation, ATerrainSwitchesTheTemplateGroundOffInOneUndoStepThatUndoRestores)
{
    const ECS::EntityHandle ground = AddGround(true);

    const ECS::EntityHandle terrain = CreateSmallTerrain();
    ASSERT_TRUE(World.IsValid(terrain));
    ASSERT_NE(World.GetComponent<Components::Terrain>(terrain), nullptr);
    int volumes = 0;
    World.Query<ECS::Read<Components::TerrainSurfaceRulesEffect>>().Each(
        [&](const Components::TerrainSurfaceRulesEffect&) { ++volumes; });
    EXPECT_EQ(volumes, 1);

    EXPECT_EQ(Undo.GetUndoCount(), 1u);
    EXPECT_STREQ(Undo.PeekUndoName(), "Create Terrain");
    EXPECT_FALSE(IsOn(ground));
    EXPECT_TRUE(World.IsValid(ground)) << "the placeholder stays in the scene";
    EXPECT_EQ(Editor::TestStubs::LastSceneViewNotice(), "Floor is switched off so it does not cover the terrain.");
    EXPECT_EQ(Editor::TestStubs::LastSceneViewNoticeAction().Label, "Turn on");
    EXPECT_EQ(Editor::TestStubs::LastSceneViewNoticeAction().Tooltip, "Switch Floor back on. The terrain stays.");

    Undo.Undo();
    EXPECT_TRUE(IsOn(ground));
    EXPECT_FALSE(World.IsValid(terrain));
    volumes = 0;
    World.Query<ECS::Read<Components::TerrainSurfaceRulesEffect>>().Each(
        [&](const Components::TerrainSurfaceRulesEffect&) { ++volumes; });
    EXPECT_EQ(volumes, 0);

    Undo.Redo();
    EXPECT_TRUE(World.IsValid(terrain));
    EXPECT_NE(World.GetComponent<Components::Terrain>(terrain), nullptr);
    EXPECT_FALSE(IsOn(ground));
}

// A new terrain is flat at its transform's height: no volume supplies a Generated noise.
TEST_F(EditorTerrainCreation, ANewTerrainIsFlatWithNoGeneratedNoise)
{
    const ECS::EntityHandle terrain = CreateSmallTerrain();
    ASSERT_TRUE(World.IsValid(terrain));
    EXPECT_EQ(World.GetComponent<Components::Terrain>(terrain)->BaseSource, Components::TerrainBaseSource::Flat);
    int noises = 0;
    World.Query<ECS::Read<Components::TerrainNoiseEffect>>().Each([&](const Components::TerrainNoiseEffect&) { ++noises; });
    EXPECT_EQ(noises, 0);
    EXPECT_FALSE(TerrainECS::FindGeneratedNoiseVolume(World).IsValid());
}

// The notice's "Turn on" switches the ground back on as an undo step of its own, and the terrain stays.
TEST_F(EditorTerrainCreation, TheNoticeTurnsTheGroundBackOnInItsOwnUndoStep)
{
    const ECS::EntityHandle ground = AddGround(true);
    const ECS::EntityHandle terrain = CreateSmallTerrain();
    const Editor::TestStubs::RecordedNoticeAction action = Editor::TestStubs::LastSceneViewNoticeAction();
    ASSERT_TRUE(action.OnInvoke);

    action.OnInvoke();
    EXPECT_TRUE(IsOn(ground));
    EXPECT_TRUE(World.IsValid(terrain));
    EXPECT_EQ(Undo.GetUndoCount(), 2u);
    EXPECT_STREQ(Undo.PeekUndoName(), "Switch Floor on");

    Undo.Undo();
    EXPECT_FALSE(IsOn(ground));
    EXPECT_TRUE(World.IsValid(terrain));
}

// With two marked grounds, the notice and the button's tooltip name both, and the notice's verbs agree.
TEST_F(EditorTerrainCreation, TheNoticeNamesEverySwitchedOffGround)
{
    AddGround(true);
    AddGround(true, "Ground");
    CreateSmallTerrain();
    EXPECT_EQ(Editor::TestStubs::LastSceneViewNotice(),
              "Floor and Ground are switched off so they do not cover the terrain.");
    EXPECT_EQ(Editor::TestStubs::LastSceneViewNoticeAction().Tooltip,
              "Switch Floor and Ground back on. The terrain stays.");
}

// After a scene change the notice's "Turn on" does nothing: the old handles can name entities of the
// new scene, so the action reads the world's handle run before it uses them.
TEST_F(EditorTerrainCreation, TheNoticeDoesNothingAfterTheSceneChanges)
{
    AddGround(true);
    CreateSmallTerrain();
    const Editor::TestStubs::RecordedNoticeAction action = Editor::TestStubs::LastSceneViewNoticeAction();
    ASSERT_TRUE(action.OnInvoke);

    World.Clear();
    Undo.Clear();
    const ECS::EntityHandle ground = AddGround(true);
    ECS::Entity(&World, ground).SetEnabled(false);

    action.OnInvoke();
    EXPECT_FALSE(IsOn(ground));
    EXPECT_EQ(Undo.GetUndoCount(), 0u);
}

// A second terrain while the ground is off records no switch and shows no notice: undoing it leaves
// the ground off, and undoing the first terrain turns it on.
TEST_F(EditorTerrainCreation, ASecondTerrainLeavesTheGroundOffUntilTheFirstIsUndone)
{
    const ECS::EntityHandle ground = AddGround(true);
    CreateSmallTerrain();
    Editor::TestStubs::LastSceneViewNotice().clear();
    Editor::TestStubs::LastSceneViewNoticeAction() = {};

    EXPECT_TRUE(World.IsValid(CreateSmallTerrain()));
    EXPECT_EQ(Undo.GetUndoCount(), 2u);
    EXPECT_EQ(Editor::TestStubs::LastSceneViewNotice(), "");
    EXPECT_FALSE(Editor::TestStubs::LastSceneViewNoticeAction().OnInvoke);

    Undo.Undo();
    EXPECT_FALSE(IsOn(ground));
    Undo.Undo();
    EXPECT_TRUE(IsOn(ground));
}

TEST_F(EditorTerrainCreation, AGroundWithoutTheMarkerStaysOn)
{
    const ECS::EntityHandle ground = AddGround(false);

    EXPECT_TRUE(World.IsValid(CreateSmallTerrain()));
    EXPECT_EQ(Undo.GetUndoCount(), 1u);
    EXPECT_TRUE(IsOn(ground));
    EXPECT_EQ(Editor::TestStubs::LastSceneViewNotice(), "");
}

TEST(EditorTerrainProvisioning, TheTemplateGroundPlaceholderHasNoInspectorSection)
{
    RegisterTerrainInspector();
    Editor::EditorComponentTraits traits;
    ASSERT_TRUE(Editor::EditorComponentTraitsRegistry::Get().TryGet(
        ECS::GetComponentTypeId<Components::TemplateGroundPlaceholder>(), traits));
    EXPECT_TRUE(traits.HideInInspector);
}

} // namespace
