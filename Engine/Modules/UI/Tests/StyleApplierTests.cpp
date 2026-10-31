#include "../Source/StyleApplier.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleOverrides.h"
#include "UI/StyleProperties.h"
#include "UI/UIStyle.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{
template<class... Arguments>
concept CanSetStyle = requires(StyleOverrides& overrides, Arguments&&... arguments)
{
    overrides.Set(std::forward<Arguments>(arguments)...);
};

static_assert(CanSetStyle<decltype(Style::Gap), StyleLength>);
static_assert(CanSetStyle<decltype(Style::FlexDir), FlexDirection,
    decltype(Style::AlignItems), AlignItems, decltype(Style::Gap), StyleLength>);
static_assert(CanSetStyle<decltype(Style::Gap), const StyleLength&,
    decltype(Style::FontFamily), const std::vector<std::string>&>);
static_assert(!CanSetStyle<decltype(Style::Gap), StyleLength,
    decltype(Style::FontFamily), std::string>);
static_assert(!CanSetStyle<>);
static_assert(!CanSetStyle<decltype(Style::Gap)>);
static_assert(!CanSetStyle<decltype(Style::Gap), float>);
static_assert(!CanSetStyle<decltype(Style::Gap), StyleLength, decltype(Style::Width)>);
static_assert(!CanSetStyle<decltype(Style::Gap), StyleLength, decltype(Style::Width), float>);
static_assert(!CanSetStyle<decltype(Style::Gap), StyleLength, StyleLength, decltype(Style::Width)>);
static_assert(!CanSetStyle<decltype(Style::FlexDir), FlexDirection,
    decltype(Style::AlignItems), FlexDirection>);
}

TEST(StyleApplierTests, PropertyPairsPreserveTypedValuesAndLastWriteOrder)
{
    StyleOverrides overrides;
    const auto kGap = StyleLength::Px(6.0f);
    std::vector<std::string> fonts{"Inter", "sans-serif"};
    EXPECT_EQ(&overrides.Set(Style::FlexDir, FlexDirection::Row,
        Style::AlignItems, AlignItems::Center, Style::Gap, kGap,
        Style::BackgroundColor, 0xFF102030u, Style::Gap, StyleLength::Px(9.0f),
        Style::FontFamily, fonts), &overrides);
    fonts.clear();

    EXPECT_EQ(overrides.PropCount(), 5u);
    EXPECT_EQ(overrides.Get(Style::AlignItems), AlignItems::Center);
    EXPECT_EQ(overrides.Get(Style::FontFamily), (std::vector<std::string>{"Inter", "sans-serif"}));
    ResolvedStyle resolved;
    ApplyOverridesToResolvedStyle(resolved, overrides);
    EXPECT_EQ(resolved.Layout.FlexDirection, FlexDirection::Row);
    EXPECT_FLOAT_EQ(resolved.Layout.Gap, 9.0f);
    EXPECT_FLOAT_EQ(resolved.Layout.RowGap, 9.0f);
    EXPECT_FLOAT_EQ(resolved.Layout.ColumnGap, 9.0f);
    EXPECT_EQ(resolved.Visual.BackgroundColor, 0xFF102030u);
}

TEST(StyleOverridesDirtyTests, PropertyPairsNotifyInOrderAndSkipUnchangedValues)
{
    StyleOverrides overrides;
    std::vector<std::uint32_t> notifications;
    overrides.SetDirtyCallback([](void* state, uint32_t flags)
    {
        static_cast<std::vector<std::uint32_t>*>(state)->push_back(flags);
    }, &notifications);

    overrides.Set(Style::Width, StyleLength::Px(100.0f), Style::BackgroundColor, 0xFF102030u);
    EXPECT_EQ(notifications, (std::vector<std::uint32_t>{StyleOverrides::Layout, StyleOverrides::Visual}));
    EXPECT_TRUE(overrides.IsLayoutDirty());
    EXPECT_TRUE(overrides.IsVisualDirty());
    EXPECT_FALSE(overrides.NeedsCascadeRerun());

    notifications.clear();
    overrides.ClearDirty();
    overrides.Set(Style::Width, StyleLength::Px(100.0f), Style::BackgroundColor, 0xFF203040u);
    EXPECT_EQ(notifications, (std::vector<std::uint32_t>{StyleOverrides::Visual}));
    EXPECT_FALSE(overrides.IsLayoutDirty());
    EXPECT_TRUE(overrides.IsVisualDirty());

    notifications.clear();
    overrides.ClearDirty();
    overrides.Set(Style::Width, StyleLength::Px(100.0f), Style::BackgroundColor, 0xFF203040u);
    EXPECT_TRUE(notifications.empty());
    EXPECT_FALSE(overrides.IsLayoutDirty());
    EXPECT_FALSE(overrides.IsVisualDirty());
    EXPECT_FALSE(overrides.NeedsCascadeRerun());
}

