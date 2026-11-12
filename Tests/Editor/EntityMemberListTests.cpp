// The shared entity member list: the rows in the caller's order with the caller's toggle state, each
// gesture one call of the caller's handler with the row's entity, the mode as segments or as one label,
// the deleted member's notice, and a note that wraps under the title when it does not fit beside it.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "UI/Controls/EntityMemberList/EntityMemberList.h"

#include "ECS/World.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using GameEngine::Button;
using GameEngine::Label;
using GameEngine::Toggle;
using GameEngine::UIElement;
using GameEngine::UIEvent;
using GameEngine::uint8;
using GameEngine::ECS::EntityHandle;
using GameEngine::EditorUI::EntityMemberList;
using InspectorLayoutTesting::FindByClass;
using InspectorLayoutTesting::InspectorLayoutFixture;

namespace
{

constexpr uint8 kKeep = 0;
constexpr uint8 kCut = 1;

// The control's sheet and the shared control rules it relies on, in the order the editor attaches them.
const std::vector<std::string> kListSheets{GE_UI_MODULE_SOURCE_DIR "/Assets/UI/controls/Button.css",
                                           GE_UI_MODULE_SOURCE_DIR "/Assets/UI/controls/Toggle.css",
                                           "Assets/UI/controls/EntityMemberList/EntityMemberList.css"};

// Layout rounds to a fraction of a pixel.
constexpr float kLayoutTolerance = 0.5f;
// The row's right padding, between the mode and the row's end.
constexpr float kRowPadding = 8.0f;
// Button.css: a segment's left and right padding together.
constexpr float kSegmentPadding = 16.0f;

std::vector<EntityMemberList::Mode> TwoModes()
{
    return {{kKeep, "Keep", "Keeps it", false}, {kCut, "Cut", "Cuts it", true}};
}

// A member of the owner's list, switched on.
EntityMemberList::Row MakeRow(EntityHandle entity, std::string title, uint8 mode = kKeep)
{
    EntityMemberList::Row row;
    row.Entity = entity;
    row.On = true;
    row.Member = true;
    row.Title = std::move(title);
    row.Modes = TwoModes();
    row.CurrentMode = mode;
    return row;
}

// An entity the owner offers and does not hold, switched off.
EntityMemberList::Row MakeCandidate(EntityHandle entity, std::string title)
{
    EntityMemberList::Row row = MakeRow(entity, std::move(title));
    row.On = false;
    row.Member = false;
    return row;
}

EntityMemberList::Content MakeContent()
{
    EntityMemberList::Content content;
    content.Text.Heading = "Members";
    content.Text.Empty = "Nothing overlaps";
    content.Text.DeletedTitle = "Deleted thing";
    content.Text.MemberTooltip = "A member";
    content.Text.CandidateTooltip = "Not a member";
    return content;
}

// What the handlers were called with.
struct Calls
{
    std::vector<EntityHandle> SwitchedOn;
    std::vector<EntityHandle> SwitchedOff;
    std::vector<std::pair<EntityHandle, uint8>> Switched;
    std::vector<EntityHandle> Selected;
};

EntityMemberList::Handlers Record(Calls& calls)
{
    EntityMemberList::Handlers handlers;
    handlers.SetOn = [&calls](EntityHandle entity, bool on) {
        (on ? calls.SwitchedOn : calls.SwitchedOff).push_back(entity);
    };
    handlers.SetMode = [&calls](EntityHandle entity, uint8 mode) { calls.Switched.emplace_back(entity, mode); };
    handlers.Select = [&calls](EntityHandle entity) { calls.Selected.push_back(entity); };
    return handlers;
}

std::vector<UIElement*> Rows(UIElement& list)
{
    std::vector<UIElement*> rows;
    for (const auto& child : list.GetChildren())
    {
        if (child->HasClass("entity-member-row"))
            rows.push_back(child.get());
    }
    return rows;
}

template <typename T>
T* Part(UIElement& row, const char* className)
{
    return dynamic_cast<T*>(FindByClass(&row, className));
}

// The row's mode segment at `index`, in the order of the row's modes.
Button* Segment(UIElement& row, std::size_t index)
{
    UIElement* modes = FindByClass(&row, "entity-member-mode");
    if (!modes)
        return nullptr;
    std::size_t found = 0;
    for (const auto& child : modes->GetChildren())
    {
        auto* segment = dynamic_cast<Button*>(child.get());
        if (segment && segment->HasClass("entity-member-segment") && found++ == index)
            return segment;
    }
    return nullptr;
}

void MouseUp(UIElement& target)
{
    UIEvent event{};
    event.Id = GameEngine::kEventMouseUp;
    event.Target = &target;
    event.CurrentTarget = &target;
    event.Button = 0;
    target.DispatchEvent(event);
}

float Right(const UIElement& element)
{
    return element.GetLayoutX() + element.GetLayoutWidth();
}

float Bottom(const UIElement& element)
{
    return element.GetLayoutY() + element.GetLayoutHeight();
}

class EntityMemberListTest : public ::testing::Test
{
  protected:
    GameEngine::ECS::World World;
    EntityHandle First = World.CreateEntity();
    EntityHandle Second = World.CreateEntity();
};

} // namespace

