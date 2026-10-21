#include "IsolatedUIFixture.h"
#include "RobotoTestFont.h"

#include "UI/Controls/TextArea.h"
#include "UI/StyleProperties.h"
#include "UI/UIStyle.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <vector>

using GameEngine::TextArea;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{
constexpr char kXml[] = R"(<uielement id="root"><textarea id="text"/></uielement>)";
constexpr char kCss[] = R"(
#root { width: 3200px; height: 400px; }
#text {
    width: 3000px;
    height: 300px;
    font-family: Roboto;
    font-size: 16px;
    line-height: 24px;
    white-space: normal;
    word-break: break-word;
    padding: 0px;
    border-width: 0px;
}
)";

constexpr char kComparisonXml[] = R"(<uielement id="root">
  <textarea id="text" class="area"/>
  <textarea id="reference" class="area"/>
</uielement>)";
constexpr char kComparisonCss[] = R"(
#root { display: flex; flex-direction: row; width: 600px; height: 400px; }
.area {
    width: 83px;
    height: 300px;
    font-family: Roboto;
    font-size: 16px;
    line-height: 24px;
    white-space: normal;
    word-break: break-word;
    padding: 0px;
    border-width: 0px;
}
)";

// A forced-empty layout between reference values prevents the oracle from
// sharing any cached prefix with the edited control.
void ExpectFullLayout(TextArea& edited, TextArea& reference)
{
    reference.SetValue("");
    (void)reference.GetVisualLineCount();
    reference.SetValue(edited.GetValue());
    ASSERT_EQ(edited.GetVisualLineCount(), reference.GetVisualLineCount());
    const std::string& text = edited.GetValue();
    for (size_t byte = 0; byte <= text.size(); ++byte)
    {
        if (byte < text.size() && (static_cast<unsigned char>(text[byte]) & 0xC0u) == 0x80u)
            continue;
        for (const int key : {GameEngine::Input::kKeyCode_Home, GameEngine::Input::kKeyCode_End})
        {
            edited.SetSelection(static_cast<int>(byte), static_cast<int>(byte));
            reference.SetSelection(static_cast<int>(byte), static_cast<int>(byte));
            ASSERT_TRUE(edited.OnKey(key, 0, nullptr));
            ASSERT_TRUE(reference.OnKey(key, 0, nullptr));
            ASSERT_EQ(edited.GetCaretIndex(), reference.GetCaretIndex()) << "byte " << byte << ", key " << key;
        }
    }
}
#ifdef NDEBUG
// Times one typed character replacing [start, end) of `before`, through the next
// line count. Replacing the first byte requires a full reflow; appending should
// retain the preceding visual lines.
double MeasureEdit(TextArea& area, const std::string& before, int start, int end)
{
    area.SetValue(before);
    area.SetSelection(start, end);
    (void)area.GetVisualLineCount();
    const auto begin = std::chrono::steady_clock::now();
    const bool consumed = area.OnChar('x');
    const size_t lines = area.GetVisualLineCount();
    const auto finish = std::chrono::steady_clock::now();
    const size_t replaced = static_cast<size_t>(end - start);
    EXPECT_TRUE(consumed);
    EXPECT_GT(lines, before.size() / 1024u);
    EXPECT_EQ(area.GetValue().size(), before.size() + 1u - replaced);
    return std::chrono::duration<double, std::milli>(finish - begin).count();
}

double MeasureAppend(TextArea& area, const std::string& before)
{
    const int end = static_cast<int>(before.size());
    return MeasureEdit(area, before, end, end);
}
#endif
} // namespace

