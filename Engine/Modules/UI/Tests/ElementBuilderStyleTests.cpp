#include "FixedScalePlatform.h"
#include "UIRgTestHarness.h"

#include "UI/Controls/Toggle.h"
#include "UI/ElementBuilder.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>

#include <memory>

using namespace GameEngine;

namespace
{
constexpr char kXml[] = R"(<UIElement id="root" class="row">
    <UIElement id="fixed" class="fixed"/>
    <Toggle id="state" class="state" checked="true"/>
    <UIElement id="grow" class="grow"/>
</UIElement>)";

constexpr char kCss[] = R"(
.row { display: flex; flex-direction: row; width: 400px; height: 80px; gap: 7px; padding: 5px; }
.fixed { width: 53px; height: 20px; }
.state { width: 30px; height: 20px; }
.state:checked { width: 67px; }
.grow { flex: 1; height: 25px; }
Toggle { background-color: #123456; }
)";

void ExpectSameLayout(const UIElement& expected, const UIElement& actual)
{
    SCOPED_TRACE(expected.GetId());
    EXPECT_EQ(actual.GetTagId(), expected.GetTagId());
    EXPECT_EQ(actual.GetId(), expected.GetId());
    EXPECT_EQ(actual.GetClasses(), expected.GetClasses());
    EXPECT_EQ(actual.IsPseudoChecked(), expected.IsPseudoChecked());
    EXPECT_EQ(actual.GetResolvedStyle().Visual.BackgroundColor,
              expected.GetResolvedStyle().Visual.BackgroundColor);
    constexpr float kLayoutTolerance = 0.002f;
    EXPECT_NEAR(actual.GetLayoutX(), expected.GetLayoutX(), kLayoutTolerance);
    EXPECT_NEAR(actual.GetLayoutY(), expected.GetLayoutY(), kLayoutTolerance);
    EXPECT_NEAR(actual.GetLayoutWidth(), expected.GetLayoutWidth(), kLayoutTolerance);
    EXPECT_NEAR(actual.GetLayoutHeight(), expected.GetLayoutHeight(), kLayoutTolerance);
    ASSERT_EQ(actual.GetChildren().size(), expected.GetChildren().size());
    for (std::size_t index = 0; index < expected.GetChildren().size(); ++index)
        ExpectSameLayout(*expected.GetChildren()[index], *actual.GetChildren()[index]);
}
}

TEST(ElementBuilderStyle, XmlAndCppShareCascadeAndLayoutAtEveryContentScale)
{
    auto* device = SharedHeadlessDevice();
    ASSERT_NE(device, nullptr) << "A Vulkan device is required for layout parity";
    for (float scale : {1.0f, 1.5f, 2.0f})
    {
        SCOPED_TRACE(scale);
        SharedDeviceReleaseRetirement retirement;
        UITesting::FixedScalePlatform platform(scale);
        UIManager xml(device), cpp(device);
        std::unique_ptr<UIElement> xmlRoot;
        ASSERT_TRUE(UIParsing::XMLParser::ParseLayoutFromString(kXml, xmlRoot));
        xml.SetRoot(std::move(xmlRoot));
        UIElement::WeakRef<Toggle> toggle;
        auto cppRoot = UI::Element<UIElement>().Id("root").Class("row").Children(
            UI::Element<UIElement>().Id("fixed").Class("fixed"),
            UI::Element<Toggle>().Id("state").Class("state").Set(&Toggle::SetChecked, true).Capture(toggle),
            UI::Element<UIElement>().Id("grow").Class("grow")).Build();
        ASSERT_NE(cppRoot, nullptr);
        cpp.SetRoot(std::move(cppRoot));
        Stylesheet sheet;
        ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(kCss, sheet));
        auto handle = std::make_shared<const Stylesheet>(std::move(sheet));
        constexpr float kViewportWidth = 800.0f;
        constexpr float kViewportHeight = 600.0f;
        for (UIManager* manager : {&xml, &cpp})
        {
            manager->SetPlatform(&platform);
            manager->SetLayoutSizeOverride(static_cast<uint32_t>(kViewportWidth * scale),
                                           static_cast<uint32_t>(kViewportHeight * scale));
            manager->AddStylesheet(handle);
            manager->OnMouseMove(-100.0f, -100.0f);
            manager->Update(0.0f, false);
        }
        ASSERT_NE(toggle.Get(), nullptr);
        EXPECT_FLOAT_EQ(toggle.Get()->GetLayoutWidth(), 67.0f);
        EXPECT_EQ(toggle.Get()->GetResolvedStyle().Visual.BackgroundColor, 0xFF123456u);
        ExpectSameLayout(*xml.GetRootElement(), *cpp.GetRootElement());

        auto* xmlToggle = dynamic_cast<Toggle*>(xml.GetRootElement()->FindById("state"_sid));
        ASSERT_NE(xmlToggle, nullptr);
        xmlToggle->SetChecked(false);
        toggle.Get()->SetChecked(false);
        xml.Update(0.0f, false);
        cpp.Update(0.0f, false);
        EXPECT_FLOAT_EQ(toggle.Get()->GetLayoutWidth(), 30.0f);
        ExpectSameLayout(*xml.GetRootElement(), *cpp.GetRootElement());
    }
}