// The rows keep the caller's order, a row switched off dimmed; switching a row calls SetOn with the
// row's entity and the new value; a click on a title selects. The glyph takes the caller's classes,
// and a row without them shows none.
TEST_F(EntityMemberListTest, RowsKeepTheCallersOrderAndTheTogglesCallSetOn)
{
    Calls calls;
    EntityMemberList::Content content = MakeContent();
    content.Rows.push_back(MakeRow(Second, "Pond"));
    content.Rows.back().GlyphClasses = {"test-glyph", "test-glyph-pond"};
    content.Rows.back().GlyphTooltip = "Pond kind";
    content.Rows.push_back(MakeCandidate(First, "Shed"));
    EntityMemberList list(content, Record(calls));

    const std::vector<UIElement*> rows = Rows(list);
    ASSERT_EQ(rows.size(), 2u);
    UIElement* memberGlyph = Part<UIElement>(*rows[0], "entity-member-glyph");
    UIElement* candidateGlyph = Part<UIElement>(*rows[1], "entity-member-glyph");
    ASSERT_NE(memberGlyph, nullptr);
    ASSERT_NE(candidateGlyph, nullptr);
    EXPECT_TRUE(memberGlyph->HasClass("test-glyph") && memberGlyph->HasClass("test-glyph-pond"));
    EXPECT_EQ(memberGlyph->GetTooltip(), "Pond kind");
    EXPECT_TRUE(candidateGlyph->HasClass("hidden")) << "a row without glyph classes shows an empty glyph";
    Label* memberTitle = Part<Label>(*rows[0], "entity-member-title");
    Label* candidateTitle = Part<Label>(*rows[1], "entity-member-title");
    ASSERT_NE(memberTitle, nullptr);
    ASSERT_NE(candidateTitle, nullptr);
    EXPECT_EQ(memberTitle->GetText(), "Pond");
    EXPECT_TRUE(rows[0]->HasClass("entity-member-in"));
    EXPECT_FALSE(memberTitle->HasClass("entity-member-title-out"));
    EXPECT_EQ(candidateTitle->GetText(), "Shed");
    EXPECT_TRUE(rows[1]->HasClass("entity-member-out"));
    EXPECT_TRUE(candidateTitle->HasClass("entity-member-title-out")) << "a candidate's title is not dimmed";

    Toggle* member = Part<Toggle>(*rows[0], "entity-member-toggle");
    Toggle* candidate = Part<Toggle>(*rows[1], "entity-member-toggle");
    ASSERT_NE(member, nullptr);
    ASSERT_NE(candidate, nullptr);
    EXPECT_TRUE(member->IsChecked());
    EXPECT_FALSE(candidate->IsChecked());
    EXPECT_EQ(member->GetTooltip(), "A member");
    EXPECT_EQ(candidate->GetTooltip(), "Not a member");

    candidate->SetValue(true);
    member->SetValue(false);
    EXPECT_EQ(calls.SwitchedOn, std::vector<EntityHandle>{First});
    EXPECT_EQ(calls.SwitchedOff, std::vector<EntityHandle>{Second});
    EXPECT_TRUE(calls.Switched.empty());

    MouseUp(*candidateTitle);
    EXPECT_EQ(calls.Selected, std::vector<EntityHandle>{First}) << "a click on the title did not select the entity";
}

