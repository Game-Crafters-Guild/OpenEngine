// The Inspector's derived-sizing wording. Size and Samples Per Meter are two raw float drags
// that between them decide resolution, tile grid, height source, VRAM and how much of the
// terrain stays sharp — and an author could previously set a legal pair and get a terrain that
// rendered nothing. These oracles pin that the notice states the consequence in the author's
// units, and that the partially-resident case says so rather than reading as free detail.

#include <gtest/gtest.h>

#include "Terrain/TerrainSizingNotice.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/TerrainPages.h"
#include "TerrainECS/TerrainSizingPlan.h"
#include "UI/UIElement.h"

#include <regex>
#include <string>

using GameEngine::uint64;
using GameEngine::UIElement;
using GameEngine::Editor::AddTerrainSizingNotice;
using GameEngine::Editor::DescribeTerrainResidency;
using GameEngine::Editor::DescribeTerrainSizing;
using GameEngine::TerrainECS::DeriveTerrainSizingPlan;
using GameEngine::TerrainECS::kPinWholePyramidCacheFraction;
using GameEngine::TerrainECS::PlatformHeightPageProfile;
using GameEngine::TerrainECS::TerrainHeightSource;
using GameEngine::TerrainECS::TerrainSizingPlan;

namespace
{
bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}
} // namespace

// A small terrain whose whole height pyramid fits a quarter of the platform's page cache does not
// stream, so the author is told it fits in memory. On Steam Deck the pyramid streams.
TEST(TerrainSizingNotice, ASmallTerrainReportsItsResolutionAndThatNothingStreams)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(1024.0f, 1024.0f, 1.0f);
    const std::string text = DescribeTerrainSizing(plan);
    const uint64 pinCap = PlatformHeightPageProfile().Slots / kPinWholePyramidCacheFraction;

    EXPECT_TRUE(Contains(text, "1,025 x 1,025")) << text;
    if (plan.HeightPages <= pinCap)
        EXPECT_TRUE(Contains(text, " - fits in memory, nothing streams (120 pages)")) << text;
    else
        EXPECT_TRUE(Contains(text, "1 x 1 streamed tiles")) << text;
}

// A 576 x 300 m terrain is two 300 m tiles of 513 samples: 1025 samples along X, 513 along Z.
TEST(TerrainSizingNotice, ANonSquareTerrainReportsTheSampleCountOfEachAxis)
{
    const std::string text = DescribeTerrainSizing(DeriveTerrainSizingPlan(576.0f, 300.0f, 1.0f));

    EXPECT_TRUE(Contains(text, "1,025 x 513 samples")) << text;
}

// Two 300 m tiles store samples for 600 m along X, past the authored 576 m, so the notice states that
// memory and the tile grid first, where a short panel shows it. The terrain draws only 576 m, so no
// notice states a drawn size. Four 300 m tiles cover 1200 m exactly, so that notice states no stored size.
TEST(TerrainSizingNotice, ATileGridPastTheAuthoredSizeReportsTheStoredSizeFirst)
{
    const std::string overhang = DescribeTerrainSizing(DeriveTerrainSizingPlan(576.0f, 300.0f, 1.0f));
    const std::string exact = DescribeTerrainSizing(DeriveTerrainSizingPlan(1200.0f, 300.0f, 1.0f));

    EXPECT_TRUE(overhang.starts_with(
        "Stores samples for 600 m x 300 m (2 x 1 tiles of 300 m), more than it draws. 1,025 x 513 samples"))
        << overhang;
    EXPECT_TRUE(exact.starts_with("2,049 x 513 samples")) << exact;
    EXPECT_FALSE(Contains(overhang, "drawn")) << overhang;
    EXPECT_FALSE(Contains(exact, "Stores")) << exact;
    EXPECT_FALSE(Contains(exact, "drawn")) << exact;
}

TEST(TerrainSizingNotice, ATiledTerrainReportsItsTileGrid)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(4096.0f, 4096.0f, 2.0f);
    const std::string text = DescribeTerrainSizing(plan);

    ASSERT_EQ(plan.Source, TerrainHeightSource::Unified);
    EXPECT_TRUE(Contains(text, "8 x 8 streamed tiles")) << text;
}

// Detail is quoted in meters per sample because that is the unit an author reasons in ("0.5 m
// per sample on an 8 km island"), not samples per meter, and never raw sample counts alone.
TEST(TerrainSizingNotice, DetailIsQuotedInMetersPerSample)
{
    EXPECT_TRUE(Contains(DescribeTerrainSizing(DeriveTerrainSizingPlan(4096.0f, 4096.0f, 2.0f)),
                         "0.5 m per sample"));
    EXPECT_TRUE(Contains(DescribeTerrainSizing(DeriveTerrainSizingPlan(4096.0f, 4096.0f, 0.5f)),
                         "2 m per sample"));
}

