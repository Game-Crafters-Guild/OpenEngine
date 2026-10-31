#include "Toolbar/ClientToolbar.h"
#include "Platform/ContextMenu.h"
#include "Platform/Window.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace
{
using namespace GameEngine;

struct PopupRecord
{
    struct Row
    {
        uint32_t Parent = 0;
        std::string Title;
        uint32_t Command = 0;
        uint32_t Submenu = 0;
    };
    std::vector<Row> Rows;
    std::string Icon;
    std::string Shortcut;
    INativeContextMenu::CommandCallback Command;
    std::function<void()> Closed;
    Platform::Window* Window = nullptr;
    int X = 0;
    int Y = 0;
    int Shows = 0;
    int Clears = 0;
};

class RecordingPopup final : public INativeContextMenu
{
  public:
    explicit RecordingPopup(std::shared_ptr<PopupRecord> record) : m_Record(std::move(record)) {}
    void Clear() override { ++m_Record->Clears; m_Record->Rows.clear(); }
    uint32_t AddSubMenu(uint32_t parent, const std::string& title) override
    {
        const uint32_t id = ++m_NextId;
        m_Record->Rows.push_back({parent, title, 0, id});
        return id;
    }
    void AddItem(uint32_t parent, const std::string& title, uint32_t command, uint32_t) override
    {
        m_Record->Rows.push_back({parent, title, command, 0});
    }
    void AddSeparator(uint32_t) override {}
    void SetCommandHandler(CommandCallback callback) override { m_Record->Command = std::move(callback); }
    void SetCloseHandler(std::function<void()> callback) override { m_Record->Closed = std::move(callback); }
    void SetStateProvider(StateProviderCallback) override {}
    void SetItemEnabled(uint32_t, bool) override {}
    void SetItemChecked(uint32_t, bool) override {}
    void SetItemIcon(uint32_t, const std::string& icon) override { m_Record->Icon = icon; }
    void SetItemShortcut(uint32_t, const std::string& shortcut) override { m_Record->Shortcut = shortcut; }
    void Show(Platform::Window* window, int x, int y) override
    {
        m_Record->Window = window;
        m_Record->X = x;
        m_Record->Y = y;
        ++m_Record->Shows;
    }
  private:
    std::shared_ptr<PopupRecord> m_Record;
    uint32_t m_NextId = 100;
};

class ClientToolbarTests : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_Toolbar.SetMenuFactory([record = m_Record] { return std::make_unique<RecordingPopup>(record); });
        ASSERT_TRUE(m_Toolbar.Install(&m_Window));
        m_File = m_Toolbar.AddMenu("File");
        m_Toolbar.AddItem(m_File, "New Scene", kNewCommand);
        m_Toolbar.AddItem(m_File, "Load Scene", kLoadCommand);
        m_Toolbar.AddItem(m_File, "Save Scene", kSaveCommand);
        m_Toolbar.SetTitleBarLayout(32, {{m_File, 0, 0, 40, 32}});
    }
    static constexpr uint32_t kNewCommand = 0x4001;
    static constexpr uint32_t kLoadCommand = 0x4002;
    static constexpr uint32_t kSaveCommand = 0x4003;
    Platform::Window m_Window;
    std::shared_ptr<PopupRecord> m_Record = std::make_shared<PopupRecord>();
    ClientToolbar m_Toolbar;
    uint32_t m_File = 0;
};

TEST_F(ClientToolbarTests, UsesAClientMenuRowWithoutAPlatformWindowHandle)
{
    EXPECT_EQ(m_Window.GetGLFWHandle(), nullptr);
    EXPECT_EQ(m_Toolbar.GetPresentation(), ToolbarPresentation::ClientMenuBar);
    EXPECT_EQ(m_Toolbar.GetTitleBarMetrics().CaptionWidth, 0);
    EXPECT_FALSE(m_Toolbar.SetApplicationQuitCommand(kSaveCommand));
    const auto menus = m_Toolbar.GetMenus();
    ASSERT_EQ(menus.size(), 1u);
    EXPECT_EQ(menus[0].Title, "File");
    EXPECT_EQ(menus[0].Id, m_File);
}

