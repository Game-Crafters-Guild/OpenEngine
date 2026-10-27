// The editor-drawn context menu's submenus, over the editor's real stylesheets and real pointer
// dispatch: a press on a submenu parent opens its submenu and leaves the menu open (only a leaf
// command or a press outside every panel closes it), and a submenu opens beside its parent
// panel, never over it, its first row level with the parent row, on the left when the right
// has no room. A row that names its key shows it at the row's trailing edge.

#include <gtest/gtest.h>

#include "EditorContextMenu/UIContextMenu.h"
#include "InspectorLayoutFixture.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

// The registry keys on the pointer alone; Show() resolves the UIManager from it and never
// dereferences the window.
Platform::Window* FakeWindow()
{
    return reinterpret_cast<Platform::Window*>(0x2);
}

constexpr float kFrameSeconds = 1.0f / 60.0f;
// Hover opens a submenu through a deferred action, and a new panel takes a few layouts to
// measure before it shows; this many frames covers both with room to spare.
constexpr int kSettleFrames = 12;
// Layout rounds to a fraction of a pixel.
constexpr float kTolerance = 0.5f;
// The space allowed between a submenu and the panel it opened from.
constexpr float kMaxSubmenuGap = 2.0f;

constexpr uint32_t kCmdLeaf = 1;
constexpr uint32_t kCmdChildA = 2;
constexpr uint32_t kCmdChildB = 3;

struct Rect
{
    float X = 0.0f;
    float Y = 0.0f;
    float Width = 0.0f;
    float Height = 0.0f;
    float Right() const { return X + Width; }
    float Bottom() const { return Y + Height; }
};

Rect RectOf(const UIElement& element)
{
    return {element.GetLayoutX(), element.GetLayoutY(), element.GetLayoutWidth(), element.GetLayoutHeight()};
}

bool Intersects(const Rect& a, const Rect& b)
{
    return a.X < b.Right() - kTolerance && b.X < a.Right() - kTolerance && a.Y < b.Bottom() - kTolerance &&
           b.Y < a.Bottom() - kTolerance;
}

void CollectWithClass(UIElement* element, const char* className, std::vector<UIElement*>& out)
{
    if (!element)
        return;
    if (element->HasClass(className))
        out.push_back(element);
    for (const auto& child : element->GetChildren())
        CollectWithClass(child.get(), className, out);
}

std::vector<UIElement*> OpenPanels(UIManager& ui)
{
    std::vector<UIElement*> panels;
    CollectWithClass(ui.GetRootElement(), "context-menu-panel", panels);
    return panels;
}

// The menu row whose label reads `title`.
UIElement* FindRow(UIElement* scope, const std::string& title)
{
    std::vector<UIElement*> rows;
    CollectWithClass(scope, "dropdown-item", rows);
    for (UIElement* row : rows)
    {
        for (const auto& child : row->GetChildren())
        {
            if (const auto* label = dynamic_cast<const Label*>(child.get()); label && label->GetText() == title)
                return row;
        }
    }
    return nullptr;
}

