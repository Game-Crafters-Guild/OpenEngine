#include "Platform/Toolbar.h"
#include "Platform/Window.h"
#include "RecordingPopup.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <windows.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace
{
class TitleBarTests : public testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_TRUE(GameEngine::Platform::Window::Initialize());
        GameEngine::Platform::WindowDesc desc;
        desc.Title = "Title bar regression (hidden)";
        desc.Width = 800;
        desc.Height = 500;
        desc.StartHidden = true;
        ASSERT_TRUE(m_Window.Create(desc));
        m_Handle = glfwGetWin32Window(m_Window.GetGLFWHandle());
        ASSERT_NE(m_Handle, nullptr);
        m_Window.GetPosition(m_CreatedX, m_CreatedY);
        m_Window.GetFramebufferSize(m_ClientWidth, m_ClientHeight);
        m_Window.SetFramebufferSizeHandler([this](int width, int height) { RecordClientSize(width, height); });
        m_Toolbar = GameEngine::CreateNativeToolbar();
        ASSERT_TRUE(m_Toolbar->Install(&m_Window));
    }

    void TearDown() override
    {
        if (m_Toolbar)
            m_Toolbar->Uninstall();
        m_Toolbar.reset();
        m_Window.Destroy();
        GameEngine::Platform::Window::Terminate();
    }

    LRESULT Hit(int x, int y)
    {
        POINT point{x, y};
        ClientToScreen(m_Handle, &point);
        return SendMessageW(m_Handle, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
    }

    void Customize()
    {
        m_Toolbar->SetTitleBarLayout(48, {{1, {32, 12, 80, 36}}});
    }

    void RecordClientSize(int width, int height)
    {
        if (width == m_ClientWidth && height == m_ClientHeight)
            return;
        m_ClientWidth = width;
        m_ClientHeight = height;
        ++m_ClientSizeChanges;
    }

    static DWORD UserObjectCount() { return GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS); }

    GameEngine::Platform::Window m_Window;
    HWND m_Handle = nullptr;
    int m_CreatedX = 0;
    int m_CreatedY = 0;
    int m_ClientWidth = 0;
    int m_ClientHeight = 0;
    // Client size changes the window reported since the fixture created it.
    int m_ClientSizeChanges = 0;
    std::unique_ptr<GameEngine::INativeToolbar> m_Toolbar;
};

TEST_F(TitleBarTests, NeverAttachesANativeMenuRowAndKeepsMenuIdentity)
{
    EXPECT_EQ(GetMenu(m_Handle), nullptr);
    const auto edit = m_Toolbar->AddMenu("Edit");
    m_Toolbar->AddItem(edit, "Undo", 0x1001);
    const auto window = m_Toolbar->AddMenu("Window");
    m_Toolbar->AddSubMenu(window, "Layout");
    Customize();
    EXPECT_EQ(GetMenu(m_Handle), nullptr);
    const auto menus = m_Toolbar->GetMenus();
    ASSERT_EQ(menus.size(), 2u);
    EXPECT_EQ(menus[0].Id, edit);
    EXPECT_EQ(menus[0].Title, "Edit");
    EXPECT_EQ(menus[1].Id, window);
    EXPECT_EQ(menus[1].Title, "Window");
}

