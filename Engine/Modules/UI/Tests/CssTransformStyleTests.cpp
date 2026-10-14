// `transform` and `transform-origin`: parsing, the cascade, overrides,
// interpolation and the matrix ComputeElementTransform builds. What the
// renderer and hit-testing do with it is CssTransformRenderTests.cpp.

#include <gtest/gtest.h>

#include <string>

#include "UI/Parsers/CSSParser.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UITransform.h"

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

void ExpectPoint(const UI::Affine2D& m, float x, float y, float ex, float ey)
{
    m.Apply(x, y);
    EXPECT_NEAR(x, ex, 1e-3f);
    EXPECT_NEAR(y, ey, 1e-3f);
}

} // namespace

TEST(CssTransformStyle, NoneIsTheInitialValue)
{
    const ResolvedStyle s = ComputeFor("#t { opacity: 1; }");
    EXPECT_TRUE(s.Visual.Transform.IsNone());
    EXPECT_TRUE(s.Visual.TransformOrigin.XIsPercent);
    EXPECT_FLOAT_EQ(s.Visual.TransformOrigin.X, 50.0f);
    EXPECT_FLOAT_EQ(s.Visual.TransformOrigin.Y, 50.0f);
}

TEST(CssTransformStyle, ParsesAFunctionListInOrder)
{
    const ResolvedStyle s =
        ComputeFor("#t { transform: translate(10px, -50%) rotate(0.25turn) scale(2, 50%) skewX(10deg); }");
    const TransformValue& tv = s.Visual.Transform;
    ASSERT_EQ(tv.Count, 4);

    EXPECT_EQ(tv.Ops[0].Function, TransformFunction::Translate);
    EXPECT_FLOAT_EQ(tv.Ops[0].Args[0], 10.0f);
    EXPECT_FALSE(tv.Ops[0].XIsPercent);
    EXPECT_FLOAT_EQ(tv.Ops[0].Args[1], -50.0f);
    EXPECT_TRUE(tv.Ops[0].YIsPercent);

    EXPECT_EQ(tv.Ops[1].Function, TransformFunction::Rotate);
    EXPECT_NEAR(tv.Ops[1].Args[0], 90.0f, 1e-4f);

    EXPECT_EQ(tv.Ops[2].Function, TransformFunction::Scale);
    EXPECT_FLOAT_EQ(tv.Ops[2].Args[0], 2.0f);
    EXPECT_FLOAT_EQ(tv.Ops[2].Args[1], 0.5f);

    EXPECT_EQ(tv.Ops[3].Function, TransformFunction::Skew);
    EXPECT_FLOAT_EQ(tv.Ops[3].Args[0], 10.0f);
    EXPECT_FLOAT_EQ(tv.Ops[3].Args[1], 0.0f);
}

TEST(CssTransformStyle, SingleAxisFormsFillTheOtherAxisWithIdentity)
{
    const TransformValue tv =
        ComputeFor("#t { transform: translateY(4px) scaleX(3) rotate(1rad) scale(2) matrix(1, 0, 0, 1, 5, 6); }")
            .Visual.Transform;
    ASSERT_EQ(tv.Count, 5);
    EXPECT_FLOAT_EQ(tv.Ops[0].Args[0], 0.0f);
    EXPECT_FLOAT_EQ(tv.Ops[0].Args[1], 4.0f);
    EXPECT_FLOAT_EQ(tv.Ops[1].Args[0], 3.0f);
    EXPECT_FLOAT_EQ(tv.Ops[1].Args[1], 1.0f);
    EXPECT_NEAR(tv.Ops[2].Args[0], 57.29578f, 1e-3f);
    EXPECT_FLOAT_EQ(tv.Ops[3].Args[0], 2.0f);
    EXPECT_FLOAT_EQ(tv.Ops[3].Args[1], 2.0f);
    EXPECT_EQ(tv.Ops[4].Function, TransformFunction::Matrix);
    EXPECT_FLOAT_EQ(tv.Ops[4].Args[5], 6.0f);
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
        ASSERT_EQ(tv.Count, 1) << bad;
        EXPECT_FLOAT_EQ(tv.Ops[0].Args[0], 30.0f) << bad;
    }
}

TEST(CssTransformStyle, NoneResetsALaterRule)
{
    const ResolvedStyle s = ComputeFor("#t { transform: rotate(30deg); }\n#t { transform: none; }");
    EXPECT_TRUE(s.Visual.Transform.IsNone());
}

TEST(CssTransformStyle, OriginTakesKeywordsLengthsAndPercentages)
{
    TransformOriginValue o = ComputeFor("#t { transform-origin: left top; }").Visual.TransformOrigin;
    EXPECT_FLOAT_EQ(o.X, 0.0f);
    EXPECT_FLOAT_EQ(o.Y, 0.0f);
    o = ComputeFor("#t { transform-origin: 12px 75%; }").Visual.TransformOrigin;
    EXPECT_FALSE(o.XIsPercent);
    EXPECT_FLOAT_EQ(o.X, 12.0f);
    EXPECT_TRUE(o.YIsPercent);
    EXPECT_FLOAT_EQ(o.Y, 75.0f);
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
    ovr.Set(Style::Transform, TransformValue{}.Rotate(45.0f).Translate(-50.0f, -50.0f, true, true));
    ovr.Set(Style::TransformOrigin, TransformOriginValue{0.0f, false, 100.0f, true});
    ResolvedStyle rs{};
    ApplyOverridesToResolvedStyle(rs, ovr);
    ASSERT_EQ(rs.Visual.Transform.Count, 2);
    EXPECT_EQ(rs.Visual.Transform.Ops[0].Function, TransformFunction::Rotate);
    EXPECT_TRUE(rs.Visual.Transform.Ops[1].XIsPercent);
    EXPECT_FLOAT_EQ(rs.Visual.TransformOrigin.X, 0.0f);
    EXPECT_FLOAT_EQ(rs.Visual.TransformOrigin.Y, 100.0f);
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
    const StyleValue a = TransformValue{}.Rotate(0.0f).Translate(0.0f, 10.0f);
    const StyleValue b = TransformValue{}.Rotate(90.0f).Translate(20.0f, 10.0f);
    const TransformValue mid = std::get<TransformValue>(InterpolateProperty(StylePropertyId::Transform, a, b, 0.5f));
    ASSERT_EQ(mid.Count, 2);
    EXPECT_FLOAT_EQ(mid.Ops[0].Args[0], 45.0f);
    EXPECT_FLOAT_EQ(mid.Ops[1].Args[0], 10.0f);
    EXPECT_FLOAT_EQ(mid.Ops[1].Args[1], 10.0f);
}