TEST_F(ClientToolbarTests, OpensTheDeclaredSceneCommandsInOrder)
{
    m_Toolbar.ShowTitleBarMenu(m_File);
    ASSERT_EQ(m_Record->Rows.size(), 3u);
    EXPECT_EQ(m_Record->Rows[0].Title, "New Scene");
    EXPECT_EQ(m_Record->Rows[1].Command, kLoadCommand);
    EXPECT_EQ(m_Record->Rows[2].Title, "Save Scene");
    EXPECT_EQ(m_Record->Window, &m_Window);
    EXPECT_EQ(m_Record->Y, 32);
    EXPECT_EQ(m_Record->Shows, 1);
    EXPECT_TRUE(m_Toolbar.IsMenuOpen());
}

TEST_F(ClientToolbarTests, ConvertsPhysicalHeadingBoundsToLogicalPopupCoordinates)
{
    m_Window.SetUiContentScale(2.0f);
    m_Toolbar.SetTitleBarLayout(64, {{m_File, 80, 8, 100, 64}});
    m_Toolbar.ShowTitleBarMenu(m_File);
    EXPECT_EQ(m_Record->X, 40);
    EXPECT_EQ(m_Record->Y, 36);
}

TEST_F(ClientToolbarTests, OverflowRetainsTheCompleteNestedMenuTree)
{
    const auto window = m_Toolbar.AddMenu("Window");
    const auto layout = m_Toolbar.AddSubMenu(window, "Layout");
    m_Toolbar.AddItem(layout, "Reset Layout", 0x2001);
    m_Toolbar.SetTitleBarLayout(32, {{0, 12, 0, 60, 32}});
    m_Toolbar.ShowTitleBarMenu(0);
    ASSERT_EQ(m_Record->Rows.size(), 7u);
    EXPECT_EQ(m_Record->Rows[0].Title, "File");
    EXPECT_EQ(m_Record->Rows[4].Title, "Window");
    EXPECT_EQ(m_Record->Rows[5].Title, "Layout");
    EXPECT_EQ(m_Record->Rows[6].Parent, m_Record->Rows[5].Submenu);
    EXPECT_EQ(m_Record->Rows[6].Command, 0x2001u);
    EXPECT_EQ(m_Record->X, 12);
}

TEST_F(ClientToolbarTests, RelabelingKeepsIconsAndShortcutHints)
{
    m_Toolbar.SetItemIcon(kNewCommand, "editor:Icons/scene.svg");
    m_Toolbar.UpdateItemTitle(kNewCommand, "New Scene\tCtrl+N");
    m_Toolbar.ShowTitleBarMenu(m_File);
    ASSERT_FALSE(m_Record->Rows.empty());
    EXPECT_EQ(m_Record->Rows[0].Title, "New Scene");
    EXPECT_EQ(m_Record->Rows[0].Command, kNewCommand);
    EXPECT_EQ(m_Record->Icon, "editor:Icons/scene.svg");
    EXPECT_EQ(m_Record->Shortcut, "Ctrl+N");
}

TEST_F(ClientToolbarTests, PopupSelectionUsesTheExistingCommandDispatcher)
{
    uint32_t received = 0;
    m_Toolbar.SetCommandHandler([&received](uint32_t command) { received = command; });
    m_Toolbar.ShowTitleBarMenu(m_File);
    ASSERT_TRUE(m_Record->Command);
    m_Record->Command(kSaveCommand);
    EXPECT_EQ(received, kSaveCommand);
}

TEST_F(ClientToolbarTests, PopupDismissalReleasesTheMenuRefreshGate)
{
    m_Toolbar.ShowTitleBarMenu(m_File);
    ASSERT_TRUE(m_Record->Closed);
    m_Record->Closed();
    EXPECT_FALSE(m_Toolbar.IsMenuOpen());
}

TEST_F(ClientToolbarTests, ClearingAnOpenMenuDetachesCallbacksAndResetsTheInventory)
{
    m_Toolbar.ShowTitleBarMenu(m_File);
    const auto revision = m_Toolbar.GetMenuRevision();
    m_Toolbar.Clear();
    EXPECT_FALSE(m_Toolbar.IsMenuOpen());
    EXPECT_FALSE(m_Record->Command);
    EXPECT_FALSE(m_Record->Closed);
    EXPECT_TRUE(m_Toolbar.GetMenus().empty());
    EXPECT_GT(m_Toolbar.GetMenuRevision(), revision);
}

