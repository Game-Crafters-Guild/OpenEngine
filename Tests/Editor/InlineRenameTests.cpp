// Inline rename, the session the Assets browser and the Hierarchy share, over a real UIManager and
// real key dispatch: Enter commits an accepted name, Escape cancels, a refused name keeps the
// field open with the reason on screen, and a click elsewhere commits or cancels and leaves the
// keyboard focus on the element clicked. And the Hierarchy's entity rename: one undo step that writes the
// Name and announces it, and a field that stays open while its row is bound again to the same entity.

#include <gtest/gtest.h>

#include "Editor/Hierarchy/EntityNameEdit.h"
#include "Editor/Hierarchy/HierarchyEntityRename.h"
#include "EditorChangeNotifications.h"
#include "InspectorLayoutFixture.h"
#include "UI/InlineRenameSession.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Input/KeyCodes.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr float kFrameSeconds = 1.0f / 60.0f;
constexpr int kKeyPress = 1;

class InlineRenameSessionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto root = std::make_unique<UIElement>();
        auto row = std::make_unique<UIElement>();
        auto title = std::make_unique<Label>();
        title->SetText("Rock");
        m_Title = title.get();
        m_Row = row.get();
        row->SetFocusable(true);
        row->SetId("rename-test-row");
        row->AddChild(std::move(title));
        root->AddChild(std::move(row));
        // Another focusable element away from the row, the target of a click away from the field.
        auto other = std::make_unique<UIElement>();
        other->SetFocusable(true);
        other->SetId("rename-test-other");
        UI::Layout::SetAbsolutePosition(*other, Mathematics::Rect{400.0f, 300.0f, 120.0f, 40.0f});
        m_Other = other.get();
        root->AddChild(std::move(other));
        if (!m_Fixture.Build(std::move(root)))
            GTEST_SKIP() << m_Fixture.Diagnostic;
    }

    void Begin()
    {
        m_Session.Begin(
            m_Title, m_Row, "Rock", nullptr,
            [](const std::string& name) { return name == "Taken" ? std::string("'Taken' already exists here") : std::string{}; },
            [this](const std::string& name) { m_Committed.push_back(name); });
        Frame();
    }

    TextField* Field() { return dynamic_cast<TextField*>(InspectorLayoutTesting::FindByClass(m_Row, "inline-rename-field")); }

    void Type(const std::string& text)
    {
        TextField* field = Field();
        ASSERT_NE(field, nullptr);
        field->SetValue(text);
    }

    void Press(int key)
    {
        m_Fixture.Ui->OnKey(key, kKeyPress, 0);
        Frame();
    }

    void Frame() { m_Fixture.Ui->Update(kFrameSeconds, /*interactive=*/true); }

    void ClickOther()
    {
        m_Fixture.Ui->OnMouseMove(m_Other->GetLayoutX() + m_Other->GetLayoutWidth() * 0.5f,
                                  m_Other->GetLayoutY() + m_Other->GetLayoutHeight() * 0.5f);
        Frame();
        m_Fixture.Ui->OnMouseButton(0, true);
        Frame();
        m_Fixture.Ui->OnMouseButton(0, false);
        Frame();
    }

    bool TitleHidden() const
    {
        const auto display = m_Title->Overrides().Get(Style::Display);
        return display.has_value() && *display == DisplayMode::None;
    }

    InspectorLayoutTesting::InspectorLayoutFixture m_Fixture;
    UIElement* m_Row = nullptr;
    UIElement* m_Other = nullptr;
    Label* m_Title = nullptr;
    InlineRenameSession m_Session;
    std::vector<std::string> m_Committed;
};

} // namespace

TEST_F(InlineRenameSessionTest, OpensFocusedOnTheCurrentName)
{
    Begin();
    TextField* field = Field();
    ASSERT_NE(field, nullptr) << "no rename field opened";
    EXPECT_EQ(field->GetValue(), "Rock");
    EXPECT_EQ(m_Fixture.Ui->GetFocusedElementId(), field->GetId()) << "the field does not have keyboard focus";
    EXPECT_TRUE(TitleHidden()) << "the title still shows behind the field";
}