TEST(TextAreaIncrementalWrap, AppendingReflowsLessThanAFullValueEdit)
{
#ifndef NDEBUG
    GTEST_SKIP() << "Measured wrapping regression requires an optimized Release build";
#else
    IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kXml, kCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();
    auto* area = dynamic_cast<TextArea*>(fixture.Element("text"));
    ASSERT_NE(area, nullptr);
    constexpr size_t kBytes = 64u * 1024u;
    constexpr size_t kSamples = 15;
    const std::string appendBefore(kBytes - 1, 'x');
    std::string fullBefore(kBytes, 'x');
    fullBefore.front() = 'a';
    (void)MeasureEdit(*area, fullBefore, 0, 1);
    (void)MeasureEdit(*area, appendBefore, static_cast<int>(kBytes - 1), static_cast<int>(kBytes - 1));

    std::array<double, kSamples> appendMs{};
    std::array<double, kSamples> fullMs{};
    for (size_t i = 0; i < kSamples; ++i)
    {
        // Alternate order to balance any warm-cache or clock drift.
        if (i % 2 == 0)
            fullMs[i] = MeasureEdit(*area, fullBefore, 0, 1);
        appendMs[i] = MeasureEdit(*area, appendBefore, static_cast<int>(kBytes - 1), static_cast<int>(kBytes - 1));
        if (i % 2 != 0)
            fullMs[i] = MeasureEdit(*area, fullBefore, 0, 1);
    }
    std::sort(appendMs.begin(), appendMs.end());
    std::sort(fullMs.begin(), fullMs.end());
    const double appendMedian = appendMs[kSamples / 2];
    const double fullMedian = fullMs[kSamples / 2];
    RecordProperty("append_median_ms", std::to_string(appendMedian));
    RecordProperty("full_reflow_median_ms", std::to_string(fullMedian));
    RecordProperty("samples_per_edit", static_cast<int>(kSamples));
    EXPECT_LT(appendMedian * 4.0, fullMedian)
        << "64 KiB at 3000 px, Roboto 16: append " << appendMedian
        << " ms, full reflow " << fullMedian << " ms (median of " << kSamples << ")";
#endif
}

TEST(TextAreaIncrementalWrap, AppendCostDoesNotGrowWithTheText)
{
#ifndef NDEBUG
    GTEST_SKIP() << "Measured wrapping regression requires an optimized Release build";
#else
    IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kXml, kCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();
    auto* area = dynamic_cast<TextArea*>(fixture.Element("text"));
    ASSERT_NE(area, nullptr);
    constexpr size_t kSamples = 15;
    // A text four times longer may cost at most half as much again per append;
    // an append linear in the text costs about four times as much.
    constexpr double kMaxRatio = 1.5;
    const std::string smallBefore(16u * 1024u - 1u, 'x');
    const std::string largeBefore(64u * 1024u - 1u, 'x');
    (void)MeasureAppend(*area, smallBefore);
    (void)MeasureAppend(*area, largeBefore);

    std::array<double, kSamples> smallMs{};
    std::array<double, kSamples> largeMs{};
    for (size_t i = 0; i < kSamples; ++i)
    {
        // Alternate order to balance any warm-cache or clock drift.
        if (i % 2 == 0)
            smallMs[i] = MeasureAppend(*area, smallBefore);
        largeMs[i] = MeasureAppend(*area, largeBefore);
        if (i % 2 != 0)
            smallMs[i] = MeasureAppend(*area, smallBefore);
    }
    std::sort(smallMs.begin(), smallMs.end());
    std::sort(largeMs.begin(), largeMs.end());
    const double smallMedian = smallMs[kSamples / 2];
    const double largeMedian = largeMs[kSamples / 2];
    RecordProperty("append_16k_median_ms", std::to_string(smallMedian));
    RecordProperty("append_64k_median_ms", std::to_string(largeMedian));
    RecordProperty("samples_per_size", static_cast<int>(kSamples));
    EXPECT_LT(largeMedian, smallMedian * kMaxRatio)
        << "3000 px, Roboto 16: append at 64 KiB " << largeMedian << " ms, at 16 KiB "
        << smallMedian << " ms (median of " << kSamples << ")";
#endif
}
TEST(TextAreaIncrementalWrap, EditedBreaksAndCaretBoundariesMatchAFullLayout)
{
    IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kComparisonXml, kComparisonCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();
    auto* edited = dynamic_cast<TextArea*>(fixture.Element("text"));
    auto* reference = dynamic_cast<TextArea*>(fixture.Element("reference"));
    ASSERT_NE(edited, nullptr);
    ASSERT_NE(reference, nullptr);

    const std::vector<std::string> values = {
        std::string(700, 'x'),
        "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen",
        "office AVffi AVffi AVffi AVffi AVffi AVffi AVffi AVffi AVffi AVffi AVffi AVffi",
        "one two three\nfour five six\n\nseven eight nine\n",
        "caf\xC3\xA9 cafe\xCC\x81 caf\xC3\xA9 cafe\xCC\x81 caf\xC3\xA9 cafe\xCC\x81",
    };
    for (const std::string& value : values)
    {
        SCOPED_TRACE(value);
        edited->SetValue(value);
        (void)edited->GetVisualLineCount();
        edited->SetSelection(static_cast<int>(value.size()), static_cast<int>(value.size()));
        ASSERT_TRUE(edited->OnChar('x'));
        ExpectFullLayout(*edited, *reference);

        const int end = static_cast<int>(edited->GetValue().size());
        edited->SetSelection(end, end);
        ASSERT_TRUE(edited->OnChar(0x301));
        ExpectFullLayout(*edited, *reference);
        size_t middle = edited->GetValue().size() / 2;
        while ((static_cast<unsigned char>(edited->GetValue()[middle]) & 0xC0u) == 0x80u)
            --middle;
        edited->SetSelection(static_cast<int>(middle), static_cast<int>(middle));
        ASSERT_TRUE(edited->OnChar(' '));
        ExpectFullLayout(*edited, *reference);
        edited->SetSelection(2, 2);
        ASSERT_TRUE(edited->OnChar('\n'));
        ExpectFullLayout(*edited, *reference);
        edited->SetSelection(3, 3);
        ASSERT_TRUE(edited->OnKey(GameEngine::Input::kKeyCode_Backspace, 0, nullptr));
        ExpectFullLayout(*edited, *reference);

        // Programmatic replacement can contain several disjoint edits.
        std::string replacement = edited->GetValue();
        replacement.replace(1, 1, "changed ");
        replacement += "\nlast\n";
        edited->SetValue(replacement);
        ExpectFullLayout(*edited, *reference);
        edited->SetValue("short\n");
        ExpectFullLayout(*edited, *reference);
        edited->SetValue("");
        ExpectFullLayout(*edited, *reference);
    }

    // The first strong character can change the direction of the entire run.
    edited->SetValue(std::string(160, '.'));
    (void)edited->GetVisualLineCount();
    edited->SetSelection(160, 160);
    ASSERT_TRUE(edited->OnChar(0x05D0));
    ExpectFullLayout(*edited, *reference);
}

