// The custom title bar on a toolbar that draws its menus in the window's title bar. A title bar
// with no UIManager cannot load its layout, so Bind takes the asset-failure path.

#include <gtest/gtest.h>

#include "Platform/Toolbar.h"
#include "Platform/Window.h"
#include "UI/Controls/EditorTitleBar.h"

#include <string>
#include <vector>

namespace
{

// Records each title-bar height that the title bar pushes.
class RecordingToolbar final : public GameEngine::INativeToolbar
{
  public:
    bool Install(GameEngine::Platform::Window*) override { return true; }
    void Uninstall() override {}
    void Clear() override {}
    void SetCommandHandler(CommandCallback) override {}
    uint32_t AddMenu(const std::string&) override { return 0; }
    uint32_t AddSubMenu(uint32_t, const std::string&) override { return 0; }
    void AddItem(uint32_t, const std::string&, uint32_t) override {}
    void AddSeparator(uint32_t) override {}
    GameEngine::ToolbarPresentation GetPresentation() const override
    {
        return GameEngine::ToolbarPresentation::WindowTitleBar;
    }
    void SetTitleBarLayout(int height, std::vector<GameEngine::TitleBarMenuRegion>) override
    {
        Heights.push_back(height);
    }

    std::vector<int> Heights;
};

} // namespace

TEST(EditorTitleBar, BindWithoutItsAssetsRestoresTheSystemCaption)
{
    RecordingToolbar toolbar;
    GameEngine::Platform::Window window;
    GameEngine::EditorTitleBar titleBar;

    titleBar.Bind(&toolbar, &window);

    ASSERT_FALSE(toolbar.Heights.empty());
    EXPECT_EQ(toolbar.Heights.back(), 0);
}
