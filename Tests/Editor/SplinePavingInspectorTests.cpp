// The paving inspector: Splines It Touches on the shared member list with the modes each spline takes,
// the junction list with each junction's state, the header badge, Join at Grade, and Ground
// Authority reading its resolved value.

#include "Components/Name.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Spline/SplinePaving.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "EditorContext.h"
#include "InspectorRegistry.h"
#include "Inspectors/SplineRecipe/Paving/SplinePavingInspector.h"
#include "PlayMode/PlayModeManager.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineNetwork.h"
#include "SplineECS/SplineService.h"
#include "SplineRecipes/Paving/PavingStatus.h"
#include "Input/KeyCodes.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/PictureChoiceField.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/InspectorSection.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

using Mathematics::Vector3;
using SplineRecipes::PavingStatus;
using SplineRecipes::PavingJunctionState;
using SplineRecipes::PavingJunctionStatus;

void CollectTexts(UIElement& element, std::vector<std::string>& texts)
{
    if (const auto* label = dynamic_cast<const Label*>(&element))
        texts.push_back(label->GetText());
    for (const auto& child : element.GetChildren())
        CollectTexts(*child, texts);
}

// The first number box under `element`, or null.
FloatField* FirstNumberBox(UIElement& element)
{
    if (auto* field = dynamic_cast<FloatField*>(&element))
        return field;
    for (const auto& child : element.GetChildren())
    {
        if (FloatField* field = FirstNumberBox(*child))
            return field;
    }
    return nullptr;
}

// The text the first number box under `element` shows.
std::string NumberBoxText(UIElement& element)
{
    if (auto* field = dynamic_cast<FloatField*>(&element))
    {
        for (const auto& child : field->GetChildren())
        {
            if (const auto* editor = dynamic_cast<const TextInput*>(child.get()))
                return editor->GetValue();
        }
        return {};
    }
    for (const auto& child : element.GetChildren())
    {
        const std::string text = NumberBoxText(*child);
        if (!text.empty())
            return text;
    }
    return {};
}

bool Shows(UIElement& element, const std::string& text)
{
    std::vector<std::string> texts;
    CollectTexts(element, texts);
    return std::find(texts.begin(), texts.end(), text) != texts.end();
}

void CollectWithClass(UIElement& element, const char* className, std::vector<UIElement*>& found)
{
    if (element.HasClass(className))
        found.push_back(&element);
    for (const auto& child : element.GetChildren())
        CollectWithClass(*child, className, found);
}

UIElement* FirstWithClass(UIElement& element, const char* className)
{
    std::vector<UIElement*> found;
    CollectWithClass(element, className, found);
    return found.empty() ? nullptr : found.front();
}

// The inspector row whose label reads `label`, or null.
UIElement* RowLabeled(UIElement& element, const std::string& label)
{
    std::vector<UIElement*> rows;
    CollectWithClass(element, "inspector-row", rows);
    for (UIElement* row : rows)
    {
        if (const auto* text = dynamic_cast<const Label*>(FirstWithClass(*row, "inspector-label"));
            text && text->GetText() == label)
            return row;
    }
    return nullptr;
}

// The position of `text` among the labels under `element` in tree order, or the label count.
std::size_t TextPosition(UIElement& element, const std::string& text)
{
    std::vector<std::string> texts;
    CollectTexts(element, texts);
    return static_cast<std::size_t>(std::find(texts.begin(), texts.end(), text) - texts.begin());
}

// A left press on the tile `index` of the picture row labeled `label`.
void PickTile(UIElement& section, const std::string& label, std::size_t index)
{
    UIElement* row = RowLabeled(section, label);
    ASSERT_NE(row, nullptr) << label;
    std::vector<UIElement*> tiles;
    CollectWithClass(*row, "picture-choice-tile", tiles);
    ASSERT_LT(index, tiles.size()) << label;
    UIEvent press{};
    press.Id = kEventMouseDown;
    press.Button = Input::kMouseButton_Left;
    press.Target = tiles[index];
    press.CurrentTarget = tiles[index];
    tiles[index]->DispatchEvent(press);
}

// The setting rows of the Road Mouths section under `first`, in order: `first` and the rows after it,
// without the caption rows.
std::vector<UIElement*> RowsFrom(UIElement& first)
{
    std::vector<UIElement*> rows;
    bool reached = false;
    for (const auto& row : first.GetParent()->GetChildren())
    {
        reached |= row.get() == &first;
        if (reached && row->HasClass("inspector-row") && !row->HasClass("inspector-caption-row"))
            rows.push_back(row.get());
    }
    return rows;
}

// The member list's row whose title reads `title`.
UIElement* MemberRow(UIElement& root, const std::string& title)
{
    std::vector<UIElement*> rows;
    CollectWithClass(root, "entity-member-row", rows);
    for (UIElement* row : rows)
        if (const auto* label = dynamic_cast<Label*>(FirstWithClass(*row, "entity-member-title"));
            label && label->GetText() == title)
            return row;
    return nullptr;
}

// The titles of the member rows, top to bottom.
std::vector<std::string> RowTitles(UIElement& root)
{
    std::vector<UIElement*> rows;
    CollectWithClass(root, "entity-member-row", rows);
    std::vector<std::string> titles;
    for (UIElement* row : rows)
        if (const auto* label = dynamic_cast<Label*>(FirstWithClass(*row, "entity-member-title")))
            titles.push_back(label->GetText());
    return titles;
}

// The titles of the junction rows, top to bottom.
std::vector<std::string> JunctionTitles(UIElement& root)
{
    std::vector<UIElement*> rows;
    CollectWithClass(root, "spline-junction-row", rows);
    std::vector<std::string> titles;
    for (UIElement* row : rows)
        if (const auto* label = dynamic_cast<Label*>(FirstWithClass(*row, "spline-junction-title")))
            titles.push_back(label->GetText());
    return titles;
}