// A menu over the editor's widget sheet, shown at a point, with one submenu parent and a leaf.
class UIContextMenuSubmenuTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Fixture.ExtraSheets = {"Assets/UI/theme/widgets.css"};
        if (!m_Fixture.Build(std::make_unique<UIElement>()))
            GTEST_SKIP() << m_Fixture.Diagnostic;
        UIContextMenu::Register(FakeWindow(), m_Fixture.Ui.get());
        m_Menu = std::make_unique<UIContextMenu>();
        const uint32_t create = m_Menu->AddSubMenu(0, "Create");
        m_Menu->AddItem(create, "Child A", kCmdChildA);
        m_Menu->AddItem(create, "Child B", kCmdChildB);
        m_Menu->AddItem(0, "Leaf", kCmdLeaf);
        m_Menu->SetCommandHandler([this](uint32_t id) { m_Commands.push_back(id); });
    }

    void TearDown() override
    {
        m_Menu.reset();
        if (m_Fixture.Ui)
            UIContextMenu::Unregister(FakeWindow());
    }

    UIManager& Ui() { return *m_Fixture.Ui; }

    void Settle()
    {
        for (int i = 0; i < kSettleFrames; ++i)
            Ui().Update(kFrameSeconds, /*interactive=*/true);
    }

    void ShowAt(float x, float y)
    {
        m_Menu->Show(FakeWindow(), static_cast<int>(x), static_cast<int>(y));
        Settle();
    }

    void MoveOver(const UIElement& element)
    {
        Ui().OnMouseMove(element.GetLayoutX() + element.GetLayoutWidth() * 0.5f,
                         element.GetLayoutY() + element.GetLayoutHeight() * 0.5f);
        Settle();
    }

    void MoveTo(float x, float y)
    {
        Ui().OnMouseMove(x, y);
        Settle();
    }

    void Click()
    {
        Ui().OnMouseButton(0, true);
        Ui().Update(kFrameSeconds, /*interactive=*/true);
        Ui().OnMouseButton(0, false);
        Settle();
    }

    // Hovers the Create row and returns {root panel, Create row, submenu panel}; any null means
    // the submenu did not open.
    struct OpenSubmenu
    {
        UIElement* Root = nullptr;
        UIElement* Parent = nullptr;
        UIElement* Submenu = nullptr;
    };
    OpenSubmenu HoverCreate()
    {
        OpenSubmenu result;
        const auto panels = OpenPanels(Ui());
        if (panels.size() != 1)
            return result;
        result.Root = panels[0];
        result.Parent = FindRow(result.Root, "Create");
        if (!result.Parent)
            return result;
        MoveOver(*result.Parent);
        const auto after = OpenPanels(Ui());
        for (UIElement* panel : after)
        {
            if (panel != result.Root)
                result.Submenu = panel;
        }
        return result;
    }

    InspectorLayoutTesting::InspectorLayoutFixture m_Fixture;
    std::unique_ptr<UIContextMenu> m_Menu;
    std::vector<uint32_t> m_Commands;
};

} // namespace

TEST_F(UIContextMenuSubmenuTests, APressOnASubmenuParentLeavesTheMenuOpenWithItsSubmenu)
{
    ShowAt(100.0f, 100.0f);
    const OpenSubmenu open = HoverCreate();
    ASSERT_NE(open.Root, nullptr) << "the menu did not open";
    ASSERT_NE(open.Parent, nullptr) << "the menu has no Create row";
    ASSERT_NE(open.Submenu, nullptr) << "hovering Create did not open its submenu; the test is vacuous";

    Click();

    const auto panels = OpenPanels(Ui());
    EXPECT_EQ(panels.size(), 2u)
        << "a press on a submenu parent closed the menu; it must open or keep its submenu and leave "
           "the menu open, as every desktop menu does";
    EXPECT_NE(FindRow(Ui().GetRootElement(), "Child A"), nullptr) << "the submenu is no longer open";
    EXPECT_TRUE(m_Commands.empty()) << "a submenu parent ran a command";
}

TEST_F(UIContextMenuSubmenuTests, APressOnALeafRunsItsCommandAndClosesTheMenu)
{
    ShowAt(100.0f, 100.0f);
    const auto panels = OpenPanels(Ui());
    ASSERT_EQ(panels.size(), 1u) << "the menu did not open";
    UIElement* leaf = FindRow(panels[0], "Leaf");
    ASSERT_NE(leaf, nullptr);

    MoveOver(*leaf);
    Click();

    EXPECT_EQ(m_Commands, std::vector<uint32_t>{kCmdLeaf});
    EXPECT_TRUE(OpenPanels(Ui()).empty()) << "a leaf command left the menu open";
}

TEST_F(UIContextMenuSubmenuTests, APressOutsideEveryPanelClosesTheMenu)
{
    ShowAt(100.0f, 100.0f);
    ASSERT_NE(HoverCreate().Submenu, nullptr) << "the submenu did not open; the test is vacuous";

    MoveTo(700.0f, 550.0f);
    Click();

    EXPECT_TRUE(OpenPanels(Ui()).empty()) << "a press outside the menu left it open";
    EXPECT_TRUE(m_Commands.empty());
}