// A member's mode is the filled segment, and a click on another segment calls SetMode with that
// mode; the filled segment calls nothing. A candidate shows no mode filled and cannot set one.
TEST_F(EntityMemberListTest, TheFilledSegmentIsTheModeAndAnotherSegmentSwitchesIt)
{
    Calls calls;
    EntityMemberList::Content content = MakeContent();
    content.Rows.push_back(MakeRow(First, "Pond", kCut));
    content.Rows.push_back(MakeCandidate(Second, "Shed"));
    EntityMemberList list(content, Record(calls));

    const std::vector<UIElement*> rows = Rows(list);
    ASSERT_EQ(rows.size(), 2u);
    Button* keep = Segment(*rows[0], kKeep);
    Button* cut = Segment(*rows[0], kCut);
    ASSERT_NE(keep, nullptr);
    ASSERT_NE(cut, nullptr);
    EXPECT_EQ(keep->GetText(), "Keep");
    EXPECT_EQ(keep->GetTooltip(), "Keeps it");
    EXPECT_TRUE(cut->HasClass("segmented-button-selected"));
    EXPECT_FALSE(keep->HasClass("segmented-button-selected"));
    EXPECT_TRUE(cut->HasClass("entity-member-segment-warning")) << "a filled warning mode is not in the warning color";
    // The control's sheet comes after Button.css on a segment, so its rules win at equal specificity.
    const std::vector<UIElement::SubtreeStyleAssetRequest> cutSheets = cut->ConsumeRequestedSubtreeStyleAssetPaths();
    ASSERT_FALSE(cutSheets.empty());
    EXPECT_EQ(cutSheets.back().Path, "UI/controls/EntityMemberList/EntityMemberList.css")
        << "a segment does not take the control's sheet after Button.css";
    cut->TriggerClick();
    EXPECT_TRUE(calls.Switched.empty()) << "the filled segment switched";
    keep->TriggerClick();
    ASSERT_EQ(calls.Switched.size(), 1u);
    EXPECT_EQ(calls.Switched[0].first, First);
    EXPECT_EQ(calls.Switched[0].second, kKeep);

    Button* candidateKeep = Segment(*rows[1], kKeep);
    Button* candidateCut = Segment(*rows[1], kCut);
    ASSERT_NE(candidateKeep, nullptr);
    ASSERT_NE(candidateCut, nullptr);
    EXPECT_FALSE(candidateKeep->HasClass("segmented-button-selected") ||
                 candidateCut->HasClass("segmented-button-selected"))
        << "a candidate shows a mode";
    EXPECT_FALSE(candidateCut->HasClass("entity-member-segment-warning")) << "an unfilled segment is in the warning color";
    EXPECT_FALSE(candidateKeep->IsEnabled() || candidateCut->IsEnabled()) << "a candidate's mode can be set";
}

// A row that offers one mode shows it as a label, not as a segment.
TEST_F(EntityMemberListTest, ARowWithOneModeShowsItAsALabel)
{
    Calls calls;
    EntityMemberList::Content content = MakeContent();
    EntityMemberList::Row row = MakeRow(First, "Bed", kCut);
    row.Modes = {{kCut, "Exclude", "Cuts it", true}};
    content.Rows.push_back(std::move(row));
    EntityMemberList list(content, Record(calls));

    const std::vector<UIElement*> rows = Rows(list);
    ASSERT_EQ(rows.size(), 1u);
    Button* first = Segment(*rows[0], 0);
    Button* second = Segment(*rows[0], 1);
    ASSERT_TRUE(first && second);
    EXPECT_TRUE(first->HasClass("hidden") && second->HasClass("hidden")) << "one mode shows as a segment";
    Label* mode = Part<Label>(*rows[0], "entity-member-mode-label");
    UIElement* modes = Part<UIElement>(*rows[0], "entity-member-mode");
    ASSERT_NE(mode, nullptr);
    ASSERT_NE(modes, nullptr);
    EXPECT_EQ(mode->GetText(), "Exclude");
    EXPECT_TRUE(modes->HasClass("entity-member-mode-single"));
}

