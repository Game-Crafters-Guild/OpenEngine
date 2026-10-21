// A pending asset's inspector header: the name, then "preparing…" right after it, then the row's
// free space, at the inspector's default width and at a wide and a narrow one. Lays the real
// labels (MakePendingAssetHeaderLabels) out over the real editor stylesheets, beside a fixed-size
// stand-in for the header's history buttons.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "Inspectors/PendingAssetHeader.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <memory>
#include <string>

using GameEngine::Label;
using GameEngine::UIElement;
using InspectorLayoutTesting::InspectorLayoutFixture;

namespace
{

// Layout rounds to a fraction of a pixel.
constexpr float kLayoutTolerance = 0.5f;
// The header row's gap between children (inspector.css, .inspector-header-container).
constexpr float kRowGap = 8.0f;
constexpr float kHistoryWidth = 36.0f;
const std::string kName = "T_Farm_GenTree_Bark_01_C";

struct HeaderLayout
{
    float NameX = 0.0f;
    float NameWidth = 0.0f;
    float SuffixX = 0.0f;
    float SuffixWidth = 0.0f;
    float HistoryX = 0.0f;
    std::string Diagnostic;
};

// Lays the header row out at `rowWidth` and reads the three children back.
bool LayOut(uint32_t rowWidth, HeaderLayout& out)
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("inspector-header-container");
    GameEngine::Editor::PendingAssetHeaderLabels labels = GameEngine::Editor::MakePendingAssetHeaderLabels(kName);
    labels.Suffix->SetText("preparing…");
    labels.Suffix->RemoveClass("hidden");
    Label* name = labels.Name.get();
    Label* suffix = labels.Suffix.get();
    auto history = std::make_unique<UIElement>();
    history->Overrides()
        .Set(GameEngine::Style::Width, GameEngine::StyleLength::Px(kHistoryWidth))
        .Set(GameEngine::Style::FlexShrink, 0.0f);
    UIElement* historyRaw = history.get();
    row->AddChild(std::move(labels.Name));
    row->AddChild(std::move(labels.Suffix));
    row->AddChild(std::move(history));

    InspectorLayoutFixture fixture;
    fixture.ViewportW = rowWidth;
    if (!fixture.Build(std::move(row)))
    {
        out.Diagnostic = fixture.Diagnostic;
        return false;
    }
    out.NameX = name->GetLayoutX();
    out.NameWidth = name->GetLayoutWidth();
    out.SuffixX = suffix->GetLayoutX();
    out.SuffixWidth = suffix->GetLayoutWidth();
    out.HistoryX = historyRaw->GetLayoutX();
    return true;
}

} // namespace

TEST(PendingAssetHeaderLayout, TheNameFitsItsTextAndTheSuffixFollowsIt)
{
    // Wide: the name is as wide as its text and the row's free space is after the suffix, so the
    // suffix reads as this asset's state, not as a status pinned beside the history buttons.
    HeaderLayout wide;
    if (!LayOut(599, wide))
        GTEST_SKIP() << wide.Diagnostic;
    ASSERT_GT(wide.NameWidth, 0.0f) << "the name measured to nothing; the fixture has no font";
    ASSERT_GT(wide.SuffixWidth, 0.0f);
    EXPECT_NEAR(wide.SuffixX, wide.NameX + wide.NameWidth + kRowGap, kLayoutTolerance);
    EXPECT_GT(wide.HistoryX - (wide.SuffixX + wide.SuffixWidth), 4.0f * kRowGap)
        << "the free space lies after the suffix";

    // At the default width this stand-in row (a 36 px history slot, no icon, no toggle) still holds
    // the whole name; the editor header at 383 px ellipsises it, as the live check shows.
    HeaderLayout standard;
    ASSERT_TRUE(LayOut(383, standard)) << standard.Diagnostic;
    EXPECT_NEAR(standard.NameWidth, wide.NameWidth, kLayoutTolerance) << "the name sizes to its text";
    EXPECT_NEAR(standard.SuffixX, standard.NameX + standard.NameWidth + kRowGap, kLayoutTolerance);
}

TEST(PendingAssetHeaderLayout, WhereTheRowIsShortTheNameGivesWayAndTheSuffixStaysWhole)
{
    HeaderLayout wide;
    if (!LayOut(599, wide))
        GTEST_SKIP() << wide.Diagnostic;
    HeaderLayout narrow;
    ASSERT_TRUE(LayOut(240, narrow)) << narrow.Diagnostic;
    EXPECT_LT(narrow.NameWidth, wide.NameWidth) << "the name shrinks (and ellipsises)";
    EXPECT_NEAR(narrow.SuffixWidth, wide.SuffixWidth, kLayoutTolerance) << "the suffix never shrinks";
    EXPECT_NEAR(narrow.SuffixX, narrow.NameX + narrow.NameWidth + kRowGap, kLayoutTolerance);
    EXPECT_LE(narrow.SuffixX + narrow.SuffixWidth, narrow.HistoryX + kLayoutTolerance)
        << "the suffix stays clear of the history buttons";
}
