#include "Components/Spline/SplineExtrude.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/UnresolvedComponentStore.h"
#include "SplineRecipes/SplineExtrudeSanitize.h"
#include "Scene/SceneIO.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>

using namespace GameEngine;
using Components::MaterialRef;
using Components::SplineExtrude;
using Components::SplineExtrudeProfile;
using Components::SplineExtrudeWidthMode;
using Components::SplineExtrudeWidthScale;
using SplineRecipes::SanitizeExtrudeRecipe;

namespace
{

MaterialRef MaterialTestRef(uint8 ordinal)
{
    GUID::Data data{};
    data[0] = ordinal;
    data[15] = 0xC3;
    return MaterialRef(GUID(data));
}

} // namespace

TEST(SplineExtrudeRecipe, ComponentSizeStaysEcsFriendly)
{
    // The recipe rides an ECS chunk column. It is authored data, so it should
    // stay in the tens of bytes -- a jump here means someone embedded something
    // that belongs behind a handle.
    EXPECT_LE(sizeof(SplineExtrude), 128u);
    static_assert(std::is_trivially_copyable_v<SplineExtrude>);
    static_assert(std::is_standard_layout_v<SplineExtrude>);
}

// THE trap both sibling recipe designs name: the rebuild scheduler compares
// whole recipes, so a field missing from operator== does not fail loudly -- it
// silently stops regenerating geometry for edits to that field. The comparison
// is defaulted precisely so it cannot miss one; this test is what keeps a
// future hand-written replacement honest.
TEST(SplineExtrudeRecipe, EveryFieldParticipatesInEquality)
{
    const SplineExtrude base{};
    EXPECT_TRUE(base == SplineExtrude{});

    struct Case
    {
        const char* Name;
        void (*Flip)(SplineExtrude&);
    };
    // If this fires you added a field: add it to the flip table below (and, if
    // it is a float, to SanitizeExtrudeRecipe), then re-baseline the size.
    static_assert(sizeof(SplineExtrude) == 96u,
                  "SplineExtrude changed size: update the flip table beside this assert and "
                  "SanitizeExtrudeRecipe before re-baselining");
    const Case cases[] = {
        {"Profile", [](SplineExtrude& e) { e.Profile = SplineExtrudeProfile::Crown; }},
        {"Width", [](SplineExtrude& e) { e.Width = 7.5f; }},
        {"EdgeDrop", [](SplineExtrude& e) { e.EdgeDrop = 0.4f; }},
        {"EdgeInset", [](SplineExtrude& e) { e.EdgeInset = 0.55f; }},
        {"CrownRise", [](SplineExtrude& e) { e.CrownRise = 0.31f; }},
        {"ShoulderWidth", [](SplineExtrude& e) { e.ShoulderWidth = 1.4f; }},
        {"ShoulderDrop", [](SplineExtrude& e) { e.ShoulderDrop = 0.62f; }},
        {"WidthScale", [](SplineExtrude& e) { e.WidthScale = SplineExtrudeWidthScale::Uniform; }},
        {"WidthMode",
         [](SplineExtrude& e) { e.WidthMode = SplineExtrudeWidthMode::FitToBanks; }},
        {"MaxHalfWidth", [](SplineExtrude& e) { e.MaxHalfWidth = 21.5f; }},
        {"FloorHeight", [](SplineExtrude& e) { e.FloorHeight = 2.0f; }},
        {"EndTaperMetres", [](SplineExtrude& e) { e.EndTaperMetres = 6.25f; }},
        {"ConformMode",
         [](SplineExtrude& e) { e.ConformMode = Components::SplinePlacementConform::None; }},
        {"ConformTarget",
         [](SplineExtrude& e)
         { e.ConformTarget = Components::SplineConformTarget::TerrainOnly; }},
        {"LateralOffset", [](SplineExtrude& e) { e.LateralOffset = -1.75f; }},
        {"VerticalOffset", [](SplineExtrude& e) { e.VerticalOffset = 0.08f; }},
        {"TilesPerMetreU", [](SplineExtrude& e) { e.TilesPerMetreU = 0.35f; }},
        {"TilesPerMetreV", [](SplineExtrude& e) { e.TilesPerMetreV = 2.5f; }},
        {"Material", [](SplineExtrude& e) { e.Material = MaterialTestRef(9); }},
        {"GroundAuthority",
         [](SplineExtrude& e)
         { e.GroundAuthority = Components::SplineGroundAuthority::None; }},
        {"CastShadows", [](SplineExtrude& e) { e.CastShadows = false; }},
        {"ReceiveShadows", [](SplineExtrude& e) { e.ReceiveShadows = false; }},
    };

    for (const Case& c : cases)
    {
        SplineExtrude flipped = base;
        c.Flip(flipped);
        EXPECT_FALSE(flipped == base) << c.Name << " does not participate in operator==";
    }
}