TEST_F(InlineRenameSessionTest, EnterCommitsTheNewName)
{
    Begin();
    Type("Boulder");
    Press(Input::kKeyCode_Enter);

    EXPECT_EQ(m_Committed, std::vector<std::string>{"Boulder"});
    EXPECT_FALSE(m_Session.IsActive());
    EXPECT_EQ(m_Fixture.Ui->GetFocusedElementId(), "rename-test-row") << "focus did not return to the view";
}

TEST_F(InlineRenameSessionTest, EscapeCancelsAndRestoresTheTitle)
{
    Begin();
    Type("Boulder");
    Press(Input::kKeyCode_Escape);

    EXPECT_TRUE(m_Committed.empty()) << "Escape committed the rename";
    EXPECT_FALSE(m_Session.IsActive());
    EXPECT_FALSE(TitleHidden()) << "the title did not come back";
    EXPECT_EQ(Field(), nullptr) << "the field outlived the session";
}

TEST_F(InlineRenameSessionTest, ARefusedNameKeepsTheFieldOpenWithTheReason)
{
    Begin();
    Type("Taken");
    Press(Input::kKeyCode_Enter);

    EXPECT_TRUE(m_Committed.empty()) << "a colliding name was committed";
    ASSERT_TRUE(m_Session.IsActive()) << "a refused name closed the field";
    TextField* field = Field();
    ASSERT_NE(field, nullptr);
    EXPECT_TRUE(field->HasClass("invalid"));
    EXPECT_EQ(field->GetTooltip(), "'Taken' already exists here") << "the refusal does not name the clash";
}

TEST(EntityRename, RefusesBlankAndOverlongNamesAndAcceptsDuplicates)
{
    EXPECT_EQ(Editor::ValidateEntityName(""), "Name can't be empty");
    EXPECT_EQ(Editor::ValidateEntityName("   "), "Name can't be empty");
    EXPECT_FALSE(Editor::ValidateEntityName(std::string(64, 'a')).empty());
    EXPECT_TRUE(Editor::ValidateEntityName(std::string(63, 'a')).empty());
    EXPECT_TRUE(Editor::ValidateEntityName("Cube").empty()) << "entity names need not be unique";
}

TEST(EntityRename, OneUndoStepThatWritesAndAnnouncesTheName)
{
    ECS::World world;
    Components::Name original{};
    std::strcpy(original.value, "Cube");
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, original);

    Editor::EditorChangeNotifications notifications;
    int nameCommits = 0;
    const auto token = notifications.SubscribeComponentChanged(
        [&](const Editor::EditorChangeNotifications::ComponentChangedEvent& e)
        {
            if (e.entity == entity && e.componentType == ECS::GetComponentTypeId<Components::Name>() &&
                e.kind == Editor::EditorChangeNotifications::ChangeKind::Commit)
                ++nameCommits;
        });
    Editor::UndoRedoService undo;

    ASSERT_TRUE(Editor::CommitEntityRename(world, entity, "Crate", &undo, &notifications));
    EXPECT_EQ(world.GetComponent<Components::Name>(entity)->View(), "Crate");
    EXPECT_EQ(nameCommits, 1) << "the Hierarchy and the Inspector were not told about the new name";

    undo.Undo();
    EXPECT_EQ(world.GetComponent<Components::Name>(entity)->View(), "Cube");
    notifications.Unsubscribe(token);
}

// The refusal shows while the user types, not only once Enter is pressed.
TEST_F(InlineRenameSessionTest, ARefusedNameIsMarkedAsItIsTyped)
{
    Begin();
    for (const char c : std::string("Taken"))
        m_Fixture.Ui->OnChar(static_cast<unsigned int>(c));
    Frame();

    TextField* field = Field();
    ASSERT_NE(field, nullptr);
    ASSERT_EQ(field->GetValue(), "Taken") << "typing did not replace the selected name; the test is vacuous";
    EXPECT_TRUE(field->HasClass("invalid")) << "the colliding name is not marked until Enter";
    EXPECT_EQ(field->GetTooltip(), "'Taken' already exists here");
}

