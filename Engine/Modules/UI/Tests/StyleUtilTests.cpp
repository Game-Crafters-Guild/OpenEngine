#include <gtest/gtest.h>
#include <string>
#include "UI/StyleUtil.h"
#include "UI/UIElement.h"

using namespace GameEngine;
using namespace GameEngine::UIUtil;

TEST(StyleUtilTests, NormalizesAssetSchemeOnly)
{
    std::string a = NormalizeCssUrlPath("asset:textures/a.png");
    EXPECT_EQ(a, std::string("textures/a.png"));

    std::string b = NormalizeCssUrlPath("textures/a.png");
    EXPECT_EQ(b, std::string("textures/a.png"));
}

TEST(StyleUtilTests, DoesNotStripLeadingSlash)
{
    std::string p = NormalizeCssUrlPath("/textures/a.png");
    EXPECT_EQ(p, std::string("/textures/a.png"));
}

TEST(StyleUtilTests, SetShownTogglesTheHiddenClass)
{
    struct Step
    {
        bool StartsHidden;
        bool Shown;
        bool EndsHidden;
    };
    // Hide a shown element, hide it again, show a hidden one, show a shown one.
    constexpr Step kSteps[] = {{false, false, true}, {true, false, true}, {true, true, false}, {false, true, false}};
    for (const Step& step : kSteps)
    {
        UIElement element;
        element.AddClass("notice");
        if (step.StartsHidden)
            element.AddClass("hidden");
        SetShown(element, step.Shown);
        EXPECT_EQ(element.HasClass("hidden"), step.EndsHidden);
        EXPECT_TRUE(element.HasClass("notice")) << "SetShown keeps the element's other classes";
        EXPECT_EQ(element.GetClasses().size(), step.EndsHidden ? 2u : 1u) << "the class is held once";
    }
}
