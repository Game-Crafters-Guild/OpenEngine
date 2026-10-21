#include "CssInspector/CssInspector.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>
#include <memory>

using namespace GameEngine;

namespace
{

void InstallOverlays(UIManager& manager)
{
    auto root = std::make_unique<UIElement>();
    for (const char* id : {"css-inspector-overlay", "css-inspector-highlight"})
    {
        auto element = std::make_unique<UIElement>();
        element->SetId(id);
        element->Overrides().Set(Style::Display, DisplayMode::Block);
        root->AddChild(std::move(element));
    }
    manager.SetRoot(std::move(root));
}

void SetOverlayDisplay(UIManager& manager, DisplayMode display)
{
    for (const char* id : {"css-inspector-overlay", "css-inspector-highlight"})
        manager.GetRootElement()->FindById(HashStringId(id))->Overrides().Set(Style::Display, display);
}

void ExpectOverlayDisplay(UIManager& manager, DisplayMode display)
{
    for (const char* id : {"css-inspector-overlay", "css-inspector-highlight"})
        EXPECT_EQ(manager.GetRootElement()->FindById(HashStringId(id))->Overrides().Get(Style::Display), display);
}

} // namespace

TEST(CssInspectorIdle, DisabledUpdatesLeaveTheTreeUntouched)
{
    UIManager manager(nullptr);
    InstallOverlays(manager);
    CssInspector inspector;
    inspector.Update(&manager, 800, 600);
    ExpectOverlayDisplay(manager, DisplayMode::Block);
}

TEST(CssInspectorIdle, DisablingHidesEveryVisitedWindowOnce)
{
    UIManager first(nullptr);
    UIManager second(nullptr);
    InstallOverlays(first);
    InstallOverlays(second);
    CssInspector inspector;
    inspector.SetEnabled(true);
    inspector.Update(&first, 800, 600);
    inspector.Update(&second, 800, 600);
    SetOverlayDisplay(first, DisplayMode::Block);
    SetOverlayDisplay(second, DisplayMode::Block);

    inspector.SetEnabled(false);
    ExpectOverlayDisplay(first, DisplayMode::None);
    ExpectOverlayDisplay(second, DisplayMode::None);
    SetOverlayDisplay(first, DisplayMode::Block);
    inspector.Update(&first, 800, 600);
    ExpectOverlayDisplay(first, DisplayMode::Block);
}

TEST(CssInspectorIdle, AClosedWindowDoesNotLeaveADanglingDisableTarget)
{
    CssInspector inspector;
    inspector.SetEnabled(true);
    {
        UIManager manager(nullptr);
        InstallOverlays(manager);
        inspector.Update(&manager, 800, 600);
    }
    inspector.ToggleEnabled();
    EXPECT_FALSE(inspector.IsEnabled());
}
