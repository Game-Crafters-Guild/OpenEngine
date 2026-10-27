// The Cube LUT preview's enlarged view: a click on the inspector's preview opens it in the window,
// with the same bands at the enlarged size; Escape (the manager's topmost dismissal) closes it, and
// so does a click on it. The view goes with the preview it enlarges.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "Inspectors/CubeLut/CubeLutPreview.h"
#include "Inspectors/CubeLut/CubeLutPreviewPopover.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <memory>
#include <string>
#include <vector>

using GameEngine::CubeLutPreview;
using GameEngine::CubeLutPreviewPopover;
using GameEngine::CubeLutPreviewSize;
using GameEngine::CubeLutStripRequest;
using GameEngine::GUID;
using GameEngine::UIElement;
using GameEngine::UIEvent;
using InspectorLayoutTesting::InspectorLayoutFixture;

namespace
{

// A press and release on one element: the click the preview and the enlarged view listen for.
void Click(UIElement& element)
{
    for (const auto id : {GameEngine::kEventMouseDown, GameEngine::kEventMouseUp})
    {
        UIEvent e{};
        e.Id = id;
        e.Target = &element;
        e.CurrentTarget = &element;
        element.DispatchEvent(e);
    }
}

size_t CountByClass(const UIElement* root, const std::string& className)
{
    if (!root)
        return 0;
    size_t count = root->HasClass(className) ? 1u : 0u;
    for (const auto& child : root->GetChildren())
        count += CountByClass(child.get(), className);
    return count;
}

void CollectByClass(UIElement* root, const std::string& className, std::vector<UIElement*>& out)
{
    if (!root)
        return;
    if (root->HasClass(className))
        out.push_back(root);
    for (const auto& child : root->GetChildren())
        CollectByClass(child.get(), className, out);
}

} // namespace

TEST(CubeLutPreviewPopover, AClickOpensTheEnlargedViewAndAClickOrEscapeClosesIt)
{
    auto root = std::make_unique<UIElement>();
    UIElement* field = root.get();
    CubeLutStripRequest request{};
    request.Lut = GUID("33333333-3333-3333-3333-333333333333");
    request.Owner = 5;
    request.Intensity = 0.8f;
    const std::shared_ptr<CubeLutPreview> preview = CubeLutPreview::Build(
        field, request, true, CubeLutPreviewSize::Inspector, [](CubeLutPreview& p) { p.OpenEnlarged(); });
    ASSERT_NE(preview, nullptr);

    InspectorLayoutFixture fixture;
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;
    UIElement* window = fixture.Ui->GetRootElement();
    EXPECT_EQ(CountByClass(window, "cube-lut-strip-band"), 4u) << "the ramp and the probes, each with and without the LUT";

    Click(*preview->GetRoot());
    CubeLutPreviewPopover* enlarged = preview->GetEnlarged();
    ASSERT_NE(enlarged, nullptr) << "a click on the preview opens the enlarged view";
    EXPECT_TRUE(enlarged->IsPopupOpen());
    EXPECT_EQ(enlarged->GetParent(), window) << "the enlarged view sits in the window, above the inspector";
    EXPECT_EQ(CountByClass(enlarged, "cube-lut-preview-enlarged"), 1u);
    EXPECT_EQ(CountByClass(enlarged, "cube-lut-strip-band"), 4u);

    Click(*preview->GetRoot());
    EXPECT_EQ(preview->GetEnlarged(), enlarged) << "a second click keeps the one open view";

    EXPECT_TRUE(fixture.Ui->DismissTopmostPopup()) << "Escape reaches the open view";
    EXPECT_FALSE(enlarged->IsPopupOpen());
    EXPECT_EQ(preview->GetEnlarged(), nullptr);

    Click(*preview->GetRoot());
    enlarged = preview->GetEnlarged();
    ASSERT_NE(enlarged, nullptr) << "the view opens again after it closed";
    Click(*enlarged->GetPanel());
    EXPECT_FALSE(enlarged->IsPopupOpen()) << "a click on the enlarged view closes it";
    EXPECT_EQ(preview->GetEnlarged(), nullptr);
}

