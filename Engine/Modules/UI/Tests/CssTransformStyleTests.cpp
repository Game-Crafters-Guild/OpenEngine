// `transform` and `transform-origin`: parsing, the cascade, overrides,
// interpolation and the matrix ComputeElementTransform builds. What the
// renderer and hit-testing do with it is CssTransformRenderTests.cpp.

#include <gtest/gtest.h>

#include <string>

#include "UI/Parsers/CSSParser.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UITransform.h"
#include "Mathematics/MatrixOps.h"

#include "../Source/StyleApplier.h"
#include "../Source/StyleInterpolation.h"

using namespace GameEngine;
using namespace GameEngine::UIParsing;

namespace
{

ResolvedStyle ComputeFor(const std::string& css, const std::string& id = "t")
{
    Stylesheet sheet{};
    EXPECT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    ElementState st{};
    UIElement el;
    el.SetId(id);
    return CSSParser::ComputeStyleFor(el, sheet, st);
}

void ExpectPoint(const Mathematics::Matrix3x3& m, float x, float y, float ex, float ey)
{
    const Mathematics::Vector2 p = m.TransformPoint(Mathematics::Vector2(x, y));
    EXPECT_NEAR(p.x, ex, 1e-3f);
    EXPECT_NEAR(p.y, ey, 1e-3f);
}

Mathematics::Rect Box(float x, float y, float w, float h)
{
    return Mathematics::Rect{x, y, w, h};
}

} // namespace

TEST(CssTransformStyle, NoneIsTheInitialValue)
{
    const ResolvedStyle s = ComputeFor("#t { opacity: 1; }");
    EXPECT_TRUE(s.Visual.Transform.IsNone());
    EXPECT_EQ(s.Visual.TransformOrigin.X, StyleLength::Percent(50.0f));
    EXPECT_EQ(s.Visual.TransformOrigin.Y, StyleLength::Percent(50.0f));
}

TEST(CssTransformStyle, ParsesAFunctionListInOrder)
{
    const ResolvedStyle s =
        ComputeFor("#t { transform: translate(10px, -50%) rotate(0.25turn) scale(2, 50%) skewX(10deg); }");
    const TransformValue& tv = s.Visual.Transform;
    ASSERT_EQ(tv.Ops().size(), 4u);

    EXPECT_EQ(tv.Ops()[0].Function, TransformFunction::Translate);
    EXPECT_EQ(tv.Ops()[0].TranslateX, StyleLength::Px(10.0f));
    EXPECT_EQ(tv.Ops()[0].TranslateY, StyleLength::Percent(-50.0f));

    EXPECT_EQ(tv.Ops()[1].Function, TransformFunction::Rotate);
    EXPECT_NEAR(tv.Ops()[1].Args[0], 90.0f, 1e-4f);

    EXPECT_EQ(tv.Ops()[2].Function, TransformFunction::Scale);
    EXPECT_FLOAT_EQ(tv.Ops()[2].Args[0], 2.0f);
    EXPECT_FLOAT_EQ(tv.Ops()[2].Args[1], 0.5f);

    EXPECT_EQ(tv.Ops()[3].Function, TransformFunction::Skew);
    EXPECT_FLOAT_EQ(tv.Ops()[3].Args[0], 10.0f);
    EXPECT_FLOAT_EQ(tv.Ops()[3].Args[1], 0.0f);
}

TEST(CssTransformStyle, SingleAxisFormsFillTheOtherAxisWithIdentity)
{
    const TransformValue tv =
        ComputeFor("#t { transform: translateY(4px) scaleX(3) rotate(1rad) scale(2) matrix(1, 0, 0, 1, 5, 6); }")
            .Visual.Transform;
    ASSERT_EQ(tv.Ops().size(), 5u);
    EXPECT_EQ(tv.Ops()[0].TranslateX, StyleLength::Px(0.0f));
    EXPECT_EQ(tv.Ops()[0].TranslateY, StyleLength::Px(4.0f));
    EXPECT_FLOAT_EQ(tv.Ops()[1].Args[0], 3.0f);
    EXPECT_FLOAT_EQ(tv.Ops()[1].Args[1], 1.0f);
    EXPECT_NEAR(tv.Ops()[2].Args[0], 57.29578f, 1e-3f);
    EXPECT_FLOAT_EQ(tv.Ops()[3].Args[0], 2.0f);
    EXPECT_FLOAT_EQ(tv.Ops()[3].Args[1], 2.0f);
    EXPECT_EQ(tv.Ops()[4].Function, TransformFunction::Matrix);
    EXPECT_FLOAT_EQ(tv.Ops()[4].Args[5], 6.0f);
}

