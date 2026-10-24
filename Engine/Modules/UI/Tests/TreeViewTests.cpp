#include <gtest/gtest.h>

#include "UI/Controls/TreeView.h"
#include "UI/Interaction/Payload.h"
#include "UI/Interaction/Selection.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"
#include "Platform/SystemMetrics.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace {

class TestTreeProvider : public TreeChangeTrackingProvider {
public:
    int GetRootCount() const override { return 1; }

    TreeId GetRootId(int index) const override {
        (void)index;
        return 1; // single root
    }

    int GetChildCount(TreeId parent) const override {
        if (parent == 1) return 2;      // children: 2, 3
        if (parent == 2) return 1;      // child: 4
        return 0;                       // leaves
    }

    TreeId GetChildId(TreeId parent, int index) const override {
        if (parent == 1) {
            return (index == 0) ? 2 : 3;
        }
        if (parent == 2) {
            return 4;
        }
        return 0;
    }

    const char* GetLabel(TreeId id) const override {
        switch (id) {
        case 1: return "root";
        case 2: return "child-a";
        case 3: return "child-b";
        case 4: return "grandchild";
        default: return "";
        }
    }

    bool IsExpandable(TreeId id) const override {
        // Only nodes with children are expandable
        return id == 1 || id == 2;
    }
};

} // namespace

TEST(TreeViewTests, ExpandAllExpandsAllExpandableNodes)
{
    TestTreeProvider provider;
    TreeView view;
    view.SetDataProvider(&provider);
    view.SetShowRoot(true);

    // Initially nothing is expanded
    EXPECT_FALSE(view.IsExpanded(1));
    EXPECT_FALSE(view.IsExpanded(2));
    EXPECT_FALSE(view.IsExpanded(3));
    EXPECT_FALSE(view.IsExpanded(4));

    view.ExpandAll();

    // All expandable nodes (those with children) should now be expanded
    EXPECT_TRUE(view.IsExpanded(1));
    EXPECT_TRUE(view.IsExpanded(2));

    // Leaf nodes should remain non-expandable/non-expanded
    EXPECT_FALSE(view.IsExpanded(3));
    EXPECT_FALSE(view.IsExpanded(4));
}

// The Hierarchy and the Assets folder tree rename with the file-manager gesture. Their trees drag
// rows, so the press captures the mouse and the release reaches the tree, not the row. A slow
// second click on the sole selected row sends the selection, then asks for a rename; the click
// that selects the row does not ask for a rename.
TEST(TreeViewTests, ASlowSecondClickOnTheSoleSelectionRequestsRename)
{
    SharedDeviceReleaseRetirement releaseRetirement;
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    UiRgHarness harness(device);

    TestTreeProvider provider;
    UI::Interaction::SelectionModel selection;
    auto root = std::make_unique<UIElement>();
    auto owner = std::make_unique<TreeView>();
    TreeView* tree = owner.get();
    tree->SetDataProvider(&provider);
    tree->SetShowRoot(true);
    tree->SetSelectionModel(&selection);
    tree->SetSelectOnMouseUp(true);
    tree->SetDragPayloadBuilder([](TreeId) { return UI::Interaction::DragPayload{}; });
    tree->Overrides().Set(Style::Width, StyleLength::Px(240.0f)).Set(Style::Height, StyleLength::Px(200.0f));
    // The callbacks in the order they run: 'S' for a selection sent, 'R' for a rename request.
    std::string calls;
    std::vector<TreeId> renameRequests;
    tree->SetOnItemRenameRequested([&](TreeId id) { renameRequests.push_back(id); calls += 'R'; });
    tree->SetOnSelectionChanged([&](TreeId) { calls += 'S'; });
    root->AddChild(std::move(owner));
    UIManager ui(device);
    ui.SetLayoutSizeOverride(300, 300);
    ui.SetRoot(std::move(root));
    tree->RefreshFromProvider();
    auto frame = [&]()
    {
        ui.Update(1.0f / 60.0f, /*interactive=*/true);
        DriveUiRender(ui, harness);
    };
    for (int i = 0; i < 3; ++i)
        frame();

    const UIElement* row = tree->FindBoundRow(1);
    ASSERT_NE(row, nullptr) << "the root row is not bound";
    const float x = row->GetLayoutX() + row->GetLayoutWidth() * 0.5f;
    const float y = row->GetLayoutY() + row->GetLayoutHeight() * 0.5f;
    auto click = [&]()
    {
        ui.OnMouseMove(x, y);
        frame();
        ui.OnMouseButton(0, true);
        frame();
        ui.OnMouseButton(0, false);
        frame();
        frame();
    };

    click();
    ASSERT_TRUE(selection.IsSelected(1)) << "the first click did not select the row; the test is vacuous";
    EXPECT_TRUE(renameRequests.empty()) << "the selecting click is not a rename request";
    calls.clear();

    std::this_thread::sleep_for(Platform::GetDoubleClickInterval() + std::chrono::milliseconds(50));
    click();
    EXPECT_EQ(renameRequests, std::vector<TreeId>{1});
    // A host showing another item (the Inspector on an asset) returns to this one on the click.
    EXPECT_EQ(calls, "SR") << "the rename click must send the selection, then request the rename";
}
