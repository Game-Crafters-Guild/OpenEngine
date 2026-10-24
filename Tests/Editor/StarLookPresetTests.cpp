// The night stars' looks (Sky/StarLookPresets): the values each look sets, that a look is one undo step
// over the sky and every global volume, that a look never touches the rest of the sky, and that the
// looks' pictures were taken with the values the looks have.

#include "Sky/StarLookPresets.h"

#include "AssetCore/SharedFileRead.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Types/ParseNumber.h"
#include "Types/StringUtils.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

const char* ShapeName(Components::StarShapeKind shape)
{
    return shape == Components::StarShapeKind::Sparkle ? "Sparkle" : "Point";
}

// One `key=value` of a picture's line in star-pictures.txt, or empty when the line has no such key.
std::string_view RecordValue(std::string_view line, std::string_view key)
{
    for (std::string_view token : Split(line, ' '))
    {
        const size_t equals = token.find('=');
        if (equals != std::string_view::npos && token.substr(0, equals) == key)
            return token.substr(equals + 1);
    }
    return {};
}

void ExpectRecordedNumber(std::string_view line, std::string_view key, float expected)
{
    const std::optional<float> recorded = ParseFloat(RecordValue(line, key));
    ASSERT_TRUE(recorded.has_value()) << "no number for " << key << " in: " << line;
    EXPECT_NEAR(*recorded, expected, 1e-4f) << key << " in: " << line;
}

} // namespace

// The four looks in the Preset list's order, Vivid first, each with a line and a picture.
TEST(StarLookPresets, TheLooksAreVividRestrainedClassicAndPhysical)
{
    const auto presets = StarLookPresets();
    ASSERT_EQ(presets.size(), 4u);
    EXPECT_STREQ(presets[0].Name, "Vivid");
    EXPECT_STREQ(presets[1].Name, "Restrained");
    EXPECT_STREQ(presets[2].Name, "Classic");
    EXPECT_STREQ(presets[3].Name, "Physical");
    for (const StarLookPreset& preset : presets)
    {
        EXPECT_GT(std::strlen(preset.Description), 0u) << preset.Name;
        EXPECT_EQ(std::string(preset.PictureClass).rfind("option-picture--star-look-", 0), 0u) << preset.Name;
    }
}

// The engine's default is the Vivid look: a new Sky Environment and a new Post Process Volume carry its
// values, so a new scene matches the Vivid button.
TEST(StarLookPresets, TheComponentsDefaultsAreTheVividLook)
{
    const StarLookPreset& vivid = Look("Vivid");
    EXPECT_TRUE(MatchesStarLook(Components::SkyEnvironment{}, vivid));
    EXPECT_EQ(Components::SkyEnvironment{}.StarObserver, Components::StarObserverKind::Camera)
        << "a new sky is seen by a camera, so every star keeps its color";
    const Components::PostProcessVolume volume{};
    EXPECT_FLOAT_EQ(volume.NightTintStrength, vivid.NightTintStrength);
    EXPECT_FLOAT_EQ(volume.NightTintColor[0], vivid.NightTintColor.r);
    EXPECT_FLOAT_EQ(volume.NightTintColor[1], vivid.NightTintColor.g);
    EXPECT_FLOAT_EQ(volume.NightTintColor[2], vivid.NightTintColor.b);
    EXPECT_GT(vivid.Spikes, 0.0f) << "Vivid carries the diffraction spikes";
    EXPECT_LT(vivid.CoreScale, 1.0f) << "Vivid's core is tighter than the physical one";
}

// Physical is one pick to every artistic dial at its neutral value.
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

// The Preset row reads Custom when a star dial differs from every look, and names the look otherwise.
TEST(StarLookPresets, AHandChangedDialReadsCustom)
{
    ECS::World world;
    Components::SkyEnvironment sky{};
    ASSERT_NE(FindStarLook(sky, world), nullptr);
    EXPECT_STREQ(FindStarLook(sky, world)->Name, "Vivid");
    sky.StarGlow = 0.5f;
    EXPECT_EQ(FindStarLook(sky, world), nullptr) << "a dial no look has is Custom";
    sky.StarGlow = Look("Vivid").Glow;
    sky.StarShape = Components::StarShapeKind::Sparkle;
    EXPECT_EQ(FindStarLook(sky, world), nullptr);
}

// A hand-tuned night tint on a global volume reads Custom too, so picking the look again resets it.
TEST(StarLookPresets, AHandTunedNightTintReadsCustomAndTheLookResetsIt)
{
    ECS::World world;
    const ECS::EntityHandle sky = world.CreateEntity();
    world.AddComponentImmediate(sky, Components::SkyEnvironment{});
    const ECS::EntityHandle volume = world.CreateEntity();
    world.AddComponentImmediate(volume, Components::PostProcessVolume{});
    const Components::SkyEnvironment skyValues{};
    ASSERT_NE(FindStarLook(skyValues, world), nullptr);

    Components::PostProcessVolume tuned{};
    tuned.NightTintStrength = 0.6f;
    world.AddComponentImmediate(volume, tuned);
    EXPECT_EQ(FindStarLook(skyValues, world), nullptr) << "a tint no look has is Custom";
    tuned.NightTintStrength = Look("Vivid").NightTintStrength;
    tuned.NightTintColor[0] = 0.5f;
    world.AddComponentImmediate(volume, tuned);
    EXPECT_EQ(FindStarLook(skyValues, world), nullptr) << "a hue no look has is Custom";

    ApplyStarLookPreset(Look("Vivid"), world, sky, nullptr, nullptr);
    const StarLookPreset* after = FindStarLook(*world.GetComponent<Components::SkyEnvironment>(sky), world);
    ASSERT_NE(after, nullptr);
    EXPECT_STREQ(after->Name, "Vivid");
    EXPECT_FLOAT_EQ(world.GetComponent<Components::PostProcessVolume>(volume)->NightTintColor[0],
                    Look("Vivid").NightTintColor.r);
}

