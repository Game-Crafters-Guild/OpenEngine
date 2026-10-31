#include "Platform/Toolbar.h"
#include "Platform/Window.h"

#include <gtest/gtest.h>

#import <Cocoa/Cocoa.h>

namespace
{
class ToolbarMacTests : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_AutoreleasePool = [[NSAutoreleasePool alloc] init];
        [NSApplication sharedApplication];
        m_PreviousMenu = [[NSApp mainMenu] retain];
        m_PreviousServicesMenu = [[NSApp servicesMenu] retain];
        m_Toolbar = GameEngine::CreateNativeToolbar();
        ASSERT_TRUE(m_Toolbar->Install(&m_Window));
    }
    void TearDown() override
    {
        m_Toolbar->Uninstall();
        m_Toolbar.reset();
        [NSApp setMainMenu:m_PreviousMenu];
        [m_PreviousMenu release];
        [NSApp setServicesMenu:m_PreviousServicesMenu];
        [m_PreviousServicesMenu release];
        [m_AutoreleasePool drain];
    }
    NSMenuItem* QuitItem()
    {
        for (NSMenuItem* item in [[[NSApp mainMenu] itemAtIndex:0] submenu].itemArray)
            if ([[item keyEquivalent] isEqualToString:@"q"])
                return item;
        return nil;
    }
    GameEngine::Platform::Window m_Window;
    std::unique_ptr<GameEngine::INativeToolbar> m_Toolbar;
    NSMenu* m_PreviousMenu = nil;
    NSMenu* m_PreviousServicesMenu = nil;
    NSAutoreleasePool* m_AutoreleasePool = nil;
};

TEST_F(ToolbarMacTests, KeepsFileAfterTheStandardApplicationMenu)
{
    m_Toolbar->AddMenu("File");
    m_Toolbar->AddMenu("Edit");
    EXPECT_EQ(m_Toolbar->GetPresentation(), GameEngine::ToolbarPresentation::NativeMenuBar);
    ASSERT_EQ([[NSApp mainMenu] numberOfItems], 3);
    EXPECT_TRUE([[[[NSApp mainMenu] itemAtIndex:1] title] isEqualToString:@"File"]);
    EXPECT_TRUE([[[[NSApp mainMenu] itemAtIndex:2] title] isEqualToString:@"Edit"]);
    EXPECT_NE(QuitItem(), nil);
}

TEST_F(ToolbarMacTests, ApplicationQuitUsesTheToolbarCommandDispatcher)
{
    constexpr uint32_t kQuitCommand = 0x6000;
    uint32_t received = 0;
    m_Toolbar->SetCommandHandler([&received](uint32_t command) { received = command; });
    ASSERT_TRUE(m_Toolbar->SetApplicationQuitCommand(kQuitCommand));
    NSMenuItem* quit = QuitItem();
    ASSERT_NE(quit, nil);
    EXPECT_EQ([quit keyEquivalentModifierMask], NSEventModifierFlagCommand);
    [NSApp sendAction:[quit action] to:[quit target] from:quit];
    EXPECT_EQ(received, kQuitCommand);
}

TEST_F(ToolbarMacTests, RebuildingTheMenuCanRebindApplicationQuit)
{
    constexpr uint32_t kQuitCommand = 0x6000;
    uint32_t received = 0;
    m_Toolbar->SetCommandHandler([&received](uint32_t command) { received = command; });
    ASSERT_TRUE(m_Toolbar->SetApplicationQuitCommand(kQuitCommand));
    m_Toolbar->Clear();
    ASSERT_TRUE(m_Toolbar->SetApplicationQuitCommand(kQuitCommand));
    NSMenuItem* quit = QuitItem();
    [NSApp sendAction:[quit action] to:[quit target] from:quit];
    EXPECT_EQ(received, kQuitCommand);
}

TEST_F(ToolbarMacTests, UndoAndRedoUseCocoaKeyEquivalents)
{
    constexpr uint32_t kUndoCommand = 0x1001;
    constexpr uint32_t kRedoCommand = 0x1002;
    const auto edit = m_Toolbar->AddMenu("Edit");
    m_Toolbar->AddItem(edit, "Undo\tCmd+Z", kUndoCommand);
    m_Toolbar->AddItem(edit, "Redo\tCmd+Shift+Z", kRedoCommand);
    NSMenu* popup = [[[NSApp mainMenu] itemAtIndex:1] submenu];
    NSMenuItem* undo = [popup itemAtIndex:0];
    NSMenuItem* redo = [popup itemAtIndex:1];
    EXPECT_TRUE([[undo title] isEqualToString:@"Undo"]);
    EXPECT_TRUE([[undo keyEquivalent] isEqualToString:@"z"]);
    EXPECT_EQ([undo keyEquivalentModifierMask], NSEventModifierFlagCommand);
    EXPECT_EQ([redo keyEquivalentModifierMask], NSEventModifierFlagCommand | NSEventModifierFlagShift);
    m_Toolbar->UpdateItemTitle(kUndoCommand, "Undo Move\tCmd+Z");
    EXPECT_TRUE([[undo title] isEqualToString:@"Undo Move"]);
    EXPECT_TRUE([[undo keyEquivalent] isEqualToString:@"z"]);
}

TEST_F(ToolbarMacTests, UninstallRestoresThePreviousMenuAndDetachesQuit)
{
    constexpr uint32_t kQuitCommand = 0x6000;
    bool invoked = false;
    m_Toolbar->SetCommandHandler([&invoked](uint32_t) { invoked = true; });
    ASSERT_TRUE(m_Toolbar->SetApplicationQuitCommand(kQuitCommand));
    NSMenuItem* quit = [QuitItem() retain];
    m_Toolbar->Uninstall();
    EXPECT_EQ([NSApp mainMenu], m_PreviousMenu);
    EXPECT_EQ([NSApp servicesMenu], m_PreviousServicesMenu);
    [NSApp sendAction:[quit action] to:[quit target] from:quit];
    EXPECT_FALSE(invoked);
    [quit release];
}
}