TEST_F(ClientToolbarTests, FullscreenSuspensionClosesMenusAndSuppressesNewPopups)
{
    m_Toolbar.ShowTitleBarMenu(m_File);
    m_Toolbar.SetTitleBarLayout(0, {});
    EXPECT_FALSE(m_Toolbar.IsMenuOpen());
    m_Toolbar.ShowTitleBarMenu(m_File);
    EXPECT_EQ(m_Record->Shows, 1);
}

TEST_F(ClientToolbarTests, InvalidParentsAndAbsentPresentersDoNotOpenMenus)
{
    EXPECT_EQ(m_Toolbar.AddSubMenu(9999, "Invalid"), 0u);
    m_Toolbar.ShowTitleBarMenu(9999);
    EXPECT_FALSE(m_Toolbar.IsMenuOpen());
    m_Toolbar.SetMenuFactory({});
    m_Toolbar.ShowTitleBarMenu(m_File);
    EXPECT_FALSE(m_Toolbar.IsMenuOpen());
    EXPECT_EQ(m_Record->Shows, 0);
}

TEST_F(ClientToolbarTests, ReinstallDiscardsOldCaptionBoundsAndMenus)
{
    m_Toolbar.Uninstall();
    ASSERT_TRUE(m_Toolbar.Install(&m_Window));
    const auto file = m_Toolbar.AddMenu("File");
    m_Toolbar.AddItem(file, "New Scene", kNewCommand);
    m_Toolbar.ShowTitleBarMenu(file);
    EXPECT_EQ(m_Record->Shows, 0);
}

TEST_F(ClientToolbarTests, KeyboardRoutingKeepsTheOriginalCallbackAndRestoresItOnUninstall)
{
    ASSERT_TRUE(Platform::Window::Initialize());
    Platform::WindowDesc description;
    description.Width = 800;
    description.Height = 500;
    description.StartHidden = true;
    description.Title = "Client toolbar keyboard test";
    ASSERT_TRUE(m_Window.Create(description));
    int originalCalls = 0;
    m_Window.SetKeyHandler([&originalCalls](int, int, int) { ++originalCalls; });
    ASSERT_TRUE(m_Toolbar.Install(&m_Window));
    const auto file = m_Toolbar.AddMenu("File");
    m_Toolbar.AddItem(file, "Save Scene", kSaveCommand);
    m_Toolbar.SetTitleBarLayout(32, {{file, 0, 0, 40, 32}});
    uint32_t selected = 0;
    m_Toolbar.SetCommandHandler([&selected](uint32_t command) { selected = command; });
    GLFWwindow* handle = m_Window.GetGLFWHandle();
    auto callback = glfwSetKeyCallback(handle, nullptr);
    ASSERT_NE(callback, nullptr);
    glfwSetKeyCallback(handle, callback);
    callback(handle, GLFW_KEY_F, 0, GLFW_PRESS, GLFW_MOD_ALT);
    EXPECT_TRUE(m_Toolbar.IsMenuOpen());
    EXPECT_EQ(originalCalls, 0);
    callback(handle, GLFW_KEY_S, 0, GLFW_PRESS, 0);
    EXPECT_EQ(selected, kSaveCommand);
    EXPECT_FALSE(m_Toolbar.IsMenuOpen());
    callback(handle, GLFW_KEY_Q, 0, GLFW_PRESS, 0);
    EXPECT_EQ(originalCalls, 1);
    callback(handle, GLFW_KEY_F10, 0, GLFW_PRESS, 0);
    EXPECT_TRUE(m_Toolbar.IsMenuOpen());
    callback(handle, GLFW_KEY_ESCAPE, 0, GLFW_PRESS, 0);
    EXPECT_FALSE(m_Toolbar.IsMenuOpen());
    m_Toolbar.Uninstall();
    auto restored = glfwSetKeyCallback(handle, nullptr);
    ASSERT_NE(restored, nullptr);
    EXPECT_NE(restored, callback);
    restored(handle, GLFW_KEY_Q, 0, GLFW_PRESS, 0);
    EXPECT_EQ(originalCalls, 2);
    m_Window.Destroy();
    Platform::Window::Terminate();
}
}