// The title of the nearest section that holds `element`, or empty.
std::string SectionTitleOf(const UIElement& element)
{
    for (const UIElement* parent = element.GetParent(); parent; parent = parent->GetParent())
        if (const auto* section = dynamic_cast<const Foldout*>(parent))
            return section->GetTitle();
    return std::string();
}

// Whether `part` sits in the label column (`inspector-label-cell`) or the value column (`inspector-field`)
// of an inspector row.
bool InColumn(const UIElement& part, const char* columnClass)
{
    const UIElement* column = part.GetParent();
    return column && column->HasClass(columnClass) && column->GetParent() &&
           column->GetParent()->HasClass("inspector-row");
}

std::string TextOf(UIElement& row, const char* className)
{
    const auto* label = dynamic_cast<Label*>(FirstWithClass(row, className));
    return label ? label->GetText() : std::string();
}

// Whether a notice under `element` reads `text`.
bool ShowsNotice(UIElement& element, const std::string& text)
{
    std::vector<UIElement*> labels;
    CollectWithClass(element, "inspector-notice-text", labels);
    for (UIElement* found : labels)
    {
        const auto* label = dynamic_cast<Label*>(found);
        if (label && label->GetText() == text)
            return true;
    }
    return false;
}

class SplinePavingInspectorTest : public ::testing::Test
{
  protected:
    // The look rows include asset pickers, which read the engine's asset registry.
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (engine.IsInitialized())
            return;
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        ASSERT_TRUE(engine.Initialize(config));
    }

    void SetUp() override
    {
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
        m_OwnsNetwork = !SplineECS::SplineNetwork::IsInitialized();
        if (m_OwnsNetwork)
            SplineECS::SplineNetwork::Initialize();
        RegisterSplinePavingInspector();
    }

    void TearDown() override
    {
        if (m_OwnsNetwork)
            SplineECS::SplineNetwork::Shutdown();
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
    }

    // A linear spline named `name` through `points`; a network member of `kind` unless `kind` is null.
    ECS::EntityHandle CreateSpline(const char* name, std::initializer_list<Vector3> points, bool closed,
                                   const Components::SplineNetworkMemberKind* kind)
    {
        auto& splines = SplineECS::SplineService::Get();
        const auto handle = splines.CreateSpline(Spline::SplineType::Linear, closed);
        auto* data = splines.GetSplineData(handle);
        for (const Vector3& point : points)
            data->AddPoint(point, 2.0f);
        splines.RebuildCache(handle);
        const ECS::EntityHandle entity = World.CreateEntity();
        Components::SplineComponent spline{};
        spline.SplineDataIndex = handle.Index();
        spline.SplineDataGeneration = handle.Generation();
        World.AddComponentImmediate(entity, spline);
        World.AddComponentImmediate(entity, Components::WorldTransform{});
        Components::Name label{};
        std::strncpy(label.value, name, sizeof(label.value) - 1);
        World.AddComponentImmediate(entity, label);
        if (kind)
            World.AddComponentImmediate(entity, Components::SplineNetworkMember{*kind});
        return entity;
    }

    ECS::EntityHandle CreateSquare()
    {
        const ECS::EntityHandle square = CreateSpline(
            "Market Square",
            {Vector3(-10.0f, 0.0f, -10.0f), Vector3(10.0f, 0.0f, -10.0f), Vector3(10.0f, 0.0f, 10.0f),
             Vector3(-10.0f, 0.0f, 10.0f)},
            true, &kArea);
        World.AddComponentImmediate(square, Components::SplinePaving{});
        return square;
    }

    // The inspector of `entity`'s paving under a section, as the Inspector builds it.
    std::unique_ptr<InspectorSection> Build(ECS::EntityHandle entity, const PavingStatus& status,
                                            const EditorContext* editor = nullptr)
    {
        auto section = std::make_unique<InspectorSection>("Spline Paving");
        InspectorContext context;
        context.EditorCtx = editor;
        context.Parent = section.get();
        context.Section = section.get();
        context.World = &World;
        context.Entity = entity;
        context.Undo = &Undo;
        context.ChangeNotifications = &Notifications;
        AddSplinePavingRows(context, &status);
        return section;
    }

    static constexpr Components::SplineNetworkMemberKind kArea = Components::SplineNetworkMemberKind::Area;
    static constexpr Components::SplineNetworkMemberKind kRoad = Components::SplineNetworkMemberKind::Road;

    Editor::EditorChangeNotifications Notifications;
    Editor::UndoRedoService Undo;
    ECS::World World;
    bool m_OwnsSplines = false;
    bool m_OwnsNetwork = false;
};