// The enlarged view goes with the preview it enlarges (an inspector rebuild destroys the preview),
// and a view destroyed without closing (its window torn down first) leaves the preview pointing at
// nothing.
TEST(CubeLutPreviewPopover, TheViewAndItsPreviewNeverOutliveEachOther)
{
    auto root = std::make_unique<UIElement>();
    UIElement* field = root.get();
    CubeLutStripRequest request{};
    request.Lut = GUID("33333333-3333-3333-3333-333333333333");
    request.Owner = 5;
    std::shared_ptr<CubeLutPreview> preview = CubeLutPreview::Build(
        field, request, true, CubeLutPreviewSize::Inspector, [](CubeLutPreview& p) { p.OpenEnlarged(); });
    ASSERT_NE(preview, nullptr);
    InspectorLayoutFixture fixture;
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;
    UIElement* window = fixture.Ui->GetRootElement();

    // A popover destroyed without Close.
    Click(*preview->GetRoot());
    CubeLutPreviewPopover* enlarged = preview->GetEnlarged();
    ASSERT_NE(enlarged, nullptr);
    std::unique_ptr<UIElement> taken = window->TakeChild(enlarged);
    ASSERT_NE(taken, nullptr);
    taken.reset();
    EXPECT_EQ(preview->GetEnlarged(), nullptr) << "the destroyed view cleared the preview's pointer";

    // The preview destroyed while its view is open: the view closes.
    Click(*preview->GetRoot());
    enlarged = preview->GetEnlarged();
    ASSERT_NE(enlarged, nullptr);
    ASSERT_TRUE(enlarged->IsPopupOpen());
    UIElement* previewRoot = preview->GetRoot();
    preview.reset(); // the elements' handlers hold the last reference
    field->RemoveChild(previewRoot);
    EXPECT_FALSE(enlarged->IsPopupOpen()) << "the view closed with the preview it enlarges";
}

// Each band stretches its strip over the band's box, so a 512-texel strip in a narrow inspector
// field is drawn at the band's height (12 and 14 px) and not fitted to the strip's aspect, which
// drew it 4 px tall; a change of intensity rebinds the strip and keeps the stretch.
TEST(CubeLutPreviewPopover, TheBandsStretchTheirStripsOverTheirBoxes)
{
    auto root = std::make_unique<UIElement>();
    UIElement* field = root.get();
    CubeLutStripRequest request{};
    request.Lut = GUID("33333333-3333-3333-3333-333333333333");
    request.Owner = 5;
    const std::shared_ptr<CubeLutPreview> preview =
        CubeLutPreview::Build(field, request, true, CubeLutPreviewSize::Inspector, nullptr);
    ASSERT_NE(preview, nullptr);
    InspectorLayoutFixture fixture;
    fixture.ExtraSheets = {"Assets/UI/controls/CubeLutPreview/CubeLutPreview.css"};
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;
    preview->SetIntensity(0.5f);
    fixture.Ui->Update(1.0f / 60.0f, true);

    std::vector<UIElement*> bands;
    CollectByClass(fixture.Ui->GetRootElement(), "cube-lut-strip-band", bands);
    ASSERT_EQ(bands.size(), 4u);
    for (const UIElement* band : bands)
    {
        const GameEngine::BackgroundImageStyle& image = band->GetResolvedStyle().Visual.BackgroundImage;
        EXPECT_EQ(image.SizeMode, GameEngine::BackgroundSizeMode::Explicit);
        EXPECT_TRUE(image.SizeXIsPercent && image.SizeYIsPercent);
        EXPECT_FLOAT_EQ(image.SizeX, 100.0f);
        EXPECT_FLOAT_EQ(image.SizeY, 100.0f);
        EXPECT_FLOAT_EQ(band->GetLayoutHeight(), band->HasClass("cube-lut-band-ramp") ? 12.0f : 14.0f);
    }
}