// The toggle reads the row's On whatever Member says: a member switched off (a spline left out) reads
// off with its modes live, and a row the owner does not hold reads on. A row with no modes shows none.
TEST_F(EntityMemberListTest, TheToggleReadsTheRowsOnAndAMemberSwitchedOffKeepsItsModes)
{
    Calls calls;
    EntityMemberList::Content content = MakeContent();
    content.Rows.push_back(MakeCandidate(First, "Lane"));
    content.Rows.back().On = true;
    content.Rows.back().Modes.clear();
    content.Rows.push_back(MakeRow(Second, "Ring road", kCut));
    content.Rows.back().On = false;
    EntityMemberList list(content, Record(calls));

    const std::vector<UIElement*> rows = Rows(list);
    ASSERT_EQ(rows.size(), 2u);
    Toggle* lane = Part<Toggle>(*rows[0], "entity-member-toggle");
    Toggle* ringRoad = Part<Toggle>(*rows[1], "entity-member-toggle");
    ASSERT_TRUE(lane && ringRoad);
    EXPECT_TRUE(lane->IsChecked());
    EXPECT_FALSE(ringRoad->IsChecked());
    EXPECT_EQ(Part<Label>(*rows[0], "entity-member-title")->GetText(), "Lane") << "the list reordered the rows";
    EXPECT_TRUE(Part<Label>(*rows[1], "entity-member-title")->HasClass("entity-member-title-out"));
    EXPECT_TRUE(Part<UIElement>(*rows[0], "entity-member-mode")->HasClass("hidden")) << "a row with no modes shows one";
    Button* keep = Segment(*rows[1], kKeep);
    ASSERT_NE(keep, nullptr);
    EXPECT_TRUE(keep->IsEnabled()) << "a member switched off cannot switch its mode";
    EXPECT_TRUE(Segment(*rows[1], kCut)->HasClass("segmented-button-selected"));

    lane->SetValue(false);
    ringRoad->SetValue(true);
    EXPECT_EQ(calls.SwitchedOff, std::vector<EntityHandle>{First});
    EXPECT_EQ(calls.SwitchedOn, std::vector<EntityHandle>{Second});
}

// A member whose entity is deleted reads the caller's deleted title and says that undo restores it,
// in place of the caller's note; a click on it selects nothing, and it can still be removed.
TEST_F(EntityMemberListTest, ADeletedMemberSaysUndoRestoresItAndCannotBeSelected)
{
    Calls calls;
    EntityMemberList::Content content = MakeContent();
    EntityMemberList::Row row = MakeRow(First, "Pond");
    row.Live = false;
    row.Note = "outside";
    content.Rows.push_back(std::move(row));
    EntityMemberList list(content, Record(calls));

    const std::vector<UIElement*> rows = Rows(list);
    ASSERT_EQ(rows.size(), 1u);
    Label* title = Part<Label>(*rows[0], "entity-member-title");
    Label* note = Part<Label>(*rows[0], "entity-member-note");
    Toggle* member = Part<Toggle>(*rows[0], "entity-member-toggle");
    ASSERT_NE(title, nullptr);
    ASSERT_NE(note, nullptr);
    ASSERT_NE(member, nullptr);
    EXPECT_EQ(title->GetText(), "Deleted thing");
    EXPECT_TRUE(title->HasClass("entity-member-title-deleted"));
    EXPECT_EQ(note->GetText(), "undo restores it");
    MouseUp(*title);
    EXPECT_TRUE(calls.Selected.empty()) << "a deleted member was selected";
    member->SetValue(false);
    EXPECT_EQ(calls.SwitchedOff, std::vector<EntityHandle>{First});
}