// A retained prefix fixes the paragraph's direction and script, so a rewrapped
// tail that starts in the other direction must still be shaped as part of it.
TEST(TextAreaIncrementalWrap, MixedDirectionEditsMatchAFullLayout)
{
    IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kComparisonXml, kComparisonCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();
    auto* edited = dynamic_cast<TextArea*>(fixture.Element("text"));
    auto* reference = dynamic_cast<TextArea*>(fixture.Element("reference"));
    ASSERT_NE(edited, nullptr);
    ASSERT_NE(reference, nullptr);

    // At this width the second visual line starts with the Hebrew word, so an
    // edit on the third line rewraps a tail whose first strong character is RTL.
    const std::string value = "abc def ghi \xD7\x90\xD7\x91\xD7\x92 \xD7\x93\xD7\x94\xD7\x95 "
                              "\xD7\x96\xD7\x97\xD7\x98 jkl mno pqr";
    // Delete each code point in turn, and separately insert a character before it.
    for (size_t byte = 0; byte < value.size(); ++byte)
    {
        if ((static_cast<unsigned char>(value[byte]) & 0xC0u) == 0x80u)
            continue;
        size_t next = byte + 1;
        while (next < value.size() && (static_cast<unsigned char>(value[next]) & 0xC0u) == 0x80u)
            ++next;
        SCOPED_TRACE(testing::Message() << "byte " << byte);
        edited->SetValue(value);
        (void)edited->GetVisualLineCount();
        edited->SetSelection(static_cast<int>(next), static_cast<int>(next));
        ASSERT_TRUE(edited->OnKey(GameEngine::Input::kKeyCode_Backspace, 0, nullptr));
        ExpectFullLayout(*edited, *reference);
        edited->SetValue(value);
        (void)edited->GetVisualLineCount();
        edited->SetSelection(static_cast<int>(byte), static_cast<int>(byte));
        ASSERT_TRUE(edited->OnChar(value[byte] == ' ' ? 'x' : ' '));
        ExpectFullLayout(*edited, *reference);
    }
}

