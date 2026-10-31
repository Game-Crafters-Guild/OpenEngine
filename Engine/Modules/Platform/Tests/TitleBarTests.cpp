#include "Platform/Toolbar.h"
#include "Platform/Window.h"

#include <gtest/gtest.h>

#include <string>

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
        m_Toolbar->SetTitleBarLayout(48, {{1, 32, 12, 80, 36}});
    }

    GameEngine::Platform::Window m_Window;
    HWND m_Handle = nullptr;
    std::unique_ptr<GameEngine::INativeToolbar> m_Toolbar;
};

TEST_F(TitleBarTests, DetachesTheNativeMenuRowAndKeepsMenuIdentity)
{
    ASSERT_NE(GetMenu(m_Handle), nullptr);
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

TEST_F(TitleBarTests, NativeHeadingUnderlinesItsAccessKeyAndKeepsItsDisplayTitle)
{
    const auto file = m_Toolbar->AddMenu("File");
    wchar_t text[64]{};
    ASSERT_GT(GetMenuStringW(GetMenu(m_Handle), 0, text, 64, MF_BYPOSITION), 0);
    EXPECT_EQ(std::wstring(text), L"&File");
    const auto menus = m_Toolbar->GetMenus();
    ASSERT_EQ(menus.size(), 1u);
    EXPECT_EQ(menus[0].Id, file);
    EXPECT_EQ(menus[0].Title, "File");
}

TEST_F(TitleBarTests, FileActionsExposeTheirFirstLetterAsTheAccessKey)
{
    const auto file = m_Toolbar->AddMenu("File");
    const char* titles[] = {"New Scene", "Load Scene", "Save Scene", "Exit"};
    const wchar_t* expected[] = {L"&New Scene", L"&Load Scene", L"&Save Scene", L"&Exit"};
    const HMENU popup = GetSubMenu(GetMenu(m_Handle), 0);
    for (uint32_t index = 0; index < 4; ++index)
    {
        m_Toolbar->AddItem(file, titles[index], 0x4001 + index);
        wchar_t text[64]{};
        ASSERT_GT(GetMenuStringW(popup, index, text, 64, MF_BYPOSITION), 0);
        EXPECT_EQ(std::wstring(text), expected[index]);
    }
}

TEST_F(TitleBarTests, NativeMnemonicEscapesLiteralAmpersands)
{
    const auto tools = m_Toolbar->AddMenu("Tools");
    m_Toolbar->AddSubMenu(tools, "Build & Run");
    const HMENU popup = GetSubMenu(GetMenu(m_Handle), 0);
    wchar_t text[64]{};
    ASSERT_GT(GetMenuStringW(popup, 0, text, 64, MF_BYPOSITION), 0);
    EXPECT_EQ(std::wstring(text), L"&Build && Run");
}

TEST_F(TitleBarTests, RelabelingPreservesTheMnemonicShortcutAndCommand)
{
    constexpr uint32_t kUndoCommand = 0x1001;
    const auto edit = m_Toolbar->AddMenu("Edit");
    m_Toolbar->AddItem(edit, "Undo", kUndoCommand);
    m_Toolbar->UpdateItemTitle(kUndoCommand, "Undo Move & Rotate\tCtrl+Z");
    const HMENU popup = GetSubMenu(GetMenu(m_Handle), 0);
    wchar_t text[64]{};
    ASSERT_GT(GetMenuStringW(popup, 0, text, 64, MF_BYPOSITION), 0);
    EXPECT_EQ(std::wstring(text), L"&Undo Move && Rotate\tCtrl+Z");
    EXPECT_EQ(GetMenuItemID(popup, 0), kUndoCommand);
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
    m_Toolbar->SetTitleBarLayout(48, {{0, 150, 12, 80, 36}});
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

TEST_F(TitleBarTests, UninstallRestoresTheGlfwProcedureAndReleasesDetachedMenus)
{
    const HMENU menu = GetMenu(m_Handle);
    Customize();
    m_Toolbar->Uninstall();
    EXPECT_FALSE(IsMenu(menu));
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

TEST_F(TitleBarTests, DestroyingTheWindowFirstReleasesDetachedMenus)
{
    const HMENU menu = GetMenu(m_Handle);
    Customize();
    m_Window.Destroy();
    m_Toolbar.reset();
    EXPECT_FALSE(IsMenu(menu));
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
        ASSERT_GT(button.Width, 0);
        ASSERT_GT(button.Height, 0);
        EXPECT_GE(button.X, client.right - metrics.CaptionWidth);
        EXPECT_LE(button.X + button.Width, client.right);
        EXPECT_EQ(Hit(button.X + button.Width / 2, button.Y + button.Height / 2), expected[index]);
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
} // namespace