TEST(SplineExtrudeRecipe, DefaultsDescribeAUsablePathOnTheFirstTry)
{
    const SplineExtrude recipe{};

    // Dropping the recipe on a spline should produce a conforming path, not an
    // invisible or degenerate one.
    EXPECT_EQ(recipe.Profile, SplineExtrudeProfile::Bevel);
    EXPECT_GT(recipe.Width, 0.0f);
    EXPECT_NE(recipe.ConformMode, Components::SplinePlacementConform::None);
    EXPECT_EQ(recipe.WidthScale, SplineExtrudeWidthScale::LateralOnly);
    // Width comes from the authored channel unless the author opts into
    // measuring it, so an existing scene renders exactly as it did.
    EXPECT_EQ(recipe.WidthMode, SplineExtrudeWidthMode::Channel);
    EXPECT_GT(recipe.MaxHalfWidth, 0.0f);
    // Ends stay square unless asked otherwise, so an existing run keeps the
    // geometry it had rather than growing a pinch nobody authored.
    EXPECT_FLOAT_EQ(recipe.EndTaperMetres, 0.0f);
    EXPECT_GT(recipe.TilesPerMetreU, 0.0f);
    EXPECT_GT(recipe.TilesPerMetreV, 0.0f);
    // No material means the controller mints its own default rather than
    // rendering nothing.
    EXPECT_EQ(recipe.Material.ToGuid(), GUID::Null());
    // The floor height is OFF by default, and "off" is the arithmetic
    // identity of the clamp that applies it rather than a magic value compared
    // for equality -- so it survives a scene round-trip without special casing.
    EXPECT_EQ(recipe.FloorHeight, Components::kNoFloorHeight);
    EXPECT_TRUE(std::isfinite(recipe.FloorHeight))
        << "a non-finite default would make the recipe unequal to itself and re-arm the "
           "settle rebuild forever";
    EXPECT_EQ(std::max(-12.5f, recipe.FloorHeight), -12.5f)
        << "the disabled floor must leave any waterline untouched";
}

// The trap the defaulted operator== creates: a NaN field makes a recipe unequal
// to ITSELF, and the settle-timer rebuild would then re-arm every frame for as
// long as the field stayed poisoned. Sanitizing at candidate collection is the
// single gate, exactly as for the fence sibling.
TEST(SplineExtrudeRecipe, NaNRecipeIsSelfUnequalUntilSanitized)
{
    SplineExtrude poisoned{};
    poisoned.Width = std::numeric_limits<float32>::quiet_NaN();
    // The hazard, stated so it cannot be argued away: this is what would loop.
    EXPECT_FALSE(poisoned == poisoned);

    const SplineExtrude clean = SplineRecipes::SanitizeExtrudeRecipe(poisoned);
    EXPECT_TRUE(clean == clean);
    EXPECT_TRUE(clean == SplineRecipes::SanitizeExtrudeRecipe(poisoned));
    EXPECT_FLOAT_EQ(clean.Width, SplineExtrude{}.Width);
}