// F2 is a keyboard path: the pointer is away from the field, and the reason still shows, at once,
// for as long as the name is refused.
TEST_F(InlineRenameSessionTest, ARefusedNameShowsItsReasonWithoutAHoverUntilTheNameIsAccepted)
{
    Begin();
    m_Fixture.Ui->OnMouseMove(m_Other->GetLayoutX() + 4.0f, m_Other->GetLayoutY() + 4.0f);
    for (const char c : std::string("Taken"))
        m_Fixture.Ui->OnChar(static_cast<unsigned int>(c));
    // Far shorter than the hover delay.
    for (int frame = 0; frame < 3; ++frame)
        Frame();

    UIElement* root = m_Fixture.Ui->GetRootElement();
    const UIElement* bubble = root->FindById("ui-tooltip-wrapper"_sid);
    const UIElement* text = root->FindById("ui-tooltip-text"_sid);
    ASSERT_NE(bubble, nullptr) << "no tooltip shown while the name is refused";
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->GetTextContent(), "'Taken' already exists here");
    EXPECT_GE(bubble->GetLayoutX(), 0.0f) << "the reason shows only under the pointer";
    EXPECT_GE(bubble->GetLayoutY(), 0.0f) << "the reason shows only under the pointer";

    m_Fixture.Ui->OnChar(static_cast<unsigned int>('2'));
    Frame();
    ASSERT_FALSE(Field()->HasClass("invalid")) << "'Taken2' is refused; the test is vacuous";
    EXPECT_LT(bubble->GetLayoutX(), 0.0f) << "the reason stayed on screen once the name was accepted";
}

// Focus that leaves the field by any route (Tab, a panel that takes it, a click) stays where it went.
TEST_F(InlineRenameSessionTest, LeavingTheFieldCommitsAnAcceptedNameAndKeepsFocusWhereItWent)
{
    Begin();
    Type("Boulder");
    m_Fixture.Ui->FocusElement(m_Other);
    Frame();

    EXPECT_EQ(m_Committed, std::vector<std::string>{"Boulder"});
    EXPECT_FALSE(m_Session.IsActive());
    EXPECT_EQ(m_Fixture.Ui->GetFocusedElementId(), "rename-test-other") << "the view took the keyboard back";
}

TEST_F(InlineRenameSessionTest, AClickAwayCancelsARefusedNameAndLeavesFocusOnTheClickedElement)
{
    Begin();
    Type("Taken");
    ClickOther();

    EXPECT_TRUE(m_Committed.empty()) << "a colliding name was committed";
    EXPECT_FALSE(m_Session.IsActive()) << "the field stayed open without the keyboard";
    EXPECT_FALSE(TitleHidden()) << "the title did not come back";
    EXPECT_EQ(Field(), nullptr) << "the field outlived the session";
    EXPECT_EQ(m_Fixture.Ui->GetFocusedElementId(), "rename-test-other") << "the view took the keyboard back";
}

// The Hierarchy binds its rows again on every selection change, and a slow second click on the
// selected entity sends the selection before the rename opens: the field stays open while its row
// shows the same entity and closes once the row shows another one.
TEST_F(InlineRenameSessionTest, AnEntityRenameStaysOpenWhileItsRowShowsTheSameEntity)
{
    ECS::World world;
    const ECS::EntityHandle renamed = world.CreateEntity();
    const ECS::EntityHandle other = world.CreateEntity();
    Components::Name name{};
    std::strcpy(name.value, "Rock");
    world.AddComponentImmediate(renamed, name);

    Editor::HierarchyEntityRename rename;
    rename.Begin(m_Title, m_Row, world, renamed, nullptr, nullptr);
    Frame();
    ASSERT_NE(Field(), nullptr) << "the rename did not open; the test is vacuous";

    rename.OnRowBound(*m_Other, other);
    rename.OnRowBound(*m_Row, renamed);
    Frame();
    EXPECT_EQ(rename.GetTitle(), m_Title) << "binding the row to the entity it renames closed the field";
    EXPECT_NE(Field(), nullptr) << "binding the row to the entity it renames closed the field";

    rename.OnRowBound(*m_Row, other);
    Frame();
    EXPECT_EQ(rename.GetTitle(), nullptr) << "the field stayed on a row that shows another entity";
    EXPECT_EQ(Field(), nullptr) << "the field stayed on a row that shows another entity";
    EXPECT_FALSE(TitleHidden()) << "the title did not come back";
}
