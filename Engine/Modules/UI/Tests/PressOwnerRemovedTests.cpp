// A press whose handler requested capture and then had its control destroyed by the
// deferred work it posted (an inspector rebuild after a dropdown commit) still owns the
// rest of its gesture: the release must not land on, or focus, whatever now lies under
// the pointer.

#include "IsolatedUIFixture.h"

#include "UI/Controls/Dropdown.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace GameEngine;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="host"/>
  <textfield id="field"/>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#host { width: 160px; height: 20px; }
#field { width: 160px; height: 60px; }
.dropdown-items { position: absolute; top: 100%; left: 0px; width: 100%; z-index: 1000; }
.dropdown-item { height: 20px; }
)";

} // namespace

// The NavigationSurface inspector's Mode dropdown, reduced: its open list lies over the
// next row's field, and committing a value rebuilds the section, destroying the dropdown.
TEST(PressOwnerRemoved, ReleaseOfAPressWhoseCapturerWasRebuiltAwayReachesNothingUnderThePointer)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kXml, kCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();

    UIElement* host = fixture.Element("host");
    UIElement* field = fixture.Element("field");
    ASSERT_NE(host, nullptr);
    ASSERT_NE(field, nullptr);
    int fieldMouseUps = 0;
    field->RegisterEventHandler(kEventMouseUp, [&fieldMouseUps](UIEvent&) { ++fieldMouseUps; });

    auto owned = std::make_unique<Dropdown>();
    Dropdown* dropdown = owned.get();
    host->AddChild(std::move(owned));
    dropdown->SetOptionsFromString("One,Two,Three");
    int changes = 0;
    dropdown->SetOnValueChanged([&changes, host, dropdown](const std::string&) {
        ++changes;
        dropdown->PostAction([host, dropdown] { host->RemoveChild(dropdown); });
    });
    fixture.Settle();
    dropdown->OpenMenuUi();
    fixture.Settle();

    const UIElement* second = dropdown->GetItemsContainer()->GetChildren()[1].get();
    const float x = second->GetLayoutX() + second->GetLayoutWidth() * 0.5f;
    const float y = second->GetLayoutY() + second->GetLayoutHeight() * 0.5f;
    ASSERT_GT(y, field->GetLayoutY()) << "the item lies over the field";
    ASSERT_LT(y, field->GetLayoutY() + field->GetLayoutHeight()) << "the item lies over the field";

    fixture.Manager().OnMouseMove(x, y);
    fixture.StepFrame();
    fixture.Manager().OnMouseButton(0, true);
    fixture.StepFrame();
    ASSERT_TRUE(host->GetChildren().empty()) << "the commit's rebuild destroyed the dropdown";
    // The pointer drifts before the release, so the release hit-tests rather than reading
    // the hover the last frame resolved.
    fixture.Manager().OnMouseMove(x + 1.0f, y);
    fixture.Manager().OnMouseButton(0, false);
    fixture.StepFrame();

    EXPECT_EQ(changes, 1);
    EXPECT_EQ(fieldMouseUps, 0) << "the release landed on the field the list had covered";
    EXPECT_FALSE(field->IsFocusTargetForId(fixture.Manager().GetFocusedElementId()))
        << "the release focused the field";
}