// An unsupported or malformed function invalidates the whole declaration, so
// the earlier rule survives instead of a partial list — as in CSS.
TEST(CssTransformStyle, AnInvalidDeclarationLeavesTheEarlierRuleStanding)
{
    for (const char* bad : {"rotateX(10deg)", "rotate(10)", "translate(1px, 2px, 3px)", "scale(2px)",
                            "rotate(45deg) bogus(1)", "matrix(1, 0, 0, 1)", "rotate()", "translate(auto)"})
    {
        const std::string css = std::string("#t { transform: rotate(30deg); }\n#t { transform: ") + bad + "; }";
        const TransformValue tv = ComputeFor(css).Visual.Transform;
        ASSERT_EQ(tv.Ops().size(), 1u) << bad;
        EXPECT_FLOAT_EQ(tv.Ops()[0].Args[0], 30.0f) << bad;
    }
}

// A list has no length limit, in CSS or from the builders.
TEST(CssTransformStyle, AListOfAnyLengthIsKeptWhole)
{
    const TransformValue tv =
        ComputeFor("#t { transform: rotate(1deg) rotate(2deg) rotate(3deg) rotate(4deg) rotate(5deg) rotate(6deg) "
                   "rotate(7deg) rotate(8deg) rotate(9deg) rotate(10deg); }")
            .Visual.Transform;
    ASSERT_EQ(tv.Ops().size(), 10u);
    EXPECT_FLOAT_EQ(tv.Ops()[9].Args[0], 10.0f);

    TransformValue built;
    for (int i = 0; i < 12; ++i)
        built.Rotate(static_cast<float>(i));
    EXPECT_EQ(built.Ops().size(), 12u);
}

TEST(CssTransformStyle, NoneResetsALaterRule)
{
    const ResolvedStyle s = ComputeFor("#t { transform: rotate(30deg); }\n#t { transform: none; }");
    EXPECT_TRUE(s.Visual.Transform.IsNone());
}

TEST(CssTransformStyle, OriginTakesKeywordsLengthsAndPercentages)
{
    TransformOriginValue o = ComputeFor("#t { transform-origin: left top; }").Visual.TransformOrigin;
    EXPECT_EQ(o.X, StyleLength::Percent(0.0f));
    EXPECT_EQ(o.Y, StyleLength::Percent(0.0f));
    o = ComputeFor("#t { transform-origin: 12px 75%; }").Visual.TransformOrigin;
    EXPECT_EQ(o.X, StyleLength::Px(12.0f));
    EXPECT_EQ(o.Y, StyleLength::Percent(75.0f));
}

TEST(CssTransformStyle, IsNotInherited)
{
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString("#p { transform: rotate(10deg); }", sheet));
    ElementState st{};
    UIElement parent;
    parent.SetId("p");
    auto childOwned = std::make_unique<UIElement>();
    UIElement* child = childOwned.get();
    parent.AddChild(std::move(childOwned));
    const std::vector<const Stylesheet*> sheets{&sheet};
    const ResolvedStyle ps = CSSParser::ComputeStyleFor(parent, sheets, st, static_cast<const ResolvedStyle*>(nullptr));
    const ResolvedStyle cs = CSSParser::ComputeStyleFor(*child, sheets, st, &ps);
    EXPECT_FALSE(ps.Visual.Transform.IsNone());
    EXPECT_TRUE(cs.Visual.Transform.IsNone());
}