TEST(SplineExtrudeRecipe, SanitizeRepairsEveryFloatAndKeepsTheRest)
{
    const float32 nan = std::numeric_limits<float32>::quiet_NaN();
    const float32 inf = std::numeric_limits<float32>::infinity();
    const SplineExtrude defaults{};

    // Every float field poisoned: a sanitizer that misses one leaves the
    // self-inequality loop reachable through exactly that field.
    SplineExtrude poisoned{};
    poisoned.Width = nan;
    poisoned.EdgeDrop = -nan;
    poisoned.EdgeInset = -inf;
    poisoned.CrownRise = nan;
    poisoned.ShoulderWidth = inf;
    poisoned.ShoulderDrop = nan;
    poisoned.MaxHalfWidth = -inf;
    poisoned.EndTaperMetres = nan;
    poisoned.LateralOffset = inf;
    poisoned.VerticalOffset = nan;
    poisoned.TilesPerMetreU = inf;
    poisoned.TilesPerMetreV = nan;
    poisoned.Profile = SplineExtrudeProfile::Crown;
    poisoned.Material = MaterialTestRef(5);
    poisoned.CastShadows = false;

    const SplineExtrude clean = SplineRecipes::SanitizeExtrudeRecipe(poisoned);
    // A non-finite value carries no authored intent, so it falls back to the
    // default rather than being clamped into range.
    EXPECT_FLOAT_EQ(clean.Width, defaults.Width);
    EXPECT_FLOAT_EQ(clean.EdgeDrop, defaults.EdgeDrop);
    EXPECT_FLOAT_EQ(clean.EdgeInset, defaults.EdgeInset);
    EXPECT_FLOAT_EQ(clean.CrownRise, defaults.CrownRise);
    EXPECT_FLOAT_EQ(clean.ShoulderWidth, defaults.ShoulderWidth);
    EXPECT_FLOAT_EQ(clean.ShoulderDrop, defaults.ShoulderDrop);
    EXPECT_FLOAT_EQ(clean.MaxHalfWidth, defaults.MaxHalfWidth);
    EXPECT_FLOAT_EQ(clean.EndTaperMetres, defaults.EndTaperMetres);
    EXPECT_FLOAT_EQ(clean.LateralOffset, defaults.LateralOffset);
    EXPECT_FLOAT_EQ(clean.VerticalOffset, defaults.VerticalOffset);
    EXPECT_FLOAT_EQ(clean.TilesPerMetreU, defaults.TilesPerMetreU);
    EXPECT_FLOAT_EQ(clean.TilesPerMetreV, defaults.TilesPerMetreV);
    // The repaired recipe is self-equal — buildable by the settle scheduler.
    EXPECT_TRUE(clean == clean);
    // Untouched fields survive verbatim.
    EXPECT_EQ(clean.Profile, SplineExtrudeProfile::Crown);
    EXPECT_EQ(clean.Material.ToGuid(), poisoned.Material.ToGuid());
    EXPECT_FALSE(clean.CastShadows);
    EXPECT_TRUE(clean.ReceiveShadows);
}

TEST(SplineExtrudeRecipe, TheWaterCaseIsAuthorableWithoutANewProfileShape)
{
    // A water ribbon is a flat cross-section with a material and a small lift
    // off the bed. Zeroing the bevel's drop and inset collapses it to exactly
    // the two-point strip that describes, so no fourth preset is needed and
    // the difference between a footpath and a river is authored, not compiled.
    SplineExtrude water{};
    water.Profile = SplineExtrudeProfile::Bevel;
    water.EdgeDrop = 0.0f;
    water.EdgeInset = 0.0f;
    water.VerticalOffset = 0.05f;
    water.Material = MaterialTestRef(3);

    EXPECT_FALSE(water == SplineExtrude{});
    EXPECT_GT(water.VerticalOffset, 0.0f);
    EXPECT_NE(water.Material.ToGuid(), GUID::Null());
}

// An unmarked field shows no tooltip, so ConformTarget's tooltip depends on its "@ge-tooltip" marker, the
// component scanner and the reflection registry. This asserts what the registry holds at runtime.
TEST(SplineExtrudeRecipe, ConformTargetCarriesItsAuthorTooltipThroughReflection)
{
    const ECS::ComponentTypeId typeId = ECS::GetComponentTypeId<SplineExtrude>();
    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    ASSERT_FALSE(fields.empty())
        << "SplineExtrude is not reflected at all, so no inspector row of any kind exists";

    const auto field = std::find_if(fields.begin(), fields.end(),
                                    [](const ECS::FieldInfo& f) { return f.Name == "ConformTarget"; });
    ASSERT_NE(field, fields.end()) << "ConformTarget is not a reflected field";

    EXPECT_FALSE(field->Tooltip.empty())
        << "ConformTarget reaches the author as a bare row: the @ge-tooltip marker in "
           "SplineExtrude.h is no longer reaching FieldInfo::Tooltip";

    // The clause that matters is the divergence warning — the drawn drape line
    // does not read this setting, so a TerrainOnly ribbon and its preview
    // disagree until the gizmo routes a target. A tooltip that lost this is a
    // tooltip that stopped doing the job it was added for.
    EXPECT_NE(field->Tooltip.find("does not read this setting"), std::string_view::npos)
        << "the tooltip no longer warns that the drape line ignores the conform target; it reads: "
        << field->Tooltip;

    // Every row of the extrude explains itself.
    for (const ECS::FieldInfo& each : fields)
        EXPECT_FALSE(each.Tooltip.empty()) << each.Name << " has no tooltip";

    // Positive control: an unmarked field must still be bare, or the assertions
    // above would pass on a registry that hands every field a string.
    const std::span<const ECS::FieldInfo> provisioned =
        ECS::ComponentFieldRegistry::Get(ECS::GetComponentTypeId<Components::SplineGroundProvisioned>());
    const auto unmarked = std::find_if(provisioned.begin(), provisioned.end(),
                                       [](const ECS::FieldInfo& f) { return f.Name == "Flatten"; });
    ASSERT_NE(unmarked, provisioned.end());
    EXPECT_TRUE(unmarked->Tooltip.empty())
        << "an unmarked field carries tooltip text, so this test cannot tell a harvested marker "
           "from a default";
}

