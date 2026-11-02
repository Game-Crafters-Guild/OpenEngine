#include <gtest/gtest.h>

#include "EditorPanelIds.h"
#include "SceneView/MountedViewPanels.h"
#include "UI/Controls/Mount.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <memory>

namespace GameEngine::Editor
{
struct MountedViewPanelsTestAccess
{
    using HeldMount = MountedViewPanels::HeldMount;
};
} // namespace GameEngine::Editor

namespace
{
using namespace GameEngine;
using HeldMount = Editor::MountedViewPanelsTestAccess::HeldMount;

constexpr StringId kSceneViewMountId = HashStringId(EditorPanelIds::MountSceneView);
constexpr StringId kGameViewMountId = HashStringId(EditorPanelIds::MountGameView);

Mount* AddMount(UIElement& parent, const std::string& id)
{
    auto mount = std::make_unique<Mount>();
    mount->SetId(id);
    Mount* raw = mount.get();
    parent.AddChild(std::move(mount));
    return raw;
}

// A held Mount is found again once it is destroyed: a layout reload replaces the dock.
TEST(MountedViewPanelsTests, HeldMountFindsReplacementOfDestroyedMount)
{
    UIManager ui{nullptr};
    ui.SetRoot(std::make_unique<UIElement>());
    UIElement& root = *ui.GetRootElement();
    Mount* first = AddMount(root, EditorPanelIds::MountSceneView);

    HeldMount held;
    EXPECT_EQ(held.Find(&ui, kSceneViewMountId), first);
    EXPECT_EQ(held.Find(&ui, kSceneViewMountId), first);

    root.RemoveChild(first);
    Mount* replacement = AddMount(root, EditorPanelIds::MountSceneView);
    EXPECT_EQ(held.Find(&ui, kSceneViewMountId), replacement);
}

// A held miss ends when a Mount takes the id: a dock tab activation renames the leaf's
// Mount, and a new dock leaf adds one.
TEST(MountedViewPanelsTests, HeldMountMissEndsWhenAMountTakesTheId)
{
    UIManager ui{nullptr};
    ui.SetRoot(std::make_unique<UIElement>());
    UIElement& root = *ui.GetRootElement();
    Mount* leaf = AddMount(root, EditorPanelIds::MountSceneView);

    HeldMount gameView;
    EXPECT_EQ(gameView.Find(&ui, kGameViewMountId), nullptr);
    EXPECT_EQ(gameView.Find(&ui, kGameViewMountId), nullptr);
    leaf->SetId(EditorPanelIds::MountGameView);
    EXPECT_EQ(gameView.Find(&ui, kGameViewMountId), leaf);

    HeldMount sceneView;
    EXPECT_EQ(sceneView.Find(&ui, kSceneViewMountId), nullptr);
    Mount* added = AddMount(root, EditorPanelIds::MountSceneView);
    EXPECT_EQ(sceneView.Find(&ui, kSceneViewMountId), added);
}
} // namespace
