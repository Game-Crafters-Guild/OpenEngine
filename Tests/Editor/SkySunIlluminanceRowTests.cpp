// The sky inspector's Sun illuminance row with no light linked: the Link action shows only while the
// scene has a sun to link, through the editor's own sheets and the Button control's sheet.

#include <gtest/gtest.h>

#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "InspectorLayoutFixture.h"
#include "InspectorRegistry.h"
#include "Sky/SkySun/SkySunIlluminanceRow.h"
#include "UI/Controls/Button.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <functional>
#include <memory>
#include <vector>

using namespace GameEngine;

namespace
{
Button* FirstButton(UIElement& node)
{
    for (const auto& child : node.GetChildren())
    {
        if (auto* button = dynamic_cast<Button*>(child.get()))
            return button;
        if (Button* found = FirstButton(*child))
            return found;
    }
    return nullptr;
}

// Runs the inspector's refresh callbacks, then one frame, so the next style pass has run.
void Refresh(const std::vector<std::function<void()>>& refreshes, UIManager& ui)
{
    for (const auto& callback : refreshes)
        callback();
    ui.Update(1.0f / 60.0f, /*interactive=*/true);
}
} // namespace

TEST(SkySunIlluminanceRow, TheLinkActionShowsOnlyWhileTheSceneHasASun)
{
    ECS::World world;
    const ECS::EntityHandle sky = world.CreateEntity();
    world.AddComponentImmediate(sky, Components::SkyEnvironment{});

    std::vector<std::function<void()>> refreshes;
    InspectorContext context{};
    context.World = &world;
    context.Entity = sky;
    context.GetWorld = [&world] { return &world; };
    context.SimulationRefreshCallbacks = &refreshes;
    auto root = std::make_unique<UIElement>();
    AddSkySunIlluminanceRow(root.get(), context, {});
    Button* link = FirstButton(*root);
    ASSERT_NE(link, nullptr) << "with no light linked the row offers Link";

    InspectorLayoutTesting::InspectorLayoutFixture fixture;
    fixture.ExtraSheets = {GE_UI_MODULE_SOURCE_DIR "/Assets/UI/controls/Button.css"};
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;
    EXPECT_EQ(link->GetResolvedStyle().Layout.DisplayMode, DisplayMode::None) << "no sun to link";

    const ECS::EntityHandle sun = world.CreateEntity();
    Components::Light directional{};
    directional.Type = Components::LightType::Directional;
    world.AddComponentImmediate(sun, directional);
    Refresh(refreshes, *fixture.Ui);
    EXPECT_NE(link->GetResolvedStyle().Layout.DisplayMode, DisplayMode::None) << "a sun came into the scene";

    world.GetComponentForWrite<Components::Light>(sun)->CastsLight = false;
    Refresh(refreshes, *fixture.Ui);
    EXPECT_EQ(link->GetResolvedStyle().Layout.DisplayMode, DisplayMode::None) << "the sun stopped casting light";
}
