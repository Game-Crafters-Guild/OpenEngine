// TextInput::SetMasked: the field draws one U+2022 per code point, the value stays the typed
// text, copy and cut put nothing on the clipboard, and the caret map the render path and the
// pointer read is indexed by the value's bytes. Every value here is a throwaway.

#include "FixedScalePlatform.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Rendering/Core/Device.h"
#include "UI/Controls/TextField.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace GameEngine;

namespace
{
constexpr const char* kBullet = "\xE2\x80\xA2";

std::string Bullets(size_t count)
{
    std::string text;
    for (size_t index = 0; index < count; ++index)
        text += kBullet;
    return text;
}

void Type(TextInput& input, std::string_view text)
{
    for (char c : text)
        input.OnChar(static_cast<unsigned char>(c));
}

constexpr int kPrimaryModifier =
#if defined(__APPLE__)
    Input::kModSuper;
#else
    Input::kModControl;
#endif
} // namespace

TEST(MaskedTextInputTests, TheDrawnTextIsOneBulletPerCodePointAndTheValueIsTheTypedText)
{
    TextInput input;
    input.SetMasked(true);
    Type(input, "abc");

    EXPECT_EQ(input.GetValue(), "abc");
    EXPECT_EQ(input.GetTextContent(), Bullets(3));
    EXPECT_EQ(input.GetCaretIndex(), 3);
    ITextMeasurable::TextMeasureInfo info;
    input.GetTextMeasureInfo(info, ResolvedStyle{});
    EXPECT_EQ(info.Text, Bullets(3)) << "layout measures the bullets, not the value";

    // A two-byte and a four-byte code point are one bullet each.
    input.SetValue("a\xC3\xA9\xF0\x9F\x94\x91");
    EXPECT_EQ(input.GetTextContent(), Bullets(3));
}

TEST(MaskedTextInputTests, UnmaskingShowsTheValueAgain)
{
    TextInput input;
    input.SetValue("throwaway");
    input.SetMasked(true);
    EXPECT_EQ(input.GetTextContent(), Bullets(9));
    input.SetMasked(false);
    EXPECT_EQ(input.GetTextContent(), "throwaway");
}

TEST(MaskedTextInputTests, CopyAndCutPutNothingOnTheClipboardAndPasteWorks)
{
    UITesting::FixedScalePlatform platform(1.0f);
    platform.SetClipboardText("before");
    TextInput input;
    input.SetMasked(true);
    input.OnFocusChanged(true);
    Type(input, "throwaway");

    input.OnKey(Input::kKeyCode_A, kPrimaryModifier, &platform);
    input.OnKey(Input::kKeyCode_C, kPrimaryModifier, &platform);
    EXPECT_EQ(platform.GetClipboardText(), "before");
    input.OnKey(Input::kKeyCode_X, kPrimaryModifier, &platform);
    EXPECT_EQ(platform.GetClipboardText(), "before");
    EXPECT_EQ(input.GetValue(), "throwaway") << "cut removes nothing it cannot put on the clipboard";

    platform.SetClipboardText("-pasted");
    input.OnKey(Input::kKeyCode_End, 0, &platform);
    input.OnKey(Input::kKeyCode_V, kPrimaryModifier, &platform);
    EXPECT_EQ(input.GetValue(), "throwaway-pasted");
    EXPECT_EQ(input.GetTextContent(), Bullets(16));
}

// A word step would show where the spaces of a masked value are, so the value is one word.
TEST(MaskedTextInputTests, AWordStepMovesTheCaretToTheStartOrTheEnd)
{
    UITesting::FixedScalePlatform platform(1.0f);
    TextInput input;
    input.SetMasked(true);
    input.OnFocusChanged(true);
    Type(input, "two words here");
    const int end = static_cast<int>(input.GetValue().size());

    input.OnKey(Input::kKeyCode_Home, 0, &platform);
    for (int step = 0; step < 5; ++step)
        input.OnKey(Input::kKeyCode_Right, 0, &platform);
    input.OnKey(Input::kKeyCode_Right, kPrimaryModifier, &platform);
    EXPECT_EQ(input.GetCaretIndex(), end);

    for (int step = 0; step < 5; ++step)
        input.OnKey(Input::kKeyCode_Left, 0, &platform);
    input.OnKey(Input::kKeyCode_Left, kPrimaryModifier, &platform);
    EXPECT_EQ(input.GetCaretIndex(), 0);
}

TEST(MaskedTextInputTests, TheMaskedAttributeIsParsedOnATextField)
{
    UIRegistration::RegisterBuiltInControls();
    const char* xml = R"(
<uielement id="root">
  <TextField id="key" masked="true" />
  <TextField id="plain" />
</uielement>
)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(UIParsing::XMLParser::ParseLayoutFromString(xml, root));
    auto* key = dynamic_cast<TextField*>(root->FindById("key"_sid));
    auto* plain = dynamic_cast<TextField*>(root->FindById("plain"_sid));
    ASSERT_NE(key, nullptr);
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE(key->IsMasked());
    EXPECT_FALSE(plain->IsMasked());
    key->SetValue("throwaway");
    TextInput* input = nullptr;
    for (const auto& child : key->GetChildren())
        input = input ? input : child->GetAsTextInput();
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->GetTextContent(), Bullets(9));
}

// The caret map is indexed by the value's bytes, at the bullets' positions: the caret after a
// two-byte code point sits after one bullet, and the end of the value after all of them.
TEST(MaskedTextInputTests, TheCaretMapPlacesEachValueOffsetAtItsBullet)
{
    SharedDeviceReleaseRetirement releaseRetirement;
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    UIRegistration::RegisterBuiltInControls();
    UIManager ui(device);
    // Update returns early without a root; with one, it loads the staged default font.
    ui.SetRoot(std::make_unique<UIElement>());
    ui.Update(0.0f);
    Rendering::Text::FontAtlas* font = ui.GetDefaultFontAtlas();
    if (!font)
        GTEST_SKIP() << "No default font";

    TextInput input;
    input.SetMasked(true);
    input.SetValue("a\xC3\xA9z");
    constexpr float kPixelSize = 14.0f;
    const auto& measure = input.EnsureMeasureCache(font, kPixelSize, 0.0f);
    const auto& bullets = font->MeasureText(Bullets(3), kPixelSize, 0.0f);

    ASSERT_EQ(measure.caretXByByte.size(), 5u) << "one entry per value byte offset, end included";
    ASSERT_EQ(bullets.caretXByByte.size(), 10u);
    EXPECT_FLOAT_EQ(measure.caretXByByte[0], 0.0f);
    EXPECT_FLOAT_EQ(measure.caretXByByte[1], bullets.caretXByByte[3]);
    EXPECT_FLOAT_EQ(measure.caretXByByte[3], bullets.caretXByByte[6]);
    EXPECT_FLOAT_EQ(measure.caretXByByte[4], bullets.caretXByByte[9]);
    EXPECT_FLOAT_EQ(measure.metrics.width, bullets.metrics.width);
}