// Under break-word a long word wraps inside a shaping cluster: a full layout can
// start a line at the last byte of a ligature or at a combining mark, so a
// rewrap starts there too. Kerning alone never spans a cluster: control value.
TEST(TextAreaIncrementalWrap, ClusterBoundaryEditsMatchAFullLayout)
{
    IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kComparisonXml, kComparisonCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();
    auto* edited = dynamic_cast<TextArea*>(fixture.Element("text"));
    auto* reference = dynamic_cast<TextArea*>(fixture.Element("reference"));
    ASSERT_NE(edited, nullptr);
    ASSERT_NE(reference, nullptr);
    std::string ligatures;
    std::string kerned;
    std::string marks;
    for (int i = 0; i < 30; ++i)
    {
        ligatures += "ffi";
        kerned += "AVTo";
        marks += "e\xCC\x81";
    }
    const std::string mixed = "word " + ligatures + " " + kerned;
    for (const std::string& value : {ligatures, kerned, marks, mixed})
    {
        // Delete each code point in turn, and separately insert a character before it.
        for (size_t byte = 0; byte < value.size(); ++byte)
        {
            if ((static_cast<unsigned char>(value[byte]) & 0xC0u) == 0x80u)
                continue;
            size_t next = byte + 1;
            while (next < value.size() && (static_cast<unsigned char>(value[next]) & 0xC0u) == 0x80u)
                ++next;
            SCOPED_TRACE(testing::Message() << value.substr(0, 12) << " byte " << byte);
            edited->SetValue(value);
            (void)edited->GetVisualLineCount();
            edited->SetSelection(static_cast<int>(next), static_cast<int>(next));
            ASSERT_TRUE(edited->OnKey(GameEngine::Input::kKeyCode_Backspace, 0, nullptr));
            ExpectFullLayout(*edited, *reference);
            edited->SetValue(value);
            (void)edited->GetVisualLineCount();
            edited->SetSelection(static_cast<int>(byte), static_cast<int>(byte));
            ASSERT_TRUE(edited->OnChar(value[byte] == 'x' ? 'y' : 'x'));
            ExpectFullLayout(*edited, *reference);
        }
    }
}

// A tail's caret map is the whole string's from its first byte on, even when that
// byte sits deep inside a cluster: here up to the last of ten combining marks on
// one base, further back than the first shaping's lookback.
TEST(TextAreaIncrementalWrap, TailCaretMapMatchesTheWholeStringInsideALongCluster)
{
    auto atlas = GameEngine::UITesting::LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";
    constexpr float kPixelSize = 16.0f;
    constexpr float kCaretEpsPx = 1e-3f;
    std::string text = "ab";
    for (int cluster = 0; cluster < 3; ++cluster)
    {
        text += "e";
        for (int mark = 0; mark < 10; ++mark)
            text += "\xCC\x81";
    }
    text += "cd";
    std::vector<float> whole;
    ASSERT_TRUE(atlas->BuildCaretMapUtf8(text, kPixelSize, whole));
    for (size_t first = 1; first < text.size(); ++first)
    {
        if ((static_cast<unsigned char>(text[first]) & 0xC0u) == 0x80u)
            continue;
        std::vector<float> tail;
        ASSERT_TRUE(atlas->BuildCaretMapUtf8(text, kPixelSize, tail, 0.0f, first));
        ASSERT_EQ(tail.size(), text.size() - first + 1);
        for (size_t k = 0; k < tail.size(); ++k)
            EXPECT_NEAR(tail[k], whole[first + k] - whole[first], kCaretEpsPx)
                << "first byte " << first << ", byte " << first + k;
    }
}

TEST(TextAreaIncrementalWrap, WidthAndFontMetricsInvalidateRetainedLines)
{
    IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kComparisonXml, kComparisonCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();
    auto* edited = dynamic_cast<TextArea*>(fixture.Element("text"));
    auto* reference = dynamic_cast<TextArea*>(fixture.Element("reference"));
    ASSERT_NE(edited, nullptr);
    ASSERT_NE(reference, nullptr);
    edited->SetValue("one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen");
    (void)edited->GetVisualLineCount();
    for (const float width : {51.0f, 137.0f, 83.0f})
    {
        edited->Overrides().Set(GameEngine::Style::Width, GameEngine::StyleLength::Px(width));
        reference->Overrides().Set(GameEngine::Style::Width, GameEngine::StyleLength::Px(width));
        edited->MarkDirty(GameEngine::UIElement::StyleDirty | GameEngine::UIElement::LayoutDirty);
        reference->MarkDirty(GameEngine::UIElement::StyleDirty | GameEngine::UIElement::LayoutDirty);
        fixture.Settle();
        ExpectFullLayout(*edited, *reference);
    }
    edited->Overrides().Set(GameEngine::Style::FontSize, GameEngine::StyleLength::Px(23.0f));
    reference->Overrides().Set(GameEngine::Style::FontSize, GameEngine::StyleLength::Px(23.0f));
    edited->Overrides().Set(GameEngine::Style::LetterSpacing, 1.0f);
    reference->Overrides().Set(GameEngine::Style::LetterSpacing, 1.0f);
    edited->MarkDirty(GameEngine::UIElement::StyleDirty | GameEngine::UIElement::LayoutDirty);
    reference->MarkDirty(GameEngine::UIElement::StyleDirty | GameEngine::UIElement::LayoutDirty);
    fixture.Settle();
    ExpectFullLayout(*edited, *reference);
}