TEST_F(UIContextMenuSubmenuTests, ASubmenuOpensBesideItsParentPanelAlignedWithTheParentRow)
{
    ShowAt(100.0f, 100.0f);
    const OpenSubmenu open = HoverCreate();
    ASSERT_NE(open.Submenu, nullptr) << "the submenu did not open; the test is vacuous";
    UIElement* firstChild = FindRow(open.Submenu, "Child A");
    ASSERT_NE(firstChild, nullptr);

    const Rect parentPanel = RectOf(*open.Root);
    const Rect submenu = RectOf(*open.Submenu);
    const Rect parentRow = RectOf(*open.Parent);
    const Rect childRow = RectOf(*firstChild);
    ASSERT_GT(submenu.Width, 0.0f);

    const float gap = submenu.X - parentPanel.Right();
    EXPECT_GE(gap, -kTolerance) << "the submenu overlaps its parent panel by " << -gap << " px";
    EXPECT_LE(gap, kMaxSubmenuGap + kTolerance) << "the submenu stands " << gap << " px off its parent panel";
    EXPECT_FALSE(Intersects(parentPanel, submenu)) << "the submenu covers part of the panel it opened from";
    EXPECT_NEAR(childRow.Y, parentRow.Y, kTolerance) << "the submenu's first row is not level with its parent row";
}

TEST_F(UIContextMenuSubmenuTests, ASubmenuFlipsToTheLeftWhenTheRightSideHasNoRoom)
{
    // The fixture viewport is 800 px wide: a menu opened this far right leaves no room beside it.
    ShowAt(560.0f, 100.0f);
    const OpenSubmenu open = HoverCreate();
    ASSERT_NE(open.Submenu, nullptr) << "the submenu did not open; the test is vacuous";

    const Rect parentPanel = RectOf(*open.Root);
    const Rect submenu = RectOf(*open.Submenu);
    ASSERT_GT(parentPanel.Right() + submenu.Width, 800.0f)
        << "the menu opened with room on its right; move it further right for this test to mean anything";

    const float gap = parentPanel.X - submenu.Right();
    EXPECT_GE(gap, -kTolerance) << "the flipped submenu overlaps its parent panel by " << -gap << " px";
    EXPECT_LE(gap, kMaxSubmenuGap + kTolerance) << "the flipped submenu stands " << gap << " px off its parent";
    EXPECT_FALSE(Intersects(parentPanel, submenu));
    EXPECT_GE(submenu.X, 0.0f) << "the flipped submenu left the window";
}

// A disabled row keeps its tooltip, where it says why it is off.
TEST_F(UIContextMenuSubmenuTests, ADisabledRowShowsItsTooltip)
{
    m_Menu->SetItemTooltip(kCmdLeaf, "Leaf is off. Select a root to turn it on.");
    m_Menu->SetItemEnabled(kCmdLeaf, false);
    ShowAt(100.0f, 100.0f);
    const auto panels = OpenPanels(Ui());
    ASSERT_EQ(panels.size(), 1u) << "the menu did not open";
    UIElement* leaf = FindRow(panels[0], "Leaf");
    ASSERT_NE(leaf, nullptr);
    EXPECT_TRUE(leaf->HasClass("disabled"));
    EXPECT_EQ(leaf->GetTooltip(), "Leaf is off. Select a root to turn it on.");
}

TEST_F(UIContextMenuSubmenuTests, ARowShowsItsKeyAfterItsTitle)
{
    m_Menu->SetItemShortcut(kCmdLeaf, "F2");
    ShowAt(100.0f, 100.0f);
    const auto panels = OpenPanels(Ui());
    ASSERT_EQ(panels.size(), 1u) << "the menu did not open";
    UIElement* leaf = FindRow(panels[0], "Leaf");
    ASSERT_NE(leaf, nullptr);

    const Label* title = nullptr;
    const Label* key = nullptr;
    for (const auto& child : leaf->GetChildren())
    {
        if (const auto* label = dynamic_cast<const Label*>(child.get()))
            (label->GetText() == "Leaf" ? title : key) = label;
    }
    ASSERT_NE(title, nullptr);
    ASSERT_NE(key, nullptr) << "the row shows no key";
    EXPECT_EQ(key->GetText(), "F2");
    EXPECT_GE(key->GetLayoutX(), title->GetLayoutX() + title->GetLayoutWidth() - kTolerance)
        << "the key overlaps the title";
    // Right-aligned: the key ends where a parent row's chevron does, near the panel's edge.
    const float keyRight = key->GetLayoutX() + key->GetLayoutWidth();
    const float panelRight = RectOf(*panels[0]).Right();
    EXPECT_LE(keyRight, panelRight + kTolerance) << "the key runs past the menu's edge";
    EXPECT_GE(keyRight, panelRight - 24.0f) << "the key is not at the row's trailing edge";
}
