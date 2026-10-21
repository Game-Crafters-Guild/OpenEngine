// The editor-drawn menu reads an item's icon path as `alias:path` when it names
// a mounted source (`editor:Icons/x.svg`) and as a file otherwise. Panel
// launcher rows hand it absolute files, and on Windows a file starts with a
// drive (`C:\...`): read as alias `C`, it resolved nowhere and every row of the
// Panels menu lost its icon.

#include <gtest/gtest.h>

#include "EditorContextMenu/UIContextMenu.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/StyleProperties.h"
#include "UI/UIStyle.h"

#include <memory>
#include <string>

using namespace GameEngine;

namespace
{

// The registry keys on the pointer alone; Show() resolves the UIManager from it
// and never dereferences the window.
Platform::Window* FakeWindow()
{
    return reinterpret_cast<Platform::Window*>(0x1);
}

const UIElement* FindFirstWithClass(const UIElement& element, const char* className)
{
    if (element.HasClass(className))
        return &element;
    for (const auto& child : element.GetChildren())
    {
        if (const UIElement* found = FindFirstWithClass(*child, className))
            return found;
    }
    return nullptr;
}

// Shows a one-item menu whose icon is `iconPath` and returns the image source
// its icon element carries.
BackgroundImageSource ShownIconSource(const std::string& iconPath)
{
    UIManager manager{nullptr};
    manager.SetRoot(std::make_unique<UIElement>());
    UIContextMenu::Register(FakeWindow(), &manager);
    BackgroundImageSource source{};
    {
        UIContextMenu menu;
        menu.AddItem(0, "Item", 1);
        menu.SetItemIcon(1, iconPath);
        menu.Show(FakeWindow(), 10, 10);
        const UIElement* icon = FindFirstWithClass(*manager.GetRootElement(), "context-menu-item-icon");
        if (icon)
        {
            if (const auto image = icon->Overrides().Get(Style::BackgroundImage))
                source = *image;
        }
    }
    UIContextMenu::Unregister(FakeWindow());
    return source;
}

} // namespace

TEST(ContextMenuItemIconSource, AWindowsDrivePathIsAFileNotAnAlias)
{
    const std::string file = R"(C:\Editor\Packages\ai-assistant\Assets\Editor\Icons\chatbox.svg)";
    const BackgroundImageSource source = ShownIconSource(file);
    EXPECT_EQ(source.Kind, BackgroundImageSource::SourceKind::Path);
    EXPECT_TRUE(source.SourceAlias.empty()) << "the drive letter was read as alias '" << source.SourceAlias << "'";
    EXPECT_EQ(source.Value, file);
}

TEST(ContextMenuItemIconSource, AnAliasPathNamesItsSource)
{
    const BackgroundImageSource source = ShownIconSource("editor:Icons/menulist.svg");
    EXPECT_EQ(source.Kind, BackgroundImageSource::SourceKind::Path);
    EXPECT_EQ(source.SourceAlias, "editor");
    EXPECT_EQ(source.Value, "Icons/menulist.svg");
}