// A refusal turns off every candidate's checkbox and is its tooltip; a member's checkbox stays on.
// A list that is not editable turns off every checkbox and every segment. With no rows the list says
// the caller's empty text.
TEST_F(EntityMemberListTest, ARefusalStopsAddsAndAReadOnlyListStopsEveryEdit)
{
    Calls calls;
    {
        EntityMemberList::Content content = MakeContent();
        content.Rows.push_back(MakeRow(First, "Pond"));
        content.Rows.push_back(MakeCandidate(Second, "Shed"));
        content.AddRefusal = "Full; take one out first";
        EntityMemberList list(content, Record(calls));
        const std::vector<UIElement*> rows = Rows(list);
        ASSERT_EQ(rows.size(), 2u);
        auto* empty = dynamic_cast<Label*>(FindByClass(&list, "entity-member-empty"));
        ASSERT_NE(empty, nullptr);
        EXPECT_TRUE(empty->HasClass("hidden")) << "a list with rows says it is empty";
        Toggle* member = Part<Toggle>(*rows[0], "entity-member-toggle");
        Toggle* candidate = Part<Toggle>(*rows[1], "entity-member-toggle");
        ASSERT_NE(member, nullptr);
        ASSERT_NE(candidate, nullptr);
        EXPECT_TRUE(member->IsEnabled());
        EXPECT_FALSE(candidate->IsEnabled());
        EXPECT_EQ(candidate->GetTooltip(), "Full; take one out first");
    }
    {
        EntityMemberList::Content content = MakeContent();
        content.Rows.push_back(MakeRow(First, "Pond"));
        content.Rows.push_back(MakeCandidate(Second, "Shed"));
        content.Editable = false;
        EntityMemberList list(content, Record(calls));
        const std::vector<UIElement*> rows = Rows(list);
        ASSERT_EQ(rows.size(), 2u);
        for (UIElement* row : rows)
        {
            Toggle* member = Part<Toggle>(*row, "entity-member-toggle");
            Button* keep = Segment(*row, kKeep);
            Button* cut = Segment(*row, kCut);
            ASSERT_TRUE(member && keep && cut);
            EXPECT_FALSE(member->IsEnabled());
            EXPECT_FALSE(keep->IsEnabled() || cut->IsEnabled());
        }
    }
    {
        EntityMemberList list(MakeContent(), Record(calls));
        EXPECT_TRUE(Rows(list).empty());
        auto* empty = dynamic_cast<Label*>(FindByClass(&list, "entity-member-empty"));
        ASSERT_NE(empty, nullptr);
        EXPECT_EQ(empty->GetText(), "Nothing overlaps");
        EXPECT_FALSE(empty->HasClass("hidden")) << "the empty text is hidden on a list with no rows";
    }
}

// Over the editor's sheets in a narrow inspector section: each title sits in the label column, and the
// value columns line up. A short note sits beside the checkbox; a long note wraps inside the value
// column and is not cut, and the mode keeps its place at the row's end.
TEST_F(EntityMemberListTest, TheTitleSitsInTheLabelColumnAndALongNoteWrapsInTheValueColumn)
{
    Calls calls;
    EntityMemberList::Content content = MakeContent();
    EntityMemberList::Row shortRow = MakeRow(First, "Pond");
    shortRow.Note = "outside";
    EntityMemberList::Row longRow = MakeRow(Second, "Market Square");
    longRow.Note = "base outline only: its own 12 members do not apply here, and its height is its own";
    content.Rows.push_back(std::move(shortRow));
    content.Rows.push_back(std::move(longRow));
    auto list = std::make_unique<EntityMemberList>(content, Record(calls));
    EntityMemberList* raw = list.get();
    auto section = std::make_unique<UIElement>();
    section->AddClass("inspector-section");
    auto body = std::make_unique<UIElement>();
    body->AddClass("inspector-section-body");
    body->AddChild(std::move(list));
    section->AddChild(std::move(body));

    InspectorLayoutFixture fixture;
    fixture.ViewportW = 320;
    fixture.ExtraSheets = kListSheets;
    if (!fixture.Build(std::move(section)))
        GTEST_SKIP() << fixture.Diagnostic;

    const std::vector<UIElement*> rows = Rows(*raw);
    ASSERT_EQ(rows.size(), 2u);
    Label* shortTitle = Part<Label>(*rows[0], "entity-member-title");
    Toggle* shortToggle = Part<Toggle>(*rows[0], "entity-member-toggle");
    Label* shortNote = Part<Label>(*rows[0], "entity-member-note");
    UIElement* shortValue = Part<UIElement>(*rows[0], "entity-member-value");
    Label* longTitle = Part<Label>(*rows[1], "entity-member-title");
    Label* longNote = Part<Label>(*rows[1], "entity-member-note");
    UIElement* longValue = Part<UIElement>(*rows[1], "entity-member-value");
    UIElement* longModes = Part<UIElement>(*rows[1], "entity-member-mode");
    ASSERT_TRUE(shortTitle && shortToggle && shortNote && shortValue && longTitle && longNote && longValue && longModes);
    const float lineHeight = shortNote->GetLayoutHeight();
    ASSERT_GT(lineHeight, 0.0f) << "no text measured, so a wrap cannot be told from a fixture without a font";

    EXPECT_NEAR(shortValue->GetLayoutX(), longValue->GetLayoutX(), kLayoutTolerance) << "the value columns differ";
    EXPECT_LE(Right(*shortTitle), shortValue->GetLayoutX() + kLayoutTolerance) << "a title runs into the value column";
    EXPECT_LE(Right(*longTitle), longValue->GetLayoutX() + kLayoutTolerance) << "a title runs into the value column";
    EXPECT_GE(shortNote->GetLayoutX(), Right(*shortToggle)) << "a short note left its checkbox's line";
    EXPECT_NEAR(shortNote->GetLayoutY() + 0.5f * shortNote->GetLayoutHeight(),
                shortToggle->GetLayoutY() + 0.5f * shortToggle->GetLayoutHeight(), kLayoutTolerance);

    EXPECT_GE(longNote->GetLayoutX(), longValue->GetLayoutX() - kLayoutTolerance) << "the long note left its column";
    EXPECT_GE(longNote->GetLayoutHeight(), 2.0f * lineHeight - kLayoutTolerance) << "the long note did not wrap";
    EXPECT_LE(Right(*longNote), Right(*longValue) + kLayoutTolerance) << "the long note runs past its row";
    EXPECT_NEAR(Right(*rows[1]) - Right(*longModes), kRowPadding, kLayoutTolerance)
        << "the mode left the row's end";
    for (std::size_t index = 0; index < 2; ++index)
    {
        Button* segment = Segment(*rows[1], index);
        UIElement* words = segment ? FindByClass(segment, "button-text") : nullptr;
        ASSERT_TRUE(segment && words);
        EXPECT_GT(words->GetLayoutWidth(), kSegmentPadding) << "segment " << index << " shows no words";
        EXPECT_GE(segment->GetLayoutWidth(), words->GetLayoutWidth() + kSegmentPadding - kLayoutTolerance)
            << "segment " << index << " is narrower than its words";
    }
}