TEST(CssTransformStyle, OverridesApplyThroughTheTypedKeys)
{
    StyleOverrides ovr;
    ovr.Set(Style::Transform,
            TransformValue{}.Rotate(45.0f).Translate(StyleLength::Percent(-50.0f), StyleLength::Percent(-50.0f)));
    ovr.Set(Style::TransformOrigin, TransformOriginValue{StyleLength::Px(0.0f), StyleLength::Percent(100.0f)});
    ResolvedStyle rs{};
    ApplyOverridesToResolvedStyle(rs, ovr);
    ASSERT_EQ(rs.Visual.Transform.Ops().size(), 2u);
    EXPECT_EQ(rs.Visual.Transform.Ops()[0].Function, TransformFunction::Rotate);
    EXPECT_EQ(rs.Visual.Transform.Ops()[1].TranslateX, StyleLength::Percent(-50.0f));
    EXPECT_EQ(rs.Visual.TransformOrigin.X, StyleLength::Px(0.0f));
    EXPECT_EQ(rs.Visual.TransformOrigin.Y, StyleLength::Percent(100.0f));
}

// A transform change is a paint change confined to one transform slot: the
// drain rewrites that slot and the shader carries it to the descendants.
TEST(CssTransformStyle, IsPaintOnlyAndDoesNotForceASubtreeRegen)
{
    for (StylePropertyId id : {StylePropertyId::Transform, StylePropertyId::TransformOrigin})
    {
        const StylePropertyImpact impact = GetStylePropertyImpact(id);
        EXPECT_FALSE(impact.Layout);
        EXPECT_TRUE(impact.Paint);
        EXPECT_FALSE(impact.Subtree);
        EXPECT_TRUE(IsTransitionable(id));
    }
}

TEST(CssTransformStyle, MatchingListsInterpolateFunctionByFunction)
{
    const StyleValue a = TransformValue{}.Rotate(0.0f).Translate(StyleLength::Px(0.0f), StyleLength::Percent(10.0f));
    const StyleValue b =
        TransformValue{}.Rotate(90.0f).Translate(StyleLength::Px(20.0f), StyleLength::Percent(30.0f));
    const TransformValue mid = std::get<TransformValue>(InterpolateProperty(StylePropertyId::Transform, a, b, 0.5f));
    ASSERT_EQ(mid.Ops().size(), 2u);
    EXPECT_FLOAT_EQ(mid.Ops()[0].Args[0], 45.0f);
    EXPECT_EQ(mid.Ops()[1].TranslateX, StyleLength::Px(10.0f));
    EXPECT_EQ(mid.Ops()[1].TranslateY, StyleLength::Percent(20.0f));
}

TEST(CssTransformStyle, NoneInterpolatesAsTheOtherListsIdentity)
{
    const StyleValue none = TransformValue{};
    const StyleValue b = TransformValue{}.Scale(3.0f, 3.0f).Rotate(40.0f);
    const TransformValue mid = std::get<TransformValue>(InterpolateProperty(StylePropertyId::Transform, none, b, 0.5f));
    ASSERT_EQ(mid.Ops().size(), 2u);
    EXPECT_FLOAT_EQ(mid.Ops()[0].Args[0], 2.0f);
    EXPECT_FLOAT_EQ(mid.Ops()[0].Args[1], 2.0f);
    EXPECT_FLOAT_EQ(mid.Ops()[1].Args[0], 20.0f);
}

TEST(CssTransformStyle, MismatchedListsFlipAtTheMidpoint)
{
    const StyleValue a = TransformValue{}.Rotate(10.0f);
    const StyleValue b = TransformValue{}.Scale(2.0f, 2.0f);
    EXPECT_EQ(std::get<TransformValue>(InterpolateProperty(StylePropertyId::Transform, a, b, 0.25f)),
              std::get<TransformValue>(a));
    EXPECT_EQ(std::get<TransformValue>(InterpolateProperty(StylePropertyId::Transform, a, b, 0.75f)),
              std::get<TransformValue>(b));
}