// Only a kept Profile line reading Rectangle marks an extrude as the retired
// wall: another kept field, or another value, does not. The report names the
// component to use instead.
TEST(SplineExtrudeRecipe, OnlyAKeptRectangleProfileMarksTheRetiredWall)
{
    ECS::World world;
    const ECS::EntityHandle run = world.CreateEntity();
    world.AddComponentImmediate(run, SplineExtrude{});
    EXPECT_FALSE(SplineRecipes::CarriesRetiredWallProfile(world, run));

    ECS::PreservedField otherField;
    otherField.Component = "SplineExtrude";
    otherField.Field = "Width";
    otherField.RawText = "Rectangle";
    world.GetUnresolvedComponents().AddField(run, otherField);
    ECS::PreservedField otherValue;
    otherValue.Component = "SplineExtrude";
    otherValue.Field = "Profile";
    otherValue.RawText = "Hexagon";
    world.GetUnresolvedComponents().AddField(run, otherValue);
    EXPECT_FALSE(SplineRecipes::CarriesRetiredWallProfile(world, run));

    ECS::PreservedField retired;
    retired.Component = "SplineExtrude";
    retired.Field = "Profile";
    retired.RawText = "Rectangle";
    world.GetUnresolvedComponents().AddField(run, retired);
    EXPECT_TRUE(SplineRecipes::CarriesRetiredWallProfile(world, run));
    EXPECT_NE(SplineRecipes::RetiredWallProfileValidation().find("Replace it with a Spline Wall"),
              std::string::npos);
}

// Choosing a profile for a run saved with the retired Rectangle supersedes the
// kept text, as it does for the next save: the run is no longer the retired
// wall, so it builds and grades as the profile chosen.
TEST(SplineExtrudeRecipe, ChoosingAProfileRevivesARunSavedWithTheRetiredWall)
{
    const TestUtils::ScopedTempDir sceneDir{TestUtils::MakeUniqueTempDirectory("SplineExtrudeRecipe")};
    const std::filesystem::path scenePath = sceneDir.Path() / "SplineExtrudeRecipe_retired_wall_revived.scene";
    {
        std::ofstream out(scenePath, std::ios::binary | std::ios::trunc);
        out << "[scene name=\"RetiredRectangle\" version=1]\n"
               "\n"
               "[entity id=\"rampart\"]\n"
               "SplineExtrude.Profile = Rectangle\n"
               "SplineExtrude.Width = 0.8\n";
    }
    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    std::filesystem::remove(scenePath);

    ECS::EntityHandle run{};
    world.Query<ECS::Read<SplineExtrude>>().Each([&](ECS::EntityHandle e, const SplineExtrude&) { run = e; });
    ASSERT_TRUE(run.IsValid());
    const std::vector<ECS::PreservedField>* kept = world.GetUnresolvedComponents().FieldsFor(run);
    ASSERT_NE(kept, nullptr);
    ASSERT_EQ(kept->size(), 1u);
    ASSERT_NE(kept->front().FieldSize, 0u) << "the kept Profile has no byte range to compare";
    EXPECT_TRUE(SplineRecipes::CarriesRetiredWallProfile(world, run));

    // Positive control: a write to a sibling field leaves the kept Profile standing.
    world.GetComponentForWrite<SplineExtrude>(run)->Width = 1.2f;
    EXPECT_TRUE(SplineRecipes::CarriesRetiredWallProfile(world, run));

    world.GetComponentForWrite<SplineExtrude>(run)->Profile = SplineExtrudeProfile::Crown;
    EXPECT_FALSE(SplineRecipes::CarriesRetiredWallProfile(world, run));
}