TEST_F(TitleBarTests, HeadingOpensItsMenuThroughTheMenuFactoryUnderTheHeading)
{
    auto record = std::make_shared<GameEngine::Tests::PopupRecord>();
    m_Toolbar->SetMenuFactory([record] { return std::make_unique<GameEngine::Tests::RecordingPopup>(record); });
    const auto file = m_Toolbar->AddMenu("File");
    m_Toolbar->AddItem(file, "Save Scene\tCtrl+S", 0x4001);
    m_Toolbar->AddSubMenu(file, "Build & Run");
    std::vector<uint32_t> commands;
    m_Toolbar->SetCommandHandler([&commands](uint32_t command) { commands.push_back(command); });
    m_Toolbar->SetTitleBarLayout(48, {{file, {32, 12, 80, 36}}});

    m_Toolbar->ShowTitleBarMenu(file);
    ASSERT_EQ(record->Shows, 1);
    ASSERT_EQ(record->Rows.size(), 2u);
    EXPECT_EQ(record->Rows[0].Title, "Save Scene");
    EXPECT_EQ(record->Rows[0].Command, 0x4001u);
    EXPECT_EQ(record->Shortcut, "Ctrl+S");
    EXPECT_EQ(record->Rows[1].Title, "Build & Run");
    EXPECT_NE(record->Rows[1].Submenu, 0u);
    const float scale = m_Window.GetUiContentScale() > 0.0f ? m_Window.GetUiContentScale() : 1.0f;
    EXPECT_EQ(record->Window, &m_Window);
    EXPECT_EQ(record->X, static_cast<int>(std::lround(32 / scale)));
    EXPECT_EQ(record->Y, static_cast<int>(std::lround(48 / scale)));
    EXPECT_EQ(m_Toolbar->GetOpenMenu(), file);

    record->Command(0x4001);
    EXPECT_EQ(commands, std::vector<uint32_t>{0x4001});
    record->Closed();
    EXPECT_FALSE(m_Toolbar->GetOpenMenu());
}

TEST_F(TitleBarTests, ASeparatorReachesThePopupBetweenTheItemsAroundIt)
{
    auto record = std::make_shared<GameEngine::Tests::PopupRecord>();
    m_Toolbar->SetMenuFactory([record] { return std::make_unique<GameEngine::Tests::RecordingPopup>(record); });
    const auto file = m_Toolbar->AddMenu("File");
    m_Toolbar->AddItem(file, "Save Scene", 0x4001);
    m_Toolbar->AddSeparator(file);
    m_Toolbar->AddItem(file, "Exit", 0x4002);
    m_Toolbar->SetTitleBarLayout(48, {{file, {32, 12, 80, 36}}});

    m_Toolbar->ShowTitleBarMenu(file);
    ASSERT_EQ(record->Rows.size(), 3u);
    EXPECT_EQ(record->Rows[0].Command, 0x4001u);
    EXPECT_TRUE(record->Rows[1].Separator);
    EXPECT_EQ(record->Rows[2].Command, 0x4002u);
}

TEST_F(TitleBarTests, OpenMenuNamesTheOverflowWhenItsHeadingIsHidden)
{
    auto record = std::make_shared<GameEngine::Tests::PopupRecord>();
    m_Toolbar->SetMenuFactory([record] { return std::make_unique<GameEngine::Tests::RecordingPopup>(record); });
    const auto file = m_Toolbar->AddMenu("File");
    m_Toolbar->AddItem(file, "Save Scene", 0x4001);
    const auto edit = m_Toolbar->AddMenu("Edit");
    m_Toolbar->AddItem(edit, "Undo", 0x4002);
    m_Toolbar->SetTitleBarLayout(48, {{file, {32, 12, 80, 36}}});

    m_Toolbar->ShowTitleBarMenu(file);
    EXPECT_EQ(m_Toolbar->GetOpenMenu(), file);
    m_Toolbar->SetTitleBarLayout(48, {{0, {32, 12, 40, 36}}});
    m_Toolbar->ShowTitleBarMenu(edit);
    EXPECT_EQ(m_Toolbar->GetOpenMenu(), 0u);
    ASSERT_TRUE(record->Closed);
    record->Closed();
    EXPECT_FALSE(m_Toolbar->GetOpenMenu());
}