TEST(TerrainSizingNotice, AFullyResidentTerrainSaysAllOfItIsAtFullDetail)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(2048.0f, 2048.0f, 1.0f);
    const std::string text = DescribeTerrainResidency(plan);

    ASSERT_TRUE(plan.FullyResident());
    EXPECT_TRUE(Contains(text, "full detail")) << text;
    EXPECT_TRUE(Contains(text, "MiB")) << text;
    EXPECT_FALSE(Contains(text, "falls back")) << text;
}

// Engaging the atlas is NOT the same thing as losing detail. One metre past the unified ceiling
// (4097 m at 2 samples/m) the slot budget still covers all 81 tiles, so the terrain streams but
// nothing is coarse — and the notice must not tell the author their distant ground reads smooth
// when there is no far field at all.
TEST(TerrainSizingNotice, AFullyResidentAtlasDoesNotClaimAnythingFallsBack)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(4097.0f, 4097.0f, 2.0f);
    const std::string text = DescribeTerrainResidency(plan);

    ASSERT_EQ(plan.Source, TerrainHeightSource::Atlas) << "one metre past the ceiling";
    ASSERT_TRUE(plan.FullyResident()) << "81 tiles, 81 slots";

    // Assert the CONTRADICTION is absent, not the bare phrase "falls back": the correct wording
    // legitimately contains those two words as a negation ("Nothing falls back to the coarse
    // field"), so a substring test on them fails for the same reason it would pass.
    EXPECT_FALSE(Contains(text, "Beyond the window"))
        << "there is no outside at this size — " << text;
    EXPECT_FALSE(Contains(text, "reads smooth rather than detailed")) << text;
    EXPECT_TRUE(Contains(text, "all 81 tiles")) << text;
    EXPECT_TRUE(Contains(text, "Nothing falls back")) << text;

    // CONTROL: the partially-resident wording must still say the opposite, or the assertions
    // above would hold for a notice that simply never mentions the far field.
    const std::string island =
        DescribeTerrainResidency(DeriveTerrainSizingPlan(8192.0f, 8192.0f, 2.0f));
    EXPECT_TRUE(Contains(island, "Beyond the window")) << island;
    EXPECT_FALSE(Contains(island, "Nothing falls back")) << island;
}

// THE CALL the notice exists to surface: past the unified ceiling most of an island is coarse,
// and the author has to know that before they commit to the size.
TEST(TerrainSizingNotice, AnIslandScaleTerrainStatesTheCoarseFarFieldItFallsBackTo)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(8192.0f, 8192.0f, 2.0f);
    const std::string text = DescribeTerrainResidency(plan);

    ASSERT_EQ(plan.Source, TerrainHeightSource::Atlas);
    ASSERT_FALSE(plan.FullyResident());
    EXPECT_TRUE(Contains(text, "of 256 tiles")) << text;
    EXPECT_TRUE(Contains(text, "falls back")) << text;
    // 8192 m over a 128-texel coarse field spanning the whole terrain = 8192/127.
    EXPECT_TRUE(Contains(text, "64.5 m per texel"))
        << "the far-field density must be stated as a number, not implied: " << text;
}

TEST(TerrainSizingNotice, ADegenerateTerrainProducesNoWordingRatherThanNonsense)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(0.0f, 0.0f, 1.0f);

    EXPECT_TRUE(DescribeTerrainSizing(plan).empty());
    EXPECT_TRUE(DescribeTerrainResidency(plan).empty());
}

// Counts and lengths group their digits as the size refusal does: "250,001", not "250001".
TEST(TerrainSizingNotice, ALargeTerrainGroupsTheDigitsOfEveryNumber)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(250000.0f, 250000.0f, 1.0f);
    const std::string text = DescribeTerrainSizing(plan) + " " + DescribeTerrainResidency(plan);
    const std::regex ungrouped("[0-9]{4}");

    ASSERT_FALSE(DescribeTerrainResidency(plan).empty());
    EXPECT_FALSE(std::regex_search(text, ungrouped)) << text;
}

// A size that creation refuses has no tiles and no memory, so the panel shows only the refusal
// notice and no summary rows. The control: a valid size adds the rows.
TEST(TerrainSizingNotice, ARefusedSizeAddsNoSummaryRows)
{
    UIElement refused;
    AddTerrainSizingNotice(&refused, 2000000.0f, 2000000.0f, 1.0f);
    UIElement valid;
    AddTerrainSizingNotice(&valid, 8192.0f, 8192.0f, 2.0f);

    EXPECT_TRUE(refused.GetChildren().empty());
    EXPECT_EQ(valid.GetChildren().size(), 2u);
}