// Seen by and the preset are independent: a look applies its dials and leaves the observer, and the
// observer changes no dial, so a look stays selected with either observer.
TEST(StarLookPresets, ALookLeavesSeenByAndSeenByLeavesTheLook)
{
    ECS::World world;
    Editor::EditorChangeNotifications notifications;
    Editor::UndoRedoService undo;
    const ECS::EntityHandle sky = world.CreateEntity();
    Components::SkyEnvironment skyValues{};
    skyValues.StarObserver = Components::StarObserverKind::Camera;
    world.AddComponentImmediate(sky, skyValues);

    ApplyStarLookPreset(Look("Classic"), world, sky, &notifications, &undo);
    const auto* after = world.GetComponent<Components::SkyEnvironment>(sky);
    EXPECT_EQ(after->StarObserver, Components::StarObserverKind::Camera) << "a look never changes Seen by";
    EXPECT_EQ(after->StarShape, Components::StarShapeKind::Sparkle);
    ASSERT_NE(FindStarLook(*after, world), nullptr);
    EXPECT_STREQ(FindStarLook(*after, world)->Name, "Classic");

    const float exposureTrim = after->SkyExposureTrim;
    ApplyStarObserver(Components::StarObserverKind::Eye, world, sky, &notifications, &undo);
    EXPECT_EQ(undo.GetUndoCount(), 2u) << "a look and a Seen by pick are one undo step each";
    after = world.GetComponent<Components::SkyEnvironment>(sky);
    EXPECT_EQ(after->StarObserver, Components::StarObserverKind::Eye);
    EXPECT_TRUE(MatchesStarLook(*after, Look("Classic"))) << "Seen by changes no star dial";
    EXPECT_FLOAT_EQ(after->SkyExposureTrim, exposureTrim);
    ASSERT_NE(FindStarLook(*after, world), nullptr);
    EXPECT_STREQ(FindStarLook(*after, world)->Name, "Classic") << "the look stays selected with either observer";

    undo.Undo();
    EXPECT_EQ(world.GetComponent<Components::SkyEnvironment>(sky)->StarObserver, Components::StarObserverKind::Camera);
}

// Every look's picture has a line in star-pictures.txt with the values it was taken with, and each
// line's values are its look's: a retuned look fails here until its picture is retaken.
TEST(StarLookPresets, ThePicturesWereTakenWithTheLooksValues)
{
    const std::filesystem::path record =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets/Icons/OptionPictures/star-pictures.txt";
    std::string text;
    ASSERT_TRUE(ReadFileTextShared(record, text)) << record.generic_string() << " did not read";
    size_t checked = 0;
    for (std::string_view line : Split(text, '\n'))
    {
        if (!line.starts_with("star-") || line.find(".png: look=") == std::string_view::npos)
            continue;
        const std::string lookName(RecordValue(line, "look"));
        const StarLookPreset& look = Look(lookName.c_str());
        const std::string_view seenBy = line.starts_with("star-seen-by-eye.png: ") ? "Eye" : "Camera";
        EXPECT_EQ(RecordValue(line, "seen-by"), seenBy) << line;
        ExpectRecordedNumber(line, "limit", look.LimitingMagnitude);
        ExpectRecordedNumber(line, "twinkle", look.TwinkleAmount);
        ExpectRecordedNumber(line, "highlights", look.Compression);
        ExpectRecordedNumber(line, "glow", look.Glow);
        ExpectRecordedNumber(line, "twinkle-rate", look.TwinkleRate);
        ExpectRecordedNumber(line, "core", look.CoreScale);
        ExpectRecordedNumber(line, "spikes", look.Spikes);
        ExpectRecordedNumber(line, "spike-length", look.SpikeLength);
        ExpectRecordedNumber(line, "tint", look.NightTintStrength);
        EXPECT_EQ(RecordValue(line, "shape"), std::string_view(ShapeName(look.Shape))) << line;
        const std::vector<std::string_view> hue = Split(RecordValue(line, "tint-color"), ',');
        ASSERT_EQ(hue.size(), 3u) << line;
        EXPECT_NEAR(ParseFloat(hue[0]).value_or(-1.0f), look.NightTintColor.r, 1e-4f) << line;
        EXPECT_NEAR(ParseFloat(hue[1]).value_or(-1.0f), look.NightTintColor.g, 1e-4f) << line;
        EXPECT_NEAR(ParseFloat(hue[2]).value_or(-1.0f), look.NightTintColor.b, 1e-4f) << line;
        ++checked;
    }
    for (const StarLookPreset& look : StarLookPresets())
    {
        const std::string picture = std::string(look.PictureClass).substr(std::strlen("option-picture--")) + ".png: ";
        EXPECT_NE(text.find("\n" + picture), std::string::npos) << "no values recorded for " << picture;
    }
    EXPECT_EQ(checked, StarLookPresets().size() + 2) << "one line per look picture and per Seen by picture";
}