TEST(CssTransformStyle, NoneInterpolatesAsTheOtherListsIdentity)
{
    const StyleValue none = TransformValue{};
    const StyleValue b = TransformValue{}.Scale(3.0f).Rotate(40.0f);
    const TransformValue mid = std::get<TransformValue>(InterpolateProperty(StylePropertyId::Transform, none, b, 0.5f));
    ASSERT_EQ(mid.Count, 2);
    EXPECT_FLOAT_EQ(mid.Ops[0].Args[0], 2.0f);
    EXPECT_FLOAT_EQ(mid.Ops[0].Args[1], 2.0f);
    EXPECT_FLOAT_EQ(mid.Ops[1].Args[0], 20.0f);
}

TEST(CssTransformStyle, MismatchedListsFlipAtTheMidpoint)
{
    const StyleValue a = TransformValue{}.Rotate(10.0f);
    const StyleValue b = TransformValue{}.Scale(2.0f);
    EXPECT_EQ(std::get<TransformValue>(InterpolateProperty(StylePropertyId::Transform, a, b, 0.25f)),
              std::get<TransformValue>(a));
    EXPECT_EQ(std::get<TransformValue>(InterpolateProperty(StylePropertyId::Transform, a, b, 0.75f)),
              std::get<TransformValue>(b));
}

TEST(CssTransformStyle, NoneComputesNoMatrix)
{
    VisualStyle vs{};
    UI::Affine2D m;
    EXPECT_FALSE(UI::ComputeElementTransform(vs, 0, 0, 10, 10, 1.0f, m));
}

// rotate(90deg) about the default 50% 50% origin of a 100x40 box at (10,20):
// the centre stays put and the right-middle point swings to bottom-middle.
TEST(CssTransformStyle, RotationPivotsAboutTheOriginClockwise)
{
    VisualStyle vs{};
    vs.Transform.Rotate(90.0f);
    UI::Affine2D m;
    ASSERT_TRUE(UI::ComputeElementTransform(vs, 10, 20, 100, 40, 1.0f, m));
    ExpectPoint(m, 60, 40, 60, 40);
    ExpectPoint(m, 110, 40, 60, 90);
}

// CSS applies the list left to right: translate then rotate moves along the
// unrotated x axis, rotate then translate along the rotated one.
TEST(CssTransformStyle, FunctionsComposeLeftToRight)
{
    VisualStyle a{};
    a.TransformOrigin = {0.0f, false, 0.0f, false};
    a.Transform.Translate(10.0f, 0.0f).Rotate(90.0f);
    VisualStyle b = a;
    b.Transform = TransformValue{}.Rotate(90.0f).Translate(10.0f, 0.0f);
    UI::Affine2D ma, mb;
    ASSERT_TRUE(UI::ComputeElementTransform(a, 0, 0, 100, 100, 1.0f, ma));
    ASSERT_TRUE(UI::ComputeElementTransform(b, 0, 0, 100, 100, 1.0f, mb));
    ExpectPoint(ma, 0, 0, 10, 0);
    ExpectPoint(mb, 0, 0, 0, 10);
}

// Percentages in translate() and transform-origin are of the border box, and
// the output scale multiplies lengths but never the rotation or scale.
TEST(CssTransformStyle, PercentagesUseTheBorderBoxAndScaleConvertsUnits)
{
    VisualStyle vs{};
    vs.TransformOrigin = {0.0f, false, 100.0f, true};
    vs.Transform.Translate(50.0f, 0.0f, true, false).Scale(2.0f);
    UI::Affine2D m;
    ASSERT_TRUE(UI::ComputeElementTransform(vs, 0, 0, 40, 20, 2.0f, m));
    // Origin (0, 20) logical = (0, 40) physical; translate 50% of 40 = 20 logical = 40 physical.
    ExpectPoint(m, 0, 40, 40, 40);
    ExpectPoint(m, 10, 40, 60, 40);
}

TEST(CssTransformStyle, InverseUndoesTheMapAndRefusesSingularOnes)
{
    VisualStyle vs{};
    vs.Transform.Rotate(30.0f).Scale(2.0f, 0.5f).Skew(10.0f, 0.0f);
    UI::Affine2D m, inv;
    ASSERT_TRUE(UI::ComputeElementTransform(vs, 5, 5, 50, 50, 1.0f, m));
    ASSERT_TRUE(m.Inverse(inv));
    float x = 17.0f, y = -3.0f;
    m.Apply(x, y);
    inv.Apply(x, y);
    EXPECT_NEAR(x, 17.0f, 1e-3f);
    EXPECT_NEAR(y, -3.0f, 1e-3f);

    VisualStyle flat{};
    flat.Transform.Scale(0.0f);
    ASSERT_TRUE(UI::ComputeElementTransform(flat, 0, 0, 10, 10, 1.0f, m));
    EXPECT_FALSE(m.Inverse(inv));
}