TEST(CssTransformStyle, NoneComputesNoMatrix)
{
    VisualStyle vs{};
    Mathematics::Matrix3x3 m;
    EXPECT_FALSE(UI::ComputeElementTransform(vs, Box(0, 0, 10, 10), 1.0f, m));
}

// rotate(90deg) about the default 50% 50% origin of a 100x40 box at (10,20):
// the centre stays put and the right-middle point swings to bottom-middle.
TEST(CssTransformStyle, RotationPivotsAboutTheOriginClockwise)
{
    VisualStyle vs{};
    vs.Transform.Rotate(90.0f);
    Mathematics::Matrix3x3 m;
    ASSERT_TRUE(UI::ComputeElementTransform(vs, Box(10, 20, 100, 40), 1.0f, m));
    ExpectPoint(m, 60, 40, 60, 40);
    ExpectPoint(m, 110, 40, 60, 90);
}

// CSS applies the list left to right: translate then rotate moves along the
// unrotated x axis, rotate then translate along the rotated one.
TEST(CssTransformStyle, FunctionsComposeLeftToRight)
{
    VisualStyle a{};
    a.TransformOrigin = {StyleLength::Px(0.0f), StyleLength::Px(0.0f)};
    a.Transform.Translate(StyleLength::Px(10.0f), StyleLength::Px(0.0f)).Rotate(90.0f);
    VisualStyle b = a;
    b.Transform = TransformValue{}.Rotate(90.0f).Translate(StyleLength::Px(10.0f), StyleLength::Px(0.0f));
    Mathematics::Matrix3x3 ma;
    Mathematics::Matrix3x3 mb;
    ASSERT_TRUE(UI::ComputeElementTransform(a, Box(0, 0, 100, 100), 1.0f, ma));
    ASSERT_TRUE(UI::ComputeElementTransform(b, Box(0, 0, 100, 100), 1.0f, mb));
    ExpectPoint(ma, 0, 0, 10, 0);
    ExpectPoint(mb, 0, 0, 0, 10);
}

// Percentages in translate() and transform-origin are of the border box, and
// the output scale multiplies lengths but never the rotation or scale.
TEST(CssTransformStyle, PercentagesUseTheBorderBoxAndScaleConvertsUnits)
{
    VisualStyle vs{};
    vs.TransformOrigin = {StyleLength::Px(0.0f), StyleLength::Percent(100.0f)};
    vs.Transform.Translate(StyleLength::Percent(50.0f), StyleLength::Px(0.0f)).Scale(2.0f, 2.0f);
    Mathematics::Matrix3x3 m;
    ASSERT_TRUE(UI::ComputeElementTransform(vs, Box(0, 0, 40, 20), 2.0f, m));
    // Origin (0, 20) logical = (0, 40) physical; translate 50% of 40 = 20 logical = 40 physical.
    ExpectPoint(m, 0, 40, 40, 40);
    ExpectPoint(m, 10, 40, 60, 40);
}

TEST(CssTransformStyle, InverseUndoesTheMapAndRefusesSingularOnes)
{
    VisualStyle vs{};
    vs.Transform.Rotate(30.0f).Scale(2.0f, 0.5f).Skew(10.0f, 0.0f);
    Mathematics::Matrix3x3 m;
    Mathematics::Matrix3x3 inv;
    ASSERT_TRUE(UI::ComputeElementTransform(vs, Box(5, 5, 50, 50), 1.0f, m));
    ASSERT_TRUE(Mathematics::TryInverseAffine2D(m, inv));
    const Mathematics::Vector2 back = inv.TransformPoint(m.TransformPoint(Mathematics::Vector2(17.0f, -3.0f)));
    EXPECT_NEAR(back.x, 17.0f, 1e-3f);
    EXPECT_NEAR(back.y, -3.0f, 1e-3f);

    VisualStyle flat{};
    flat.Transform.Scale(0.0f, 0.0f);
    ASSERT_TRUE(UI::ComputeElementTransform(flat, Box(0, 0, 10, 10), 1.0f, m));
    EXPECT_FALSE(Mathematics::TryInverseAffine2D(m, inv));
}
