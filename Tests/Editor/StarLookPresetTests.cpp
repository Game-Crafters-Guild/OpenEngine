// The night stars' looks (Sky/StarLookPresets): the values each look sets, that a look is one undo step
// over the sky and every global volume, and that a look never touches the rest of the sky.

#include "Sky/StarLookPresets.h"

#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

using namespace GameEngine;

namespace
{

const StarLookPreset& Look(const char* name)
{
    for (const StarLookPreset& preset : StarLookPresets())
        if (std::strcmp(preset.Name, name) == 0)
            return preset;
    ADD_FAILURE() << "no look named " << name;
    return StarLookPresets().front();
}

} // namespace

// The four looks in the row's order, Vivid first.
TEST(StarLookPresets, TheLooksAreVividRestrainedPhysicalAndClassic)
{
    const auto presets = StarLookPresets();
    ASSERT_EQ(presets.size(), 4u);
    EXPECT_STREQ(presets[0].Name, "Vivid");
    EXPECT_STREQ(presets[1].Name, "Restrained");
    EXPECT_STREQ(presets[2].Name, "Physical");
    EXPECT_STREQ(presets[3].Name, "Classic");
}

// The engine's default is the Vivid look: a new Sky Environment and a new Post Process Volume carry its
// values, so a new scene matches the Vivid button.
TEST(StarLookPresets, TheComponentsDefaultsAreTheVividLook)
{
    const StarLookPreset& vivid = Look("Vivid");
    EXPECT_TRUE(MatchesStarLook(Components::SkyEnvironment{}, vivid));
    const Components::PostProcessVolume volume{};
    EXPECT_FLOAT_EQ(volume.NightTintStrength, vivid.NightTintStrength);
    for (int i = 0; i < 3; ++i)
        EXPECT_FLOAT_EQ(volume.NightTintColor[i], vivid.NightTintColor[i]);
    EXPECT_GT(vivid.Spikes, 0.0f) << "Vivid carries the diffraction spikes";
    EXPECT_LT(vivid.CoreScale, 1.0f) << "Vivid's core is tighter than the physical one";
}

// Physical is one click to every artistic dial at its neutral value, seen by a camera; the other looks
// are the eye's.
TEST(StarLookPresets, PhysicalIsEveryArtisticDialAtItsNeutralValue)
{
    const StarLookPreset& physical = Look("Physical");
    EXPECT_FLOAT_EQ(physical.Compression, 1.0f);
    EXPECT_FLOAT_EQ(physical.Glow, 0.0f);
    EXPECT_FLOAT_EQ(physical.Spikes, 0.0f);
    EXPECT_FLOAT_EQ(physical.CoreScale, 1.0f);
    EXPECT_EQ(physical.Shape, Components::StarShapeKind::Point);
    EXPECT_FLOAT_EQ(physical.NightTintStrength, 0.0f);
    EXPECT_FLOAT_EQ(physical.LimitingMagnitude, 6.5f) << "the naked eye's limit";
    EXPECT_FLOAT_EQ(physical.TwinkleRate, Components::kStarTwinkleRateMax) << "the fastest flicker-free twinkle";
    EXPECT_EQ(physical.Observer, Components::StarObserverKind::Camera) << "no glare, the catalog color everywhere";
    EXPECT_EQ(Look("Vivid").Observer, Components::StarObserverKind::Eye);
    EXPECT_EQ(Look("Restrained").Observer, Components::StarObserverKind::Eye);
    EXPECT_EQ(Look("Classic").Observer, Components::StarObserverKind::Eye);
    EXPECT_EQ(Look("Classic").Shape, Components::StarShapeKind::Sparkle);
}

// One click is one undo step: the sky's dials and every global volume's night tint change together, a
// local volume keeps its own, the sky's exposure trim is untouched, and one undo puts all of it back.
TEST(StarLookPresets, ALookIsOneUndoStepAndLeavesTheRestOfTheSkyAlone)
{
    ECS::World world;
    Editor::EditorChangeNotifications notifications;
    Editor::UndoRedoService undo;
    const ECS::EntityHandle sky = world.CreateEntity();
    Components::SkyEnvironment skyValues{};
    skyValues.SkyExposureTrim = 0.7f;
    skyValues.StarGlow = 0.1f;
    world.AddComponentImmediate(sky, skyValues);
    const ECS::EntityHandle globalA = world.CreateEntity();
    const ECS::EntityHandle globalB = world.CreateEntity();
    const ECS::EntityHandle local = world.CreateEntity();
    Components::PostProcessVolume globalVolume{};
    globalVolume.NightTintStrength = 0.25f;
    world.AddComponentImmediate(globalA, globalVolume);
    world.AddComponentImmediate(globalB, globalVolume);
    Components::PostProcessVolume localVolume = globalVolume;
    localVolume.IsGlobal = false;
    world.AddComponentImmediate(local, localVolume);

    const StarLookPreset& physical = Look("Physical");
    EXPECT_TRUE(HasGlobalPostProcessVolume(world));
    ApplyStarLookPreset(physical, world, sky, &notifications, &undo);
    EXPECT_EQ(undo.GetUndoCount(), 1u) << "one click, one undo step";
    EXPECT_TRUE(MatchesStarLook(*world.GetComponent<Components::SkyEnvironment>(sky), physical));
    EXPECT_FLOAT_EQ(world.GetComponent<Components::SkyEnvironment>(sky)->SkyExposureTrim, 0.7f)
        << "a look never touches the sky's exposure trim";
    EXPECT_FLOAT_EQ(world.GetComponent<Components::PostProcessVolume>(globalA)->NightTintStrength, 0.0f);
    EXPECT_FLOAT_EQ(world.GetComponent<Components::PostProcessVolume>(globalB)->NightTintStrength, 0.0f);
    EXPECT_FLOAT_EQ(world.GetComponent<Components::PostProcessVolume>(local)->NightTintStrength, 0.25f);

    undo.Undo();
    EXPECT_FLOAT_EQ(world.GetComponent<Components::SkyEnvironment>(sky)->StarGlow, 0.1f);
    EXPECT_FLOAT_EQ(world.GetComponent<Components::PostProcessVolume>(globalA)->NightTintStrength, 0.25f);
    EXPECT_FLOAT_EQ(world.GetComponent<Components::PostProcessVolume>(globalB)->NightTintStrength, 0.25f);
}

// Without a global volume a look reaches no night tint, and says so.
TEST(StarLookPresets, WithoutAGlobalVolumeALookSetsTheStarDialsOnly)
{
    ECS::World world;
    const ECS::EntityHandle sky = world.CreateEntity();
    world.AddComponentImmediate(sky, Components::SkyEnvironment{});
    EXPECT_FALSE(HasGlobalPostProcessVolume(world));
    ApplyStarLookPreset(Look("Restrained"), world, sky, nullptr, nullptr);
    EXPECT_TRUE(MatchesStarLook(*world.GetComponent<Components::SkyEnvironment>(sky), Look("Restrained")));
}