TEST_F(TitleBarTests, RelabelingReachesThePopupWithItsShortcutAndCommand)
{
    constexpr uint32_t kUndoCommand = 0x1001;
    auto record = std::make_shared<GameEngine::Tests::PopupRecord>();
    m_Toolbar->SetMenuFactory([record] { return std::make_unique<GameEngine::Tests::RecordingPopup>(record); });
    const auto edit = m_Toolbar->AddMenu("Edit");
    m_Toolbar->AddItem(edit, "Undo", kUndoCommand);
    m_Toolbar->UpdateItemTitle(kUndoCommand, "Undo Move & Rotate\tCtrl+Z");
    m_Toolbar->SetTitleBarLayout(48, {{edit, {32, 12, 80, 36}}});
    m_Toolbar->ShowTitleBarMenu(edit);
    ASSERT_EQ(record->Rows.size(), 1u);
    EXPECT_EQ(record->Rows[0].Title, "Undo Move & Rotate");
    EXPECT_EQ(record->Rows[0].Command, kUndoCommand);
    EXPECT_EQ(record->Shortcut, "Ctrl+Z");
}

TEST_F(TitleBarTests, OnlyMenuHeadingsReceiveClientInputInTheCaption)
{
    Customize();
    EXPECT_EQ(Hit(60, 24), HTCLIENT);
    EXPECT_EQ(Hit(180, 24), HTCAPTION);
    EXPECT_EQ(Hit(180, 70), HTCLIENT);
    EXPECT_EQ(Hit(180, 0), HTTOP);
}

TEST_F(TitleBarTests, MovingAHeadingReplacesItsHitRegion)
{
    Customize();
    m_Toolbar->SetTitleBarLayout(48, {{0, {150, 12, 80, 36}}});
    EXPECT_EQ(Hit(60, 24), HTCAPTION);
    EXPECT_EQ(Hit(180, 24), HTCLIENT);
}

TEST_F(TitleBarTests, MenuRefreshKeepsClientSizeAndPublishesANewInventory)
{
    m_Toolbar->AddMenu("Edit");
    Customize();
    RECT before{};
    GetClientRect(m_Handle, &before);
    const uint64_t revision = m_Toolbar->GetMenuRevision();
    m_Toolbar->Clear();
    m_Toolbar->AddMenu("Tools");
    RECT after{};
    GetClientRect(m_Handle, &after);
    EXPECT_EQ(after.right, before.right);
    EXPECT_EQ(after.bottom, before.bottom);
    EXPECT_GT(m_Toolbar->GetMenuRevision(), revision);
    const auto menus = m_Toolbar->GetMenus();
    ASSERT_EQ(menus.size(), 1u);
    EXPECT_EQ(menus[0].Title, "Tools");
    EXPECT_EQ(GetMenu(m_Handle), nullptr);
}

TEST_F(TitleBarTests, InstallMenusAndFrameNeverResizeTheClientOfARestoredWindow)
{
    const auto edit = m_Toolbar->AddMenu("Edit");
    m_Toolbar->AddItem(edit, "Undo", 0x1001);
    m_Toolbar->Clear();
    m_Toolbar->AddMenu("Tools");
    Customize();
    m_Toolbar->SetTitleBarLayout(0, {});
    Customize();
    EXPECT_EQ(m_ClientSizeChanges, 0);
    int width = 0;
    int height = 0;
    m_Window.GetWindowSize(width, height);
    EXPECT_EQ(width, 800);
    EXPECT_EQ(height, 500);
}

TEST_F(TitleBarTests, CustomFrameKeepsTheClientRectTheWindowWasCreatedWith)
{
    Customize();
    int width = 0;
    int height = 0;
    m_Window.GetWindowSize(width, height);
    EXPECT_EQ(width, 800);
    EXPECT_EQ(height, 500);
    int x = 0;
    int y = 0;
    m_Window.GetPosition(x, y);
    EXPECT_EQ(x, m_CreatedX);
    EXPECT_EQ(y, m_CreatedY);
}