TEST_F(EntityMemberListTest, AListWithNoHeadingPutsItsRowsAndItsEmptyTextOnTheSectionsEdgeAndATitleNamesItsEntity)
{
    Calls calls;
    EntityMemberList::Content withRow = MakeContent();
    withRow.Text.Heading.clear();
    withRow.Rows.push_back(MakeRow(First, "Village Square"));
    EntityMemberList::Content empty = MakeContent();
    empty.Text.Heading.clear();
    auto rowList = std::make_unique<EntityMemberList>(withRow, Record(calls));
    auto emptyList = std::make_unique<EntityMemberList>(empty, Record(calls));
    EntityMemberList* rowRaw = rowList.get();
    EntityMemberList* emptyRaw = emptyList.get();
    auto section = std::make_unique<UIElement>();
    section->AddClass("inspector-section");
    auto body = std::make_unique<UIElement>();
    body->AddClass("inspector-section-body");
    body->AddChild(std::move(rowList));
    body->AddChild(std::move(emptyList));
    section->AddChild(std::move(body));

    InspectorLayoutFixture fixture;
    fixture.ViewportW = 320;
    fixture.ExtraSheets = kListSheets;
    if (!fixture.Build(std::move(section)))
        GTEST_SKIP() << fixture.Diagnostic;

    const std::vector<UIElement*> rows = Rows(*rowRaw);
    ASSERT_EQ(rows.size(), 1u);
    Label* title = Part<Label>(*rows[0], "entity-member-title");
    Label* emptyText = Part<Label>(*emptyRaw, "entity-member-empty");
    ASSERT_TRUE(title && emptyText);
    EXPECT_NEAR(title->GetLayoutX(), rowRaw->GetLayoutX(), kLayoutTolerance) << "the row keeps the plate's padding";
    EXPECT_NEAR(emptyText->GetLayoutX(), emptyRaw->GetLayoutX(), kLayoutTolerance)
        << "the empty text keeps the plate's padding";
    EXPECT_EQ(title->GetTooltip(), "Village Square. Click to select it.");
}
