// The Mark-up tool strip entry's right-click menu: every shape row carries an icon, and each icon
// names artwork that exists in the editor's asset source. An empty IconPath shows a bare row and
// a path to a missing file only warns at load, so neither fails loudly at runtime.

#include "Markups/MarkupTool.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>
#include <string_view>

using namespace GameEngine;
using namespace GameEngine::Editor;

namespace
{
constexpr std::string_view kEditorMount = "editor:";
} // namespace

TEST(MarkupToolMenuTests, EveryShapeRowHasADistinctResolvableIcon)
{
    const std::vector<ContextMenuManipulator::Item> items = BuildMarkupToolMenuItems({});
    ASSERT_EQ(items.size(), 3u);

    std::set<std::string> icons;
    for (const ContextMenuManipulator::Item& item : items)
    {
        ASSERT_TRUE(item.IconPath.starts_with(kEditorMount))
            << "'" << item.Path << "' has no editor icon (IconPath '" << item.IconPath << "')";
        // "editor:<path>" resolves against Apps/Editor/Assets in the tree.
        const std::filesystem::path artwork = std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets" /
                                              item.IconPath.substr(kEditorMount.size());
        EXPECT_TRUE(std::filesystem::exists(artwork))
            << "'" << item.Path << "' names " << item.IconPath << ", which does not exist";
        icons.insert(item.IconPath);
    }
    EXPECT_EQ(icons.size(), items.size()) << "two shape rows share one icon";
}