TEST_F(SplinePavingInspectorTest, TheBedAndTheRoadReadPavedAndLeavingTheRoadOutIsOneNamedStep)
{
    const ECS::EntityHandle square = CreateSquare();
    const ECS::EntityHandle road =
        CreateSpline("Mill Lane", {Vector3(-40.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 0.0f)}, false, &kRoad);
    CreateSpline("Bed 1", {Vector3(-2.0f, 0.0f, -2.0f), Vector3(2.0f, 0.0f, -2.0f), Vector3(0.0f, 0.0f, 2.0f)}, true,
                 nullptr);
    PavingStatus status;
    status.Entity = square;
    status.Role = SplineRecipes::PavingRole::Owns;
    status.Junctions.push_back({1u, SplineECS::SplineNodeKind::Entry, road, 0.0f, PavingJunctionState::Paved, {}, 0.0f});

    std::unique_ptr<InspectorSection> section = Build(square, status);

    EXPECT_TRUE(Shows(*section, "Splines It Touches"));
    EXPECT_TRUE(Shows(*section, "Junctions"));
    UIElement* bed = MemberRow(*section, "Bed 1");
    UIElement* lane = MemberRow(*section, "Mill Lane");
    ASSERT_NE(bed, nullptr);
    ASSERT_NE(lane, nullptr);
    EXPECT_EQ(TextOf(*bed, "entity-member-note"), "paved");
    EXPECT_EQ(TextOf(*lane, "entity-member-note"), "paved where it meets");
    EXPECT_TRUE(Shows(*section, "Mill Lane \xC2\xB7 entry"));
    EXPECT_EQ(TextOf(*section, "spline-junction-state"), "paved") << "the two lists write their states in one case";

    auto* toggle = dynamic_cast<Toggle*>(FirstWithClass(*lane, "entity-member-toggle"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_TRUE(toggle->IsChecked()) << "a paved road reads off";
    EXPECT_EQ(toggle->GetTooltip(), "Switch off to stop paving it.");
    toggle->SetValue(false);

    const auto* recipe = World.GetComponent<Components::SplinePaving>(square);
    ASSERT_EQ(recipe->MemberCount, 1u);
    EXPECT_EQ(recipe->Members[0].Entity, road);
    EXPECT_EQ(recipe->Members[0].Mode, Components::SplinePavingMemberMode::Ignore);
    EXPECT_STREQ(Undo.PeekUndoName(), "Leave Out of Paving");
}

TEST_F(SplinePavingInspectorTest, ARowKeepsItsPlaceWhenItIsSwitchedOff)
{
    const ECS::EntityHandle square = CreateSquare();
    const ECS::EntityHandle road =
        CreateSpline("Mill Lane", {Vector3(-40.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 0.0f)}, false, &kRoad);
    CreateSpline("Bed 1", {Vector3(-2.0f, 0.0f, -2.0f), Vector3(2.0f, 0.0f, -2.0f), Vector3(0.0f, 0.0f, 2.0f)}, true,
                 nullptr);
    PavingStatus status;
    status.Entity = square;
    status.Junctions.push_back({1u, SplineECS::SplineNodeKind::Entry, road, 0.0f, PavingJunctionState::Paved, {}, 0.0f});
    const std::vector<std::string> byName{"Bed 1", "Mill Lane"};

    std::unique_ptr<InspectorSection> before = Build(square, status);
    ASSERT_EQ(RowTitles(*before), byName);
    auto* toggle = dynamic_cast<Toggle*>(FirstWithClass(*MemberRow(*before, "Mill Lane"), "entity-member-toggle"));
    ASSERT_NE(toggle, nullptr);
    toggle->SetValue(false);
    ASSERT_EQ(World.GetComponent<Components::SplinePaving>(square)->MemberCount, 1u);

    std::unique_ptr<InspectorSection> after = Build(square, status);
    EXPECT_EQ(RowTitles(*after), byName) << "the switched row moved";
}

TEST_F(SplinePavingInspectorTest, TheJunctionsListByName)
{
    const ECS::EntityHandle square = CreateSquare();
    const ECS::EntityHandle tower =
        CreateSpline("Tower Street", {Vector3(0.0f, 0.0f, 40.0f), Vector3(0.0f, 0.0f, 4.0f)}, false, &kRoad);
    const ECS::EntityHandle mill =
        CreateSpline("Mill Lane", {Vector3(-40.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 0.0f)}, false, &kRoad);
    PavingStatus status;
    status.Entity = square;
    status.Junctions.push_back({1u, SplineECS::SplineNodeKind::Entry, tower, 0.0f, PavingJunctionState::Paved, {}, 0.0f});
    status.Junctions.push_back({2u, SplineECS::SplineNodeKind::Entry, mill, 0.0f, PavingJunctionState::Ignored, {}, 0.0f});

    std::unique_ptr<InspectorSection> section = Build(square, status);

    EXPECT_EQ(JunctionTitles(*section),
              (std::vector<std::string>{"Mill Lane \xC2\xB7 entry", "Tower Street \xC2\xB7 entry"}));
}

TEST_F(SplinePavingInspectorTest, TwoJunctionsWithOneRoadNameTheirDistanceAlongIt)
{
    const ECS::EntityHandle square = CreateSquare();
    const ECS::EntityHandle high =
        CreateSpline("High Street", {Vector3(0.0f, 0.0f, -40.0f), Vector3(0.0f, 0.0f, 40.0f)}, false, &kRoad);
    PavingStatus status;
    status.Entity = square;
    status.Junctions.push_back({2u, SplineECS::SplineNodeKind::Entry, high, 50.0f, PavingJunctionState::Paved, {}, 0.0f});
    status.Junctions.push_back({1u, SplineECS::SplineNodeKind::Entry, high, 30.0f, PavingJunctionState::Paved, {}, 0.0f});

    std::unique_ptr<InspectorSection> section = Build(square, status);

    EXPECT_EQ(JunctionTitles(*section), (std::vector<std::string>{"High Street \xC2\xB7 entry at 30 m",
                                                                  "High Street \xC2\xB7 entry at 50 m"}));
}

TEST_F(SplinePavingInspectorTest, AStatusThatDiffersFromTheShownOneRebuildsTheInspectorOnce)
{
    const ECS::EntityHandle square = CreateSquare();
    PavingStatus status;
    status.Entity = square;
    std::vector<std::function<void()>> refreshCallbacks;
    int refreshes = 0;
    auto section = std::make_unique<InspectorSection>("Spline Paving");
    InspectorContext context;
    context.Parent = section.get();
    context.Section = section.get();
    context.World = &World;
    context.Entity = square;
    context.Undo = &Undo;
    context.ChangeNotifications = &Notifications;
    context.SimulationRefreshCallbacks = &refreshCallbacks;
    context.RequestInspectorRefresh = [&refreshes]() { ++refreshes; };

    // No running recipe system has built this paving, so its status is none: it differs from the shown one.
    AddSplinePavingRows(context, &status);
    for (int tick = 0; tick < 2; ++tick)
        for (const std::function<void()>& callback : refreshCallbacks)
            callback();

    EXPECT_EQ(refreshes, 1);
}

TEST_F(SplinePavingInspectorTest, NeitherListChangesWhilePlaying)
{
    const ECS::EntityHandle square = CreateSquare();
    const ECS::EntityHandle road =
        CreateSpline("Mill Lane", {Vector3(-40.0f, 0.5f, 0.0f), Vector3(-4.0f, 0.5f, 0.0f)}, false, &kRoad);
    PavingStatus status;
    status.Entity = square;
    status.Junctions.push_back({1u, SplineECS::SplineNodeKind::Entry, road, 0.0f, PavingJunctionState::HeightsDiffer, {}, 0.5f});
    Editor::PlayModeManager playMode;
    playMode.SetWorld(&World);
    playMode.EnterPlayMode();
    ASSERT_TRUE(playMode.IsPlayingOrPaused());
    EditorContext editor;
    editor.PlayMode = &playMode;

    std::unique_ptr<InspectorSection> section = Build(square, status, &editor);

    auto* toggle = dynamic_cast<Toggle*>(FirstWithClass(*MemberRow(*section, "Mill Lane"), "entity-member-toggle"));
    auto* join = dynamic_cast<Button*>(FirstWithClass(*section, "spline-junction-action"));
    ASSERT_TRUE(toggle && join);
    EXPECT_FALSE(toggle->IsEnabled()) << "a row switches while playing";
    EXPECT_FALSE(join->IsEnabled()) << "Join at Grade runs while playing";
    playMode.RequestExitPlayMode();
}

TEST_F(SplinePavingInspectorTest, AClosedRoadCutOutReadsCutOutOnItsRowAndAtItsJunctions)
{
    const ECS::EntityHandle square = CreateSquare();
    const ECS::EntityHandle ring =
        CreateSpline("Ring Road",
                     {Vector3(-15.0f, 0.0f, 5.0f), Vector3(-5.0f, 0.0f, 5.0f), Vector3(-5.0f, 0.0f, 15.0f),
                      Vector3(-15.0f, 0.0f, 15.0f)},
                     true, &kRoad);
    auto* recipe = World.GetComponentForWrite<Components::SplinePaving>(square);
    recipe->Members[0] = {ring, Components::SplinePavingMemberMode::Exclude};
    recipe->MemberCount = 1;
    PavingStatus status;
    status.Entity = square;
    status.Role = SplineRecipes::PavingRole::Owns;
    status.Junctions.push_back({1u, SplineECS::SplineNodeKind::Entry, ring, 0.0f, PavingJunctionState::CutOut, {}, 0.0f});

    std::unique_ptr<InspectorSection> section = Build(square, status);

    UIElement* row = MemberRow(*section, "Ring Road");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(TextOf(*row, "entity-member-note"), "cut out");
    EXPECT_TRUE(Shows(*section, "not paved: cut out"));
    auto* toggle = dynamic_cast<Toggle*>(FirstWithClass(*row, "entity-member-toggle"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_EQ(toggle->GetTooltip(), "Switch on to pave it.");
    UIElement* modes = FirstWithClass(*row, "entity-member-mode");
    ASSERT_NE(modes, nullptr);
    std::vector<std::string> segments;
    for (const auto& child : modes->GetChildren())
        if (const auto* button = dynamic_cast<const Button*>(child.get()))
            segments.push_back(button->GetText());
    EXPECT_EQ(segments, (std::vector<std::string>{"Cut out", "Own surface"}));
}

TEST_F(SplinePavingInspectorTest, ARoadAboveTheSquareShowsTheBadgeItsHeightAndJoinAtGrade)
{
    const ECS::EntityHandle square = CreateSquare();
    const ECS::EntityHandle road =
        CreateSpline("Mill Lane", {Vector3(-40.0f, 0.5f, 0.0f), Vector3(-4.0f, 0.5f, 0.0f)}, false, &kRoad);
    SplineECS::SplineNetwork::Get().Update(World);
    const std::vector<const SplineECS::SplineNode*> nodes = SplineECS::SplineNetwork::Get().FindNodes(World, square);
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_TRUE(nodes[0]->IsSplitLevel());
    PavingStatus status;
    status.Entity = square;
    status.Role = SplineRecipes::PavingRole::Owns;
    status.Junctions.push_back(
        {nodes[0]->Id, SplineECS::SplineNodeKind::Entry, road, 0.0f, PavingJunctionState::HeightsDiffer, {}, 0.5f});

    std::unique_ptr<InspectorSection> section = Build(square, status);

    EXPECT_TRUE(Shows(*section, "1 road not paved"));
    EXPECT_TRUE(Shows(*section, "0.5 m above the paving"));
    EXPECT_TRUE(Shows(*section, "not paved: heights differ (see Junctions)"));
    EXPECT_NE(FirstWithClass(*section, "spline-junction-state-warning"), nullptr);
    auto* join = dynamic_cast<Button*>(FirstWithClass(*section, "spline-junction-action"));
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->GetText(), "Join at Grade");
    join->TriggerClick();

    EXPECT_STREQ(Undo.PeekUndoName(), "Join Mill Lane at Grade");
}

TEST_F(SplinePavingInspectorTest, GroundAuthorityReadsAutoOwnsWithItsNoteAndTheMemberFieldsHaveNoRows)
{
    const ECS::EntityHandle square = CreateSquare();
    PavingStatus status;
    status.Entity = square;

    std::unique_ptr<InspectorSection> section = Build(square, status);

    UIElement* row = RowLabeled(*section, "Ground Authority");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(SectionTitleOf(*row), "Placement");
    EXPECT_TRUE(Shows(*row, "Auto (Owns)"));
    const auto* label = dynamic_cast<const Label*>(FirstWithClass(*row, "inspector-label"));
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(label->GetTooltip().empty());
    const char* note = "Grades the ground to a smooth surface that follows the rim and meets each road at its height. "
                       "Clears its grass, paints it Dirt, and claims it, so other grading stops at its edge. Planting "
                       "beds cut out of the paving are graded and painted too.";
    EXPECT_TRUE(Shows(*section, note));
    EXPECT_LT(TextPosition(*section, "Ground Authority"), TextPosition(*section, note));
    EXPECT_LT(TextPosition(*section, note), TextPosition(*section, "Conform Target"));
    EXPECT_TRUE(Shows(*section, "Width (m)"));
    EXPECT_TRUE(Shows(*section, "Radius (m)"));
    EXPECT_FALSE(Shows(*section, "Member Count"));
    EXPECT_FALSE(Shows(*section, "1 road not paved"));

    World.GetComponentForWrite<Components::SplinePaving>(square)->GroundAuthority =
        Components::SplineGroundAuthority::None;
    std::unique_ptr<InspectorSection> none = Build(square, status);
    EXPECT_FALSE(Shows(*none, note)) << "None grades nothing, so the note on grading does not show";

    World.GetComponentForWrite<Components::SplinePaving>(square)->GroundAuthority =
        Components::SplineGroundAuthority::Grades;
    std::unique_ptr<InspectorSection> grades = Build(square, status);
    UIElement* gradesRow = RowLabeled(*grades, "Ground Authority");
    ASSERT_NE(gradesRow, nullptr);
    EXPECT_TRUE(Shows(*gradesRow, "Grades")) << "the row shows the stored value, not Auto (Owns)";
}

TEST_F(SplinePavingInspectorTest, APavingOnARoadHasNoGroundAuthorityAndSaysTheRoadOwnsTheGround)
{
    const ECS::EntityHandle road =
        CreateSpline("High Street", {Vector3(-20.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 0.0f)}, false, &kRoad);
    World.AddComponentImmediate(road, Components::SplinePaving{});
    PavingStatus status;
    status.Entity = road;

    std::unique_ptr<InspectorSection> section = Build(road, status);

    EXPECT_FALSE(Shows(*section, "Ground Authority"));
    EXPECT_TRUE(Shows(*section, "The paving lies on the terrain as it is and does not reshape it. Where a road's "
                                "Ground Authority flattens the terrain under the paving, the terrain can show through "
                                "the paving. A road that owns its ground clears the grass under it; under a road "
                                "that only grades the ground, the grass still grows."));
}

// An area's Seam rows serve the roads that enter it with no paving of their own, and its caption says so
// under them. A paving on a road owns only its own seams, so it has no caption.
TEST_F(SplinePavingInspectorTest, AnAreasSeamRowsSayTheyApplyToTheRoadsWithoutTheirOwnPaving)
{
    const char* caption = "These Seam settings apply to each road that enters this paving and has no Spline Paving of its own.";
    const ECS::EntityHandle square = CreateSquare();
    PavingStatus areaStatus;
    areaStatus.Entity = square;
    std::unique_ptr<InspectorSection> area = Build(square, areaStatus);
    EXPECT_TRUE(Shows(*area, caption));
    EXPECT_LT(TextPosition(*area, "Course Material"), TextPosition(*area, caption));

    const ECS::EntityHandle road =
        CreateSpline("High Street", {Vector3(-20.0f, 0.0f, 40.0f), Vector3(20.0f, 0.0f, 40.0f)}, false, &kRoad);
    World.AddComponentImmediate(road, Components::SplinePaving{});
    PavingStatus roadStatus;
    roadStatus.Entity = road;
    std::unique_ptr<InspectorSection> paved = Build(road, roadStatus);
    EXPECT_TRUE(Shows(*paved, "Seam"));
    EXPECT_FALSE(Shows(*paved, caption));
}

TEST_F(SplinePavingInspectorTest, TheSectionsReadInWorkingOrderWithShadowsLastAndTheSettingsShowTheirUnits)
{
    const ECS::EntityHandle square = CreateSquare();
    PavingStatus status;
    status.Entity = square;

    std::unique_ptr<InspectorSection> section = Build(square, status);

    const std::size_t surface = TextPosition(*section, "Surface");
    const std::size_t curb = TextPosition(*section, "Curb");
    const std::size_t mouths = TextPosition(*section, "Road Mouths");
    const std::size_t placement = TextPosition(*section, "Placement");
    const std::size_t shadows = TextPosition(*section, "Shadows");
    EXPECT_LT(surface, curb);
    EXPECT_LT(curb, mouths);
    EXPECT_LT(mouths, placement);
    const std::size_t splines = TextPosition(*section, "Splines It Touches");
    const std::size_t junctions = TextPosition(*section, "Junctions");
    EXPECT_LT(placement, splines);
    EXPECT_LT(splines, junctions);
    EXPECT_LT(junctions, shadows);
    EXPECT_LT(TextPosition(*section, "Conform Target"), shadows);
    EXPECT_LT(shadows, TextPosition(*section, "Cast Shadows"));
    EXPECT_LT(shadows, TextPosition(*section, "Receive Shadows"));
    EXPECT_LT(curb, TextPosition(*section, "Width (m)"));
    EXPECT_LT(TextPosition(*section, "Width (m)"), TextPosition(*section, "Height (m)"));
    UIElement* shape = RowLabeled(*section, "Shape");
    UIElement* radius = RowLabeled(*section, "Radius (m)");
    ASSERT_NE(shape, nullptr);
    ASSERT_NE(radius, nullptr);
    ASSERT_EQ(shape->GetParent(), radius->GetParent());
    const auto& mouthRows = shape->GetParent()->GetChildren();
    const auto shapeAt =
        std::find_if(mouthRows.begin(), mouthRows.end(), [shape](const auto& row) { return row.get() == shape; });
    ASSERT_NE(shapeAt, mouthRows.end());
    ASSERT_NE(std::next(shapeAt), mouthRows.end());
    EXPECT_EQ(std::next(shapeAt)->get(), radius) << "the radius is not the row under Shape";
    EXPECT_EQ(SectionTitleOf(*radius), "Road Mouths");
    EXPECT_LT(placement, TextPosition(*section, "Conform Target"));
    EXPECT_FALSE(Shows(*section, "Border Width"));
    EXPECT_FALSE(Shows(*section, "Mouth Radius"));
    for (const char* label : {"Material", "Seed", "Width (m)", "Height (m)", "Corner", "Shape", "Radius (m)",
                              "Conform Target", "Cast Shadows", "Receive Shadows"})
    {
        UIElement* row = RowLabeled(*section, label);
        ASSERT_NE(row, nullptr) << label;
        const auto* text = dynamic_cast<const Label*>(FirstWithClass(*row, "inspector-label"));
        EXPECT_FALSE(text->GetTooltip().empty()) << label << " has no tooltip";
    }
}

TEST_F(SplinePavingInspectorTest, SeedSaysUnderItWhatItMoves)
{
    const ECS::EntityHandle square = CreateSquare();
    PavingStatus status;
    status.Entity = square;

    std::unique_ptr<InspectorSection> section = Build(square, status);
    const std::string caption =
        "Offsets the paving pattern on the ground, so two pavings with one material do not look alike.";
    ASSERT_TRUE(Shows(*section, caption));
    EXPECT_LT(TextPosition(*section, "Seed"), TextPosition(*section, caption));
    EXPECT_LT(TextPosition(*section, caption), TextPosition(*section, "Curb"));
}

// The curb's Width and Height sliders reach 1 m; a typed value takes the field's range up to 10 m.
TEST_F(SplinePavingInspectorTest, TheCurbWidthAndHeightAreSlidersOverTheirRanges)
{
    const ECS::EntityHandle square = CreateSquare();
    PavingStatus status;
    status.Entity = square;

    std::unique_ptr<InspectorSection> section = Build(square, status);
    struct Expected
    {
        const char* Label;
        float Min;
        float Max;
    };
    for (const Expected& expected : {Expected{"Width (m)", 0.0f, 1.0f}, Expected{"Height (m)", 0.01f, 1.0f}})
    {
        UIElement* row = RowLabeled(*section, expected.Label);
        ASSERT_NE(row, nullptr) << expected.Label;
        auto* slider = dynamic_cast<Slider*>(FirstWithClass(*row, "slider"));
        ASSERT_NE(slider, nullptr) << expected.Label;
        EXPECT_FLOAT_EQ(slider->GetMin(), expected.Min) << expected.Label;
        EXPECT_FLOAT_EQ(slider->GetMax(), expected.Max) << expected.Label;
    }
}

TEST_F(SplinePavingInspectorTest, TheCornerMouthAndGroundArePictureTilesAndAPickIsOneNamedStep)
{
    const ECS::EntityHandle square = CreateSquare();
    PavingStatus status;
    status.Entity = square;

    std::unique_ptr<InspectorSection> section = Build(square, status);
    for (const char* label : {"Corner", "Shape", "Conform Target"})
    {
        UIElement* row = RowLabeled(*section, label);
        ASSERT_NE(row, nullptr) << label;
        EXPECT_NE(FirstWithClass(*row, "picture-choice-field"), nullptr) << label;
        EXPECT_TRUE(row->HasClass("picture-choice-row")) << label;
    }
    EXPECT_TRUE(Shows(*section, "Miter"));
    EXPECT_TRUE(Shows(*section, "Flared"));
    EXPECT_TRUE(Shows(*section, "Terrain Only"));

    PickTile(*section, "Corner", 1);
    EXPECT_EQ(World.GetComponent<Components::SplinePaving>(square)->BorderCorner, Components::SplineWallCorner::Round);
    EXPECT_STREQ(Undo.PeekUndoName(), "Change Spline Paving Corner");
    PickTile(*section, "Conform Target", 1);
    EXPECT_EQ(World.GetComponent<Components::SplinePaving>(square)->ConformTarget, Components::SplineConformTarget::Scene);

    Undo.Undo();
    EXPECT_EQ(World.GetComponent<Components::SplinePaving>(square)->ConformTarget,
              Components::SplineConformTarget::TerrainOnly);
}

TEST_F(SplinePavingInspectorTest, TheRadiusIsASliderThatReadsInactiveWithAStraightMouthAndSaysWhy)
{
    const ECS::EntityHandle square = CreateSquare();
    PavingStatus status;
    status.Entity = square;

    std::unique_ptr<InspectorSection> flared = Build(square, status);
    ASSERT_NE(RowLabeled(*flared, "Radius (m)"), nullptr);
    EXPECT_TRUE(RowLabeled(*flared, "Radius (m)")->IsEnabled());
    EXPECT_NE(FirstWithClass(*RowLabeled(*flared, "Radius (m)"), "slider"), nullptr);

    PickTile(*flared, "Shape", 1);
    ASSERT_EQ(World.GetComponent<Components::SplinePaving>(square)->Mouth, Components::SplinePavingMouth::Straight);
    std::unique_ptr<InspectorSection> straight = Build(square, status);

    UIElement* radius = RowLabeled(*straight, "Radius (m)");
    ASSERT_NE(radius, nullptr);
    EXPECT_FALSE(radius->IsEnabled());
    EXPECT_EQ(radius->GetTooltip(), "A Straight mouth has no curve. Choose Flared to set its radius.");
    const auto* label = dynamic_cast<const Label*>(FirstWithClass(*radius, "inspector-label"));
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->GetTooltip(), radius->GetTooltip()) << "a hover on the greyed label hides the reason";
}

TEST_F(SplinePavingInspectorTest, TheSeamRowsFollowTheRadiusAndARowTheSeamDoesNotUseSaysWhichSeamUsesIt)
{
    const ECS::EntityHandle square = CreateSquare();
    PavingStatus status;
    status.Entity = square;

    std::unique_ptr<InspectorSection> blend = Build(square, status);
    UIElement* seam = RowLabeled(*blend, "Seam");
    ASSERT_NE(seam, nullptr);
    EXPECT_EQ(SectionTitleOf(*seam), "Road Mouths");
    EXPECT_TRUE(Shows(*seam, "Blend"));
    EXPECT_LT(TextPosition(*blend, "Radius (m)"), TextPosition(*blend, "Seam"));
    std::vector<UIElement*> rows = RowsFrom(*seam);
    ASSERT_EQ(rows.size(), 4u) << "Seam, Band, Course Width and Course Material";
    EXPECT_TRUE(Shows(*rows[1], "Blend Width (tiles)"));
    EXPECT_TRUE(Shows(*rows[2], "Course Width (m)"));
    EXPECT_TRUE(Shows(*rows[3], "Course Material"));
    for (UIElement* row : rows)
    {
        const auto* label = dynamic_cast<const Label*>(FirstWithClass(*row, "inspector-label"));
        ASSERT_NE(label, nullptr);
        EXPECT_FALSE(label->GetTooltip().empty()) << label->GetText() << " has no tooltip";
    }
    const char* courseReason = "Only a Joint Line or a Stone Course has a width and a material. Choose one to set them.";
    EXPECT_TRUE(rows[1]->IsEnabled());
    EXPECT_NE(FirstWithClass(*rows[1], "slider"), nullptr);
    EXPECT_FALSE(rows[2]->IsEnabled());
    EXPECT_EQ(rows[2]->GetTooltip(), courseReason);
    EXPECT_FALSE(rows[3]->IsEnabled());
    EXPECT_EQ(rows[3]->GetTooltip(), courseReason);
    EXPECT_TRUE(Shows(*rows[3], "Paving")) << "an unused course names no look";

    World.GetComponentForWrite<Components::SplinePaving>(square)->Seam = Components::SplinePavingSeam::StoneCourse;
    std::unique_ptr<InspectorSection> stones = Build(square, status);
    seam = RowLabeled(*stones, "Seam");
    ASSERT_NE(seam, nullptr);
    EXPECT_TRUE(Shows(*seam, "Stone Course"));
    rows = RowsFrom(*seam);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_FALSE(rows[1]->IsEnabled());
    EXPECT_EQ(rows[1]->GetTooltip(), "Only a Blend seam has a band. Choose Blend to set how far it reaches into the road.");
    EXPECT_TRUE(rows[2]->IsEnabled());
    // The texts fit the 60 px number box and the asset field; the whole statement is the tooltip.
    EXPECT_EQ(NumberBoxText(*rows[2]), "Auto") << "a width of 0 says it is automatic";
    ASSERT_NE(FirstNumberBox(*rows[2]), nullptr);
    EXPECT_EQ(FirstNumberBox(*rows[2])->GetTooltip(),
              "Auto: a row of stones half a tile of the other pattern wide. Type a width in meters to set it.");
    EXPECT_TRUE(rows[3]->IsEnabled());
    EXPECT_TRUE(Shows(*rows[3], "Paving, as stones"));
    EXPECT_EQ(rows[3]->GetTooltip(), "Empty: the paving's material draws the course, its stones turned across the "
                                     "seam and a little brighter.");

    World.GetComponentForWrite<Components::SplinePaving>(square)->Seam = Components::SplinePavingSeam::JointLine;
    std::unique_ptr<InspectorSection> joint = Build(square, status);
    seam = RowLabeled(*joint, "Seam");
    ASSERT_NE(seam, nullptr);
    rows = RowsFrom(*seam);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(NumberBoxText(*rows[2]), "Auto");
    ASSERT_NE(FirstNumberBox(*rows[2]), nullptr);
    EXPECT_EQ(FirstNumberBox(*rows[2])->GetTooltip(), "Auto: a 3 cm joint. Type a width in meters to set it.");
    EXPECT_TRUE(Shows(*rows[3], "Paving, darker"));
    EXPECT_EQ(rows[3]->GetTooltip(), "Empty: the paving's material draws the joint, darker.");
}

// A junction whose Blend seam draws as a hard cut names the seam it gets in its state.
TEST_F(SplinePavingInspectorTest, AJunctionWhoseStripEndsOnTheSeamReadsHardCut)
{
    const ECS::EntityHandle square = CreateSquare();
    const ECS::EntityHandle road =
        CreateSpline("Mill Lane", {Vector3(-40.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 0.0f)}, false, &kRoad);
    PavingStatus status;
    status.Entity = square;
    status.Role = SplineRecipes::PavingRole::Owns;
    PavingJunctionStatus junction{1u, SplineECS::SplineNodeKind::Entry, road, 0.0f, PavingJunctionState::Paved, {}, 0.0f};
    junction.SeamDrawsHardCut = true;
    status.Junctions.push_back(junction);

    std::unique_ptr<InspectorSection> section = Build(square, status);

    EXPECT_EQ(TextOf(*section, "spline-junction-state"), "paved \xC2\xB7 Hard Cut: the road's paving ends on the seam");
}

// The Seam is picture tiles like Shape and Corner, each with a one-line caption. Under Blend, an
// Information notice under the tiles says which form the material that draws the seams gives: bending
// with a Square or Rectangular pattern, the crossfade with none. No other seam shows the notice.
TEST_F(SplinePavingInspectorTest, TheSeamIsPictureTilesAndABlendNoticeSaysWhichFormTheMaterialGives)
{
    const ECS::EntityHandle square = CreateSquare();
    PavingStatus status;
    status.Entity = square;

    std::unique_ptr<InspectorSection> unbuilt = Build(square, status);
    UIElement* seam = RowLabeled(*unbuilt, "Seam");
    ASSERT_NE(seam, nullptr);
    EXPECT_NE(FirstWithClass(*seam, "picture-choice-field"), nullptr);
    EXPECT_TRUE(seam->HasClass("picture-choice-row"));
    std::vector<UIElement*> tiles;
    CollectWithClass(*seam, "picture-choice-tile", tiles);
    ASSERT_EQ(tiles.size(), 4u);
    const char* names[] = {"Hard Cut", "Blend", "Joint Line", "Stone Course"};
    for (std::size_t i = 0; i < tiles.size(); ++i)
        EXPECT_TRUE(Shows(*tiles[i], names[i])) << names[i];
    EXPECT_TRUE(Shows(*tiles[0], "The road's pattern stops on the seam."));
    EXPECT_TRUE(Shows(*tiles[1], "Bends or fades into the other pattern."));
    EXPECT_TRUE(Shows(*tiles[2], "A thin line along the seam."));
    EXPECT_TRUE(Shows(*tiles[3], "A row of stones along the seam."));
    EXPECT_TRUE(ShowsNotice(*unbuilt, "No seam uses this setting yet.")) << "Blend shows its notice";
    EXPECT_LT(TextPosition(*unbuilt, "Bends or fades into the other pattern."),
              TextPosition(*unbuilt, "No seam uses this setting yet."))
        << "the notice lies under the tiles";

    status.SeamPatternSymmetry = MaterialPatternSymmetry::Square;
    std::unique_ptr<InspectorSection> bending = Build(square, status);
    EXPECT_TRUE(ShowsNotice(*bending, "Bending: this paving's material has a Square pattern."));

    status.SeamPatternSymmetry = MaterialPatternSymmetry::Rectangular;
    std::unique_ptr<InspectorSection> aligned = Build(square, status);
    EXPECT_TRUE(
        ShowsNotice(*aligned, "Bending where the patterns line up: this paving's material has a Rectangular pattern."));

    status.SeamPatternSymmetry = MaterialPatternSymmetry::None;
    std::unique_ptr<InspectorSection> crossfade = Build(square, status);
    const char* crossfadeText =
        "Crossfade: this paving's material has no Pattern Symmetry. Set Pattern Symmetry in the material to bend.";
    EXPECT_TRUE(ShowsNotice(*crossfade, crossfadeText));

    PavingStatus mixed;
    mixed.Entity = square;
    mixed.SeamPatternMixed = true;
    std::unique_ptr<InspectorSection> differing = Build(square, mixed);
    EXPECT_TRUE(ShowsNotice(*differing, "The seams this setting reaches meet materials with different Pattern "
                                        "Symmetry: each bends or crossfades by its own."));

    PickTile(*crossfade, "Seam", 3);
    EXPECT_EQ(World.GetComponent<Components::SplinePaving>(square)->Seam, Components::SplinePavingSeam::StoneCourse);
    EXPECT_STREQ(Undo.PeekUndoName(), "Change Spline Paving Seam");
    std::unique_ptr<InspectorSection> stones = Build(square, status);
    EXPECT_FALSE(ShowsNotice(*stones, crossfadeText)) << "only Blend shows the notice";
}

// On a road, the Blend notice names the paving the road gives way to, whose material draws the seam.
TEST_F(SplinePavingInspectorTest, ARoadsBlendNoticeNamesThePavingItGivesWayTo)
{
    const ECS::EntityHandle road =
        CreateSpline("High Street", {Vector3(-20.0f, 0.0f, 40.0f), Vector3(20.0f, 0.0f, 40.0f)}, false, &kRoad);
    World.AddComponentImmediate(road, Components::SplinePaving{});
    PavingStatus status;
    status.Entity = road;
    status.SeamPatternSymmetry = MaterialPatternSymmetry::Square;

    std::unique_ptr<InspectorSection> section = Build(road, status);

    EXPECT_TRUE(ShowsNotice(*section, "Bending: the paving it gives way to has a Square pattern."));
}

TEST_F(SplinePavingInspectorTest, EachListIsASectionWithTheNameInTheLabelColumnAndTheStateInTheValueColumn)
{
    const ECS::EntityHandle square = CreateSquare();
    const ECS::EntityHandle road =
        CreateSpline("Mill Lane", {Vector3(-40.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 0.0f)}, false, &kRoad);
    CreateSpline("Bed 1", {Vector3(-2.0f, 0.0f, -2.0f), Vector3(2.0f, 0.0f, -2.0f), Vector3(0.0f, 0.0f, 2.0f)}, true,
                 nullptr);
    PavingStatus status;
    status.Entity = square;
    status.Junctions.push_back({1u, SplineECS::SplineNodeKind::Entry, road, 0.0f, PavingJunctionState::Paved, {}, 0.0f});

    std::unique_ptr<InspectorSection> section = Build(square, status);

    for (const char* name : {"Bed 1", "Mill Lane"})
    {
        UIElement* row = MemberRow(*section, name);
        ASSERT_NE(row, nullptr) << name;
        EXPECT_EQ(SectionTitleOf(*row), "Splines It Touches") << name;
        EXPECT_TRUE(InColumn(*FirstWithClass(*row, "entity-member-title"), "inspector-label-cell")) << name;
        EXPECT_TRUE(InColumn(*FirstWithClass(*row, "entity-member-toggle"), "inspector-field")) << name;
    }
    EXPECT_FALSE(Shows(*section, "Splines it touches")) << "the list repeats its section's title";

    std::vector<UIElement*> junctions;
    CollectWithClass(*section, "spline-junction-row", junctions);
    ASSERT_EQ(junctions.size(), 1u);
    EXPECT_EQ(SectionTitleOf(*junctions[0]), "Junctions");
    EXPECT_TRUE(InColumn(*FirstWithClass(*junctions[0], "spline-junction-title"), "inspector-label-cell"));
    EXPECT_TRUE(InColumn(*FirstWithClass(*junctions[0], "spline-junction-state"), "inspector-field"));
    std::vector<std::string> texts;
    CollectTexts(*section, texts);
    EXPECT_EQ(std::count(texts.begin(), texts.end(), "Junctions"), 1) << "the list repeats its section's title";
}

TEST_F(SplinePavingInspectorTest, AnEmptyListSaysSoInOneSentence)
{
    const ECS::EntityHandle square = CreateSquare();
    PavingStatus status;
    status.Entity = square;

    std::unique_ptr<InspectorSection> section = Build(square, status);

    EXPECT_TRUE(Shows(*section, "No road meets this paving, and no closed spline overlaps it."));
    EXPECT_TRUE(Shows(*section, "No road joins this paving. To join one, drag a road's end onto the paving."));
}

} // namespace
} // namespace GameEngine