TEST_F(TitleBarTests, SizeAndPositionRequestsUnderTheCustomFrameSetTheClientRect)
{
    Customize();
    m_Window.SetWindowSize(1920, 1080);
    m_Window.SetPosition(40, 40);
    int width = 0;
    int height = 0;
    m_Window.GetWindowSize(width, height);
    EXPECT_EQ(width, 1920);
    EXPECT_EQ(height, 1080);
    int x = 0;
    int y = 0;
    m_Window.GetPosition(x, y);
    EXPECT_EQ(x, 40);
    EXPECT_EQ(y, 40);
}

TEST_F(TitleBarTests, UninstallRestoresTheGlfwProcedureAndReleasesTheMenus)
{
    m_Toolbar->AddSubMenu(m_Toolbar->AddMenu("Window"), "Layout");
    Customize();
    const DWORD withMenus = UserObjectCount();
    m_Toolbar->Uninstall();
    // The menu bar, "Window" and "Layout".
    EXPECT_EQ(UserObjectCount(), withMenus - 3);
    EXPECT_EQ(Hit(180, 24), HTCLIENT);
    EXPECT_FALSE(m_Window.ShouldClose());
    SendMessageW(m_Handle, WM_CLOSE, 0, 0);
    EXPECT_TRUE(m_Window.ShouldClose());
}

TEST_F(TitleBarTests, NativeCloseStillHonorsTheUnsavedChangesHandler)
{
    Customize();
    bool requested = false;
    m_Window.SetCloseRequestedHandler([&requested]() { requested = true; return true; });
    SendMessageW(m_Handle, WM_CLOSE, 0, 0);
    EXPECT_TRUE(requested);
    EXPECT_FALSE(m_Window.ShouldClose());
}

TEST_F(TitleBarTests, DestroyingTheWindowFirstReleasesTheMenus)
{
    m_Toolbar->AddSubMenu(m_Toolbar->AddMenu("Window"), "Layout");
    Customize();
    m_Window.Destroy();
    const DWORD withMenus = UserObjectCount();
    m_Toolbar.reset();
    // The menu bar, "Window" and "Layout".
    EXPECT_EQ(UserObjectCount(), withMenus - 3);
}

TEST_F(TitleBarTests, SuspendingTheCustomFrameDoesNotReattachTheMenuInFullscreen)
{
    Customize();
    m_Toolbar->SetTitleBarLayout(0, {});
    EXPECT_EQ(GetMenu(m_Handle), nullptr);
    EXPECT_EQ(Hit(180, 24), HTCLIENT);
    Customize();
    EXPECT_EQ(Hit(180, 24), HTCAPTION);
    const auto metrics = m_Toolbar->GetTitleBarMetrics();
    EXPECT_GT(metrics.CaptionWidth, 0);
    EXPECT_GT(metrics.Height, 0);
}

TEST_F(TitleBarTests, MinimumWindowWidthReservesCaptionButtonsAndMenuAccess)
{
    Customize();
    MINMAXINFO info{};
    SendMessageW(m_Handle, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&info));
    EXPECT_GE(info.ptMinTrackSize.x, m_Toolbar->GetTitleBarMetrics().CaptionWidth + 160);
}

TEST_F(TitleBarTests, CaptionPaintingBoundsMatchNativeButtonHitTargets)
{
    Customize();
    const auto metrics = m_Toolbar->GetTitleBarMetrics();
    RECT client{};
    GetClientRect(m_Handle, &client);
    const LRESULT expected[] = {HTMINBUTTON, HTMAXBUTTON, HTCLOSE};
    for (size_t index = 0; index < metrics.Buttons.size(); ++index)
    {
        const auto& button = metrics.Buttons[index];
        ASSERT_GT(button.Bounds.Width, 0);
        ASSERT_GT(button.Bounds.Height, 0);
        EXPECT_GE(button.Bounds.X, client.right - metrics.CaptionWidth);
        EXPECT_LE(button.Bounds.Right(), client.right);
        EXPECT_EQ(Hit(button.Bounds.X + button.Bounds.Width / 2, button.Bounds.Y + button.Bounds.Height / 2), expected[index]);
    }
}