TEST(StyleApplierTests, GapOverrideSetsRowAndColumnGap)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::Gap, StyleLength::Px(12.0f));

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_FLOAT_EQ(rs.Layout.Gap, 12.0f);
    EXPECT_FLOAT_EQ(rs.Layout.RowGap, 12.0f);
    EXPECT_FLOAT_EQ(rs.Layout.ColumnGap, 12.0f);
}

TEST(StyleApplierTests, RowGapOverrideOnlySetsRowGap)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::RowGap, StyleLength::Px(5.0f));

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_FLOAT_EQ(rs.Layout.RowGap, 5.0f);
    EXPECT_FLOAT_EQ(rs.Layout.ColumnGap, 0.0f);
}

TEST(StyleApplierTests, ColumnGapOverrideOnlySetsColumnGap)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::ColumnGap, StyleLength::Px(7.0f));

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_FLOAT_EQ(rs.Layout.ColumnGap, 7.0f);
    EXPECT_FLOAT_EQ(rs.Layout.RowGap, 0.0f);
}

// Every inherited visual property has to record that an override set it, or
// the next style resolve inherits the parent's value over the top of it:
// InheritProperties (UIManager_StyleResolve.cpp) copies down whenever the
// child's Has* flag is clear. LineHeight was the one applier that set the
// value without the flag.
TEST(StyleApplierTests, LineHeightOverrideMarksItExplicitlySet)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::LineHeight, 1.5f);

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_FLOAT_EQ(rs.Visual.LineHeight, 1.5f);
    EXPECT_TRUE(rs.Visual.HasLineHeight);
}

TEST(StyleApplierTests, PaddingTopOverrideSetsTopOnly)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::PaddingTop, StyleLength::Px(10.0f));

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_FLOAT_EQ(rs.Layout.Padding.Top, 10.0f);
    EXPECT_FLOAT_EQ(rs.Layout.Padding.Right, 0.0f);
    EXPECT_FLOAT_EQ(rs.Layout.Padding.Bottom, 0.0f);
    EXPECT_FLOAT_EQ(rs.Layout.Padding.Left, 0.0f);
}

TEST(StyleApplierTests, MarginTopOverrideSetsTopOnly)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::MarginTop, StyleLength::Px(8.0f));

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_FLOAT_EQ(rs.Layout.Margin.Top, 8.0f);
    EXPECT_FLOAT_EQ(rs.Layout.Margin.Right, 0.0f);
    EXPECT_FLOAT_EQ(rs.Layout.Margin.Bottom, 0.0f);
    EXPECT_FLOAT_EQ(rs.Layout.Margin.Left, 0.0f);
}

TEST(StyleApplierTests, BorderRadiusOverrideSetsAllCorners)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f});

    ApplyOverridesToResolvedStyle(rs, overrides);

    // The programmatic override authors circular corners, so both semi-axes of
    // every corner carry the radius.
    for (const CornerRadius& corner : {rs.Visual.BorderRadius.TopLeft,
                                       rs.Visual.BorderRadius.TopRight,
                                       rs.Visual.BorderRadius.BottomRight,
                                       rs.Visual.BorderRadius.BottomLeft})
    {
        EXPECT_FLOAT_EQ(corner.X, 4.0f);
        EXPECT_FLOAT_EQ(corner.Y, 4.0f);
    }
}

TEST(StyleApplierTests, BorderWidthOverrideSetsAllEdges)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::BorderWidth, Box4{2.0f, 2.0f, 2.0f, 2.0f});

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_FLOAT_EQ(rs.Layout.BorderWidth.Top, 2.0f);
    EXPECT_FLOAT_EQ(rs.Layout.BorderWidth.Right, 2.0f);
    EXPECT_FLOAT_EQ(rs.Layout.BorderWidth.Bottom, 2.0f);
    EXPECT_FLOAT_EQ(rs.Layout.BorderWidth.Left, 2.0f);
}

TEST(StyleApplierTests, BorderColorOverrideSetsAllEdges)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::BorderColor, BorderColorsTRBL{0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu});

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_EQ(rs.Visual.BorderColor.Top, 0xFF0000FFu);
    EXPECT_EQ(rs.Visual.BorderColor.Right, 0xFF0000FFu);
    EXPECT_EQ(rs.Visual.BorderColor.Bottom, 0xFF0000FFu);
    EXPECT_EQ(rs.Visual.BorderColor.Left, 0xFF0000FFu);
}

TEST(StyleApplierTests, BackgroundColorOverrideSetsValue)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::BackgroundColor, 0xFF3C3C43u);

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_EQ(rs.Visual.BackgroundColor, 0xFF3C3C43u);
}

TEST(StyleApplierTests, OpacityOverrideSetsValue)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::Opacity, 0.5f);

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_FLOAT_EQ(rs.Visual.LocalOpacity, 0.5f);
}

TEST(StyleApplierTests, WidthAndHeightOverridesSetValues)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::Width, StyleLength::Px(200.0f));
    overrides.Set(Style::Height, StyleLength::Px(100.0f));

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_FLOAT_EQ(rs.Layout.Width.Value, 200.0f);
    EXPECT_FLOAT_EQ(rs.Layout.Height.Value, 100.0f);
}

TEST(StyleApplierTests, FlexPropertiesSetValues)
{
    ResolvedStyle rs;
    StyleOverrides overrides;
    overrides.Set(Style::FlexGrow, 1.0f);
    overrides.Set(Style::FlexShrink, 0.0f);
    overrides.Set(Style::FlexDir, FlexDirection::Column);

    ApplyOverridesToResolvedStyle(rs, overrides);

    EXPECT_FLOAT_EQ(rs.Layout.FlexGrow, 1.0f);
    // An override is a declaration: the value must land AND be recorded as
    // declared, or a block-flow container reads it back as undeclared and
    // substitutes its own default.
    ASSERT_TRUE(rs.Layout.FlexShrink.has_value());
    EXPECT_FLOAT_EQ(*rs.Layout.FlexShrink, 0.0f);
    EXPECT_EQ(rs.Layout.FlexDirection, FlexDirection::Column);
}

// --- Dirty flag and NeedsCascadeRerun tests ---

TEST(StyleOverridesDirtyTests, SetLayoutPropertyMarksLayoutDirty)
{
    StyleOverrides overrides;
    EXPECT_FALSE(overrides.IsLayoutDirty());
    overrides.Set(Style::Width, StyleLength::Px(100.0f));
    EXPECT_TRUE(overrides.IsLayoutDirty());
    EXPECT_FALSE(overrides.IsVisualDirty());
    EXPECT_FALSE(overrides.NeedsCascadeRerun());
}

TEST(StyleOverridesDirtyTests, SetVisualPropertyMarksVisualDirty)
{
    StyleOverrides overrides;
    EXPECT_FALSE(overrides.IsVisualDirty());
    overrides.Set(Style::BackgroundColor, 0xFF000000u);
    EXPECT_TRUE(overrides.IsVisualDirty());
    EXPECT_FALSE(overrides.IsLayoutDirty());
    EXPECT_FALSE(overrides.NeedsCascadeRerun());
}