TEST_F(TitleBarTests, NativeHoverAndLeavePublishCaptionPaintState)
{
    Customize();
    SendMessageW(m_Handle, WM_NCMOUSEMOVE, HTMAXBUTTON, 0);
    EXPECT_EQ(m_Toolbar->GetTitleBarMetrics().HoveredButton, GameEngine::TitleBarButton::Maximize);
    SendMessageW(m_Handle, WM_NCMOUSEMOVE, HTCLOSE, 0);
    EXPECT_EQ(m_Toolbar->GetTitleBarMetrics().HoveredButton, GameEngine::TitleBarButton::Close);
    SendMessageW(m_Handle, WM_NCMOUSELEAVE, 0, 0);
    EXPECT_EQ(m_Toolbar->GetTitleBarMetrics().HoveredButton, GameEngine::TitleBarButton::None);
}

TEST_F(TitleBarTests, CustomFrameKeepsTheOuterRectOfAMaximizedWindow)
{
    // IsZoomed reads WS_MAXIMIZE, so the hidden window counts as maximized.
    SetWindowLongPtrW(m_Handle, GWL_STYLE, GetWindowLongPtrW(m_Handle, GWL_STYLE) | WS_MAXIMIZE);
    SetWindowPos(m_Handle, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ASSERT_NE(IsZoomed(m_Handle), FALSE);
    RECT before{};
    GetWindowRect(m_Handle, &before);
    Customize();
    RECT after{};
    GetWindowRect(m_Handle, &after);
    EXPECT_EQ(after.left, before.left);
    EXPECT_EQ(after.top, before.top);
    EXPECT_EQ(after.right, before.right);
    EXPECT_EQ(after.bottom, before.bottom);
}

TEST_F(TitleBarTests, InstallGivesAMaximizedWindowItsFinalClientBeforeTheFirstLayout)
{
    // The renderer reads the client size after Install and before the first title bar layout.
    m_Toolbar->Uninstall();
    SetWindowLongPtrW(m_Handle, GWL_STYLE, GetWindowLongPtrW(m_Handle, GWL_STYLE) | WS_MAXIMIZE);
    SetWindowPos(m_Handle, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ASSERT_NE(IsZoomed(m_Handle), FALSE);
    RECT systemFrame{};
    GetClientRect(m_Handle, &systemFrame);
    ASSERT_TRUE(m_Toolbar->Install(&m_Window));
    RECT installed{};
    GetClientRect(m_Handle, &installed);
    EXPECT_GT(installed.bottom, systemFrame.bottom);
    m_ClientSizeChanges = 0;
    Customize();
    RECT laidOut{};
    GetClientRect(m_Handle, &laidOut);
    EXPECT_EQ(m_ClientSizeChanges, 0);
    EXPECT_EQ(laidOut.right, installed.right);
    EXPECT_EQ(laidOut.bottom, installed.bottom);
}

TEST_F(TitleBarTests, SuspendingTheCustomFrameKeepsTheFullscreenRect)
{
    // GLFW's fullscreen gives the window WS_POPUP and the monitor rect.
    Customize();
    const LONG_PTR style = GetWindowLongPtrW(m_Handle, GWL_STYLE);
    SetWindowLongPtrW(m_Handle, GWL_STYLE, (style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP);
    SetWindowPos(m_Handle, nullptr, 0, 0, 1280, 720, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RECT before{};
    GetWindowRect(m_Handle, &before);
    m_Toolbar->SetTitleBarLayout(0, {});
    RECT after{};
    GetWindowRect(m_Handle, &after);
    RECT client{};
    GetClientRect(m_Handle, &client);
    EXPECT_EQ(after.left, before.left);
    EXPECT_EQ(after.top, before.top);
    EXPECT_EQ(after.right, before.right);
    EXPECT_EQ(after.bottom, before.bottom);
    EXPECT_EQ(client.right, 1280);
    EXPECT_EQ(client.bottom, 720);
}
} // namespace