TEST(StyleOverridesDirtyTests, ResetLayoutPropertySetsCascadeRerun)
{
    StyleOverrides overrides;
    overrides.Set(Style::Display, DisplayMode::None);
    overrides.ClearDirty();

    overrides.Reset(Style::Display);
    EXPECT_TRUE(overrides.IsLayoutDirty());
    EXPECT_TRUE(overrides.NeedsCascadeRerun());
    EXPECT_EQ(overrides.PropCount(), 0u);
}

TEST(StyleOverridesDirtyTests, ResetVisualPropertySetsCascadeRerun)
{
    StyleOverrides overrides;
    overrides.Set(Style::BackgroundColor, 0xFF000000u);
    overrides.ClearDirty();

    overrides.Reset(Style::BackgroundColor);
    EXPECT_TRUE(overrides.IsVisualDirty());
    EXPECT_TRUE(overrides.NeedsCascadeRerun());
    EXPECT_EQ(overrides.PropCount(), 0u);
}

TEST(StyleOverridesDirtyTests, ResetNonexistentPropertyDoesNotSetDirty)
{
    StyleOverrides overrides;
    overrides.Reset(Style::Display);
    EXPECT_FALSE(overrides.IsLayoutDirty());
    EXPECT_FALSE(overrides.NeedsCascadeRerun());
}

TEST(StyleOverridesDirtyTests, ClearDirtyClearsAllFlags)
{
    StyleOverrides overrides;
    overrides.Set(Style::Width, StyleLength::Px(100.0f));
    overrides.Set(Style::BackgroundColor, 0xFF000000u);
    overrides.Reset(Style::Width);

    EXPECT_TRUE(overrides.IsLayoutDirty());
    EXPECT_TRUE(overrides.IsVisualDirty());
    EXPECT_TRUE(overrides.NeedsCascadeRerun());

    overrides.ClearDirty();

    EXPECT_FALSE(overrides.IsLayoutDirty());
    EXPECT_FALSE(overrides.IsVisualDirty());
    EXPECT_FALSE(overrides.NeedsCascadeRerun());
}

TEST(StyleOverridesDirtyTests, SetThenResetSameFrameSetsCascadeRerun)
{
    StyleOverrides overrides;
    overrides.Set(Style::Display, DisplayMode::Flex);
    overrides.Reset(Style::Display);

    EXPECT_TRUE(overrides.IsLayoutDirty());
    EXPECT_TRUE(overrides.NeedsCascadeRerun());
    EXPECT_EQ(overrides.PropCount(), 0u);
}

TEST(StyleOverridesDirtyTests, ResetLastOverrideLeavesZeroPropCount)
{
    StyleOverrides overrides;
    overrides.Set(Style::Display, DisplayMode::None);
    overrides.ClearDirty();

    overrides.Reset(Style::Display);

    EXPECT_EQ(overrides.PropCount(), 0u);
    EXPECT_TRUE(overrides.NeedsCascadeRerun());

    overrides.ClearDirty();
    EXPECT_FALSE(overrides.NeedsCascadeRerun());
}

TEST(StyleOverridesDirtyTests, ClearRemovesPropsAndMarksDirty)
{
    StyleOverrides overrides;
    overrides.Set(Style::Width, StyleLength::Px(100.0f));
    overrides.Set(Style::BackgroundColor, 0xFFFFFFFFu);

    overrides.Clear();

    // Clearing a non-empty overrides set must re-mark dirty so the cascade
    // re-runs and picks up CSS base values for the removed properties.
    EXPECT_TRUE(overrides.IsLayoutDirty());
    EXPECT_TRUE(overrides.IsVisualDirty());
    EXPECT_TRUE(overrides.NeedsCascadeRerun());
    EXPECT_EQ(overrides.PropCount(), 0u);
}

TEST(StyleOverridesDirtyTests, ClearOnEmptyIsNoOp)
{
    StyleOverrides overrides;
    overrides.Clear();

    EXPECT_FALSE(overrides.IsLayoutDirty());
    EXPECT_FALSE(overrides.IsVisualDirty());
    EXPECT_FALSE(overrides.NeedsCascadeRerun());
    EXPECT_EQ(overrides.PropCount(), 0u);
}
