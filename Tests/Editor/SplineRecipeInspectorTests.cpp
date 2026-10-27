// The spline recipe inspectors: a notice with the recipe's build state and the reflected rows.

#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Hierarchy.h"
#include "Components/Spline/SplineFence.h"
#include "Components/Spline/SplineFenceSpanPiece.h"
#include "Components/Spline/SplinePlacement.h"
#include "Components/Spline/SplineWall.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "InspectorLayoutFixture.h"
#include "InspectorRegistry.h"
#include "RobotoTestFont.h"
#include "Inspectors/SplineFence/SplineFenceInspector.h"
#include "Inspectors/SplineFence/SplineFenceInspectorState.h"
#include "Inspectors/SplineFence/SplineFenceOverrideFields.h"
#include "Inspectors/SplineFence/SplineFencePreview.h"
#include "Inspectors/Spline/SplineInspector.h"
#include "Inspectors/SplineRecipe/SplineRecipePreviewCanvas.h"
#include "Inspectors/SplineRecipe/SplineRecipeStateNotice.h"
#include "Inspectors/Spline/SplinePlacementInspector.h"
#include "Scene/SceneIO.h"
#include "SplineRecipes/FenceRecipeLayout.h"
#include "Types/StringUtils.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"
#include "UndoRedo/UndoRedoService.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

using InspectorLayoutTesting::FindByClass;

constexpr const char* kPendingText = "Not built yet / It builds once its inputs stop changing.";

// Every label text under `element`, in tree order: row labels and the shown option of each dropdown.
void CollectTexts(UIElement& element, std::vector<std::string>& texts)
{
    if (const auto* label = dynamic_cast<const Label*>(&element))
        texts.push_back(label->GetText());
    for (const auto& child : element.GetChildren())
        CollectTexts(*child, texts);
}

bool Shows(UIElement& element, const std::string& text)
{
    std::vector<std::string> texts;
    CollectTexts(element, texts);
    return std::find(texts.begin(), texts.end(), text) != texts.end();
}

// The row whose label reads `label`.
UIElement* FindRow(UIElement& element, const std::string& label)
{
    if (element.HasClass("inspector-row") && Shows(element, label))
        return &element;
    for (const auto& child : element.GetChildren())
        if (UIElement* found = FindRow(*child, label))
            return found;
    return nullptr;
}

template <typename T>
void CollectOfType(UIElement& element, std::vector<T*>& found)
{
    if (auto* typed = dynamic_cast<T*>(&element))
        found.push_back(typed);
    for (const auto& child : element.GetChildren())
        CollectOfType(*child, found);
}

// The rows with `className` under `element`, in tree order.
void CollectWithClass(UIElement& element, const char* className, std::vector<UIElement*>& found)
{
    if (element.HasClass(className))
        found.push_back(&element);
    for (const auto& child : element.GetChildren())
        CollectWithClass(*child, className, found);
}

// The text of the label at the left of a row.
std::string RowLabel(UIElement& row)
{
    std::vector<std::string> texts;
    CollectTexts(row, texts);
    return texts.empty() ? std::string() : texts.front();
}

void Click(Button& button)
{
    UIEvent event{};
    event.Id = kEventButtonClick;
    event.Target = &button;
    event.CurrentTarget = &button;
    button.DispatchEvent(event);
}

Components::ModelRef TestMesh(uint8_t tag)
{
    GUID::Data data{};
    data[0] = tag;
    data[15] = 0x5A;
    return Components::ModelRef(GUID(data));
}

UIElement* FindById(UIElement& element, const std::string& id)
{
    if (element.GetId() == id)
        return &element;
    for (const auto& child : element.GetChildren())
        if (UIElement* found = FindById(*child, id))
            return found;
    return nullptr;
}

// What a fence preview strip emits, sorted by the colors its stylesheet gives each mark.
struct PreviewCount
{
    // The distinct x of the post lines, left to right, and the dashes they draw with.
    std::vector<float> StationXs;
    std::size_t Stations = 0;
    std::size_t StationDashes = 0;
    // The span and gate boxes along the run, one seam each, in emission order: true for a gate.
    std::vector<bool> SpansAreGates;
    std::size_t CrestBoxes = 0;
    std::size_t GateLines = 0;
    std::vector<UI::UIPrimitive> SpanTriangles;
    // The left edge of the leftmost part, in pixels.
    float PartsLeft = std::numeric_limits<float>::max();
    std::vector<UI::UIPrimitive> GroundLines;
    std::vector<UI::UIPrimitive> LabelLines;
    // The lines of the parts below the ground, and the draw order of the last of them and of the
    // first ground line.
    std::vector<UI::UIPrimitive> BuriedLines;
    std::size_t LastBuriedLine = 0;
    std::size_t FirstGroundLine = std::numeric_limits<std::size_t>::max();
    // The post, span, gate and crest triangles, and the glyphs of the text the strip draws.
    std::vector<UI::UIPrimitive> PartTriangles;
    std::vector<UI::UIPrimitive> Glyphs;
};

// The strip's text draws only with `font`; with none the strip draws its marks and parts alone.
PreviewCount CountPreview(EditorUI::SplineFencePreview& preview, Rendering::Text::FontAtlas* font = nullptr,
                          UI::UITextureRegistry* textures = nullptr)
{
    std::vector<UI::UIPrimitive> primitives;
    UI::PrimitiveEmitContext context{primitives, 0, 1.0f};
    context.FontAtlas = font;
    context.Textures = textures;
    const ResolvedStyle& style = preview.GetResolvedStyle();
    preview.OnGeneratePrimitives(context, style, 0.0f, 0.0f, preview.GetLayoutWidth(), preview.GetLayoutHeight());
    const auto color = [&style](const char* name) { return EditorUI::RecipePreviewColor(style, HashStringId(name)); };
    const uint32_t station = color("--spline-preview-station");
    const uint32_t post = color("--spline-preview-post");
    const uint32_t span = color("--spline-preview-span");
    const uint32_t gate = color("--spline-preview-gate");
    const uint32_t gateFill = color("--spline-preview-gate-fill");
    const uint32_t crest = color("--spline-preview-crest");
    const uint32_t seam = color("--spline-preview-seam");
    const uint32_t ground = color("--spline-preview-ground-line");
    const uint32_t buried = color("--spline-preview-buried");
    const uint32_t label = UI::PackFromARGB(style.GetCustomColor(HashStringId("--spline-preview-label")).value_or(0u));
    PreviewCount count;
    std::size_t crestTriangles = 0;
    bool lastFillIsGate = false;
    for (std::size_t index = 0; index < primitives.size(); ++index)
    {
        const UI::UIPrimitive& primitive = primitives[index];
        const UI::PrimitiveMode mode = UI::GetMode(primitive.ModeAndFlags);
        if (mode == UI::PrimitiveMode::Line)
        {
            if (primitive.FillColor == station)
            {
                ++count.StationDashes;
                const float x = primitive.UvRect[0];
                if (std::none_of(count.StationXs.begin(), count.StationXs.end(),
                                 [x](float known) { return std::abs(known - x) < 0.5f; }))
                    count.StationXs.push_back(x);
            }
            if (primitive.FillColor == seam)
                count.SpansAreGates.push_back(lastFillIsGate);
            if (primitive.FillColor == gate)
                ++count.GateLines;
            if (primitive.FillColor == ground)
            {
                count.GroundLines.push_back(primitive);
                count.FirstGroundLine = std::min(count.FirstGroundLine, index);
            }
            if (primitive.FillColor == buried)
            {
                count.BuriedLines.push_back(primitive);
                count.LastBuriedLine = index;
            }
            if (primitive.FillColor == label)
                count.LabelLines.push_back(primitive);
            continue;
        }
        if (mode == UI::PrimitiveMode::Slug)
            count.Glyphs.push_back(primitive);
        if (mode != UI::PrimitiveMode::Triangle)
            continue;
        if (primitive.FillColor == span || primitive.FillColor == gateFill)
        {
            lastFillIsGate = primitive.FillColor == gateFill;
            count.SpanTriangles.push_back(primitive);
        }
        if (primitive.FillColor == crest)
            ++crestTriangles;
        if (primitive.FillColor == post || primitive.FillColor == span || primitive.FillColor == gateFill ||
            primitive.FillColor == crest)
            count.PartTriangles.push_back(primitive);
        if (primitive.FillColor == post || primitive.FillColor == span || primitive.FillColor == gateFill ||
            primitive.FillColor == crest)
            count.PartsLeft = std::min({count.PartsLeft, primitive.UvRect[0], primitive.UvRect[2], primitive.BorderWidths[0]});
    }
    std::sort(count.StationXs.begin(), count.StationXs.end());
    count.Stations = count.StationXs.size();
    count.CrestBoxes = crestTriangles / 4;
    return count;
}

// The height in pixels of the ground surface line at `x`; the lowest pixel row when no line covers it.
float GroundLineY(const std::vector<UI::UIPrimitive>& groundLines, float x)
{
    for (const UI::UIPrimitive& line : groundLines)
    {
        const float x0 = line.UvRect[0];
        const float x1 = line.UvRect[2];
        if (x < std::min(x0, x1) - 0.5f || x > std::max(x0, x1) + 0.5f || x0 == x1)
            continue;
        const float t = std::clamp((x - x0) / (x1 - x0), 0.0f, 1.0f);
        return line.UvRect[1] + t * (line.UvRect[3] - line.UvRect[1]);
    }
    return std::numeric_limits<float>::max();
}

// A pixel rect: left, top, right, bottom.
struct PixelRect
{
    float Left = 0.0f;
    float Top = 0.0f;
    float Right = 0.0f;
    float Bottom = 0.0f;

    bool Overlaps(const PixelRect& other, float inset) const
    {
        return Left + inset < other.Right && other.Left + inset < Right && Top + inset < other.Bottom &&
               other.Top + inset < Bottom;
    }
};

PixelRect GlyphRect(const UI::UIPrimitive& glyph)
{
    return {glyph.X, glyph.Y, glyph.X + glyph.W, glyph.Y + glyph.H};
}

PixelRect TriangleRect(const UI::UIPrimitive& triangle)
{
    const Mathematics::Vector2 corners[3] = {{triangle.UvRect[0], triangle.UvRect[1]},
                                             {triangle.UvRect[2], triangle.UvRect[3]},
                                             {triangle.BorderWidths[0], triangle.BorderWidths[1]}};
    PixelRect rect{corners[0].x, corners[0].y, corners[0].x, corners[0].y};
    for (const Mathematics::Vector2& corner : corners)
        rect = {std::min(rect.Left, corner.x), std::min(rect.Top, corner.y), std::max(rect.Right, corner.x),
                std::max(rect.Bottom, corner.y)};
    return rect;
}

// Where the strip drew `text` in 12 px type: the rect of its glyphs, found as a run of glyphs with the
// shapes and spacing that `font` gives the text. Empty when the strip did not draw it.
std::optional<PixelRect> FindText(const std::vector<UI::UIPrimitive>& glyphs, std::string_view text,
                                  Rendering::Text::FontAtlas& font, UI::UITextureRegistry& textures)
{
    std::vector<UI::UIPrimitive> expected;
    UI::PrimitiveEmitContext context{expected, 0, 1.0f};
    context.Textures = &textures;
    context.EmitText(text, 0.0f, 0.0f, 12.0f, 0xFFFFFFFFu, &font);
    if (expected.empty() || expected.size() > glyphs.size())
        return std::nullopt;
    const auto same = [](float a, float b) { return std::abs(a - b) < 0.05f; };
    for (std::size_t first = 0; first + expected.size() <= glyphs.size(); ++first)
    {
        const float dx = glyphs[first].X - expected[0].X;
        const float dy = glyphs[first].Y - expected[0].Y;
        bool match = true;
        for (std::size_t k = 0; k < expected.size() && match; ++k)
        {
            const UI::UIPrimitive& drawn = glyphs[first + k];
            match = same(drawn.X, expected[k].X + dx) && same(drawn.Y, expected[k].Y + dy) &&
                    same(drawn.W, expected[k].W) && same(drawn.H, expected[k].H);
        }
        if (!match)
            continue;
        PixelRect rect = GlyphRect(glyphs[first]);
        for (std::size_t k = 1; k < expected.size(); ++k)
        {
            const PixelRect glyph = GlyphRect(glyphs[first + k]);
            rect = {std::min(rect.Left, glyph.Left), std::min(rect.Top, glyph.Top), std::max(rect.Right, glyph.Right),
                    std::max(rect.Bottom, glyph.Bottom)};
        }
        return rect;
    }
    return std::nullopt;
}

class SplineRecipeInspectorTest : public ::testing::Test
{
protected:
    // The recipe rows include asset pickers, which read the engine's asset registry.
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    // The registration order of the editor: the fence and placement inspectors replace any
    // inspector registered before them for their components.
    template <typename RecipeT>
    void Build(const RecipeT& recipe = RecipeT{})
    {
        RegisterSplineWallInspector();
        RegisterSplineExtrudeInspector();
        RegisterSplinePlacementInspector();
        RegisterSplineFenceInspector();
        entity = world.Create().GetHandle();
        world.AddComponentImmediate(entity, Components::SplineComponent{});
        world.AddComponentImmediate(entity, recipe);
        world.AddComponentImmediate(entity, Components::WorldTransform{});
        BuildInspectorOf<RecipeT>(entity);
    }

    template <typename RecipeT>
    void BuildInspectorOf(ECS::EntityHandle recipeEntity)
    {
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<RecipeT>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext context;
        context.Parent = &parent;
        context.World = &world;
        context.Entity = recipeEntity;
        (*inspector)(context);
    }

    // Builds the fence inspector again from the world, with the undo service, as a commit does.
    void RebuildFence()
    {
        parent.RemoveAllChildren();
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Components::SplineFence>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext context;
        context.Parent = &parent;
        context.World = &world;
        context.Entity = entity;
        context.Undo = &undo;
        (*inspector)(context);
    }

    // Builds the fence inspector again for a fence whose last build is Built, as the recipe system
    // reports it once the fence is placed.
    // `measured` are the part bounds of that build.
    void RebuildBuiltFence(std::span<const SplineRecipes::FenceMeshBounds> measured = {})
    {
        parent.RemoveAllChildren();
        InspectorContext context;
        context.Parent = &parent;
        context.World = &world;
        context.Entity = entity;
        context.Undo = &undo;
        BuildSplineFenceInspectorInState(context, SplineRecipes::BuildState::Built, {}, measured);
    }

    // A span piece that the fence placed in span `ordinal` of the segment from `point`.
    void PlaceSpan(uint32_t point, uint32_t ordinal)
    {
        const ECS::EntityHandle piece = world.Create().GetHandle();
        Components::SplineFenceSpanPiece span{};
        span.Run = point;
        span.OrdinalInRun = ordinal;
        world.AddComponentImmediate(piece, span);
        world.AddComponentImmediate(piece, Components::Parent{entity});
    }

    void BuildFenceWithUndo(const Components::SplineFence& fence)
    {
        Build(fence);
        RebuildFence();
    }

    const Components::SplineFence& Fence() { return *world.GetComponent<Components::SplineFence>(entity); }

    // The mesh that override 0 of the fence draws, or a null mesh.
    Components::ModelRef OverrideMesh()
    {
        const SplineRecipes::FencePool pool = SplineRecipes::FencePoolOf(Fence(), Components::SplineFencePartKind::Span);
        const uint8_t slot = Fence().Overrides[0].PoolSlot;
        return slot < pool.Count ? pool.Meshes[slot] : Components::ModelRef{};
    }

    std::vector<UIElement*> PartRows()
    {
        std::vector<UIElement*> rows;
        CollectWithClass(parent, "spline-fence-part-row", rows);
        return rows;
    }

    Button* AddPartButton() { return dynamic_cast<Button*>(FindById(parent, "spline-fence-add-part")); }

    // The list action `className` of part row `row`, or null.
    Button* RowAction(std::size_t row, const char* className)
    {
        const std::vector<UIElement*> rows = PartRows();
        if (row >= rows.size())
            return nullptr;
        std::vector<UIElement*> actions;
        CollectWithClass(*rows[row], className, actions);
        return actions.empty() ? nullptr : dynamic_cast<Button*>(actions.front());
    }

    Dropdown* RowKind(std::size_t row)
    {
        const std::vector<UIElement*> rows = PartRows();
        if (row >= rows.size())
            return nullptr;
        std::vector<Dropdown*> dropdowns;
        CollectOfType(*rows[row], dropdowns);
        return dropdowns.empty() ? nullptr : dropdowns.front();
    }

    std::vector<UIElement*> OverrideRows()
    {
        std::vector<UIElement*> rows;
        CollectWithClass(parent, "spline-fence-override-row", rows);
        return rows;
    }

    Button* AddOverrideButton() { return dynamic_cast<Button*>(FindById(parent, "spline-fence-add-override")); }

    // The list action `className` of override row `row`, or null.
    Button* OverrideAction(std::size_t row, const char* className)
    {
        const std::vector<UIElement*> rows = OverrideRows();
        if (row >= rows.size())
            return nullptr;
        std::vector<UIElement*> actions;
        CollectWithClass(*rows[row], className, actions);
        return actions.empty() ? nullptr : dynamic_cast<Button*>(actions.front());
    }

    // The fields of type T in override row `row`, in tree order.
    template <typename T>
    std::vector<T*> OverrideFields(std::size_t row)
    {
        std::vector<T*> fields;
        const std::vector<UIElement*> rows = OverrideRows();
        if (row < rows.size())
            CollectOfType(*rows[row], fields);
        return fields;
    }

    // True when the text of a label under the inspector holds `part`.
    bool ShowsTextWith(const std::string& part)
    {
        std::vector<std::string> texts;
        CollectTexts(parent, texts);
        return std::any_of(texts.begin(), texts.end(),
                           [&part](const std::string& text) { return text.find(part) != std::string::npos; });
    }

    // The title and text of the build state notice, or "(none)" when the inspector shows none.
    std::string StateText()
    {
        UIElement* notice = FindById(parent, "spline-recipe-build-state");
        const auto* title = notice ? dynamic_cast<const Label*>(FindByClass(notice, "inspector-notice-title")) : nullptr;
        const auto* text = notice ? dynamic_cast<const Label*>(FindByClass(notice, "inspector-notice-text")) : nullptr;
        return title && text ? title->GetText() + " / " + text->GetText() : std::string("(none)");
    }

    // Builds the fence inspector for `fence` into a real UIManager over the editor's stylesheets and
    // the strip's own, with the frame callbacks the inspector panel runs. Returns the strip, or null
    // with `skipReason` set when the machine has no device for the fixture.
    EditorUI::SplineFencePreview* BuildFencePreview(const Components::SplineFence& fence, std::string& skipReason)
    {
        Build(fence);
        return ShowFencePreview(skipReason, std::nullopt);
    }

    // Builds the inspector of the fence that Build made, as BuildFencePreview does. With `state`, the
    // inspector shows the fence in that build state, as the recipe system reports it.
    EditorUI::SplineFencePreview* ShowFencePreview(std::string& skipReason, std::optional<SplineRecipes::BuildState> state)
    {
        auto root = std::make_unique<UIElement>();
        previewRoot = root.get();
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Components::SplineFence>();
        if (!inspector)
            return nullptr;
        InspectorContext context;
        context.Parent = previewRoot;
        context.World = &world;
        context.Entity = entity;
        context.FrameRefreshCallbacks = &frameCallbacks;
        if (state)
            BuildSplineFenceInspectorInState(context, *state, {}, {});
        else
            (*inspector)(context);
        previewLayout.ExtraSheets = {"Assets/UI/inspectors/SplineRecipePreview/SplineRecipePreview.css"};
        if (!previewLayout.Build(std::move(root)))
        {
            skipReason = previewLayout.Diagnostic;
            return nullptr;
        }
        std::vector<EditorUI::SplineFencePreview*> previews;
        CollectOfType(*previewRoot, previews);
        return previews.size() == 1u ? previews.front() : nullptr;
    }

    // One frame of the inspector panel: its frame callbacks, then the UI update.
    void RunFrame()
    {
        for (const std::function<void()>& callback : frameCallbacks)
            callback();
        previewLayout.Ui->Update(1.0f / 60.0f, /*interactive=*/true);
    }

    ECS::World world;
    ECS::EntityHandle entity;
    UIElement parent;
    Editor::UndoRedoService undo;
    InspectorLayoutTesting::InspectorLayoutFixture previewLayout;
    UIElement* previewRoot = nullptr;
    std::vector<std::function<void()>> frameCallbacks;
};

// The fence of the strip's tests: a Span, a Gate and a Crest part, each with a mesh, and the fourth
// span of the run from point 0 made a gate.
Components::SplineFence FenceWithAGateAndACrest()
{
    using Components::SplineFencePartKind;
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), SplineFencePartKind::Span};
    fence.Parts[1] = {TestMesh(2), SplineFencePartKind::Gate};
    fence.Parts[2] = {TestMesh(3), SplineFencePartKind::Crest};
    fence.Overrides[0] = {0u, 3u, Components::SplineSpanOverrideKind::Gate, 0u};
    return fence;
}

} // namespace

// The strip under the notice lays the recipe out on the 12 m reference run: at Span Length 2.4 the
// run holds five spans between six stations, the fourth span is the gate, and a crest row runs along
// the tops.
TEST_F(SplineRecipeInspectorTest, TheFencePreviewDrawsThePartsTheLayoutGivesOnTheReferenceRun)
{
    std::string skipReason;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(FenceWithAGateAndACrest(), skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    ASSERT_GE(previewRoot->GetChildren().size(), 2u);
    EXPECT_EQ(previewRoot->GetChildren()[0]->GetId(), "spline-recipe-build-state");
    EXPECT_TRUE(previewRoot->GetChildren()[1]->HasClass("inspector-preview-row")) << "the strip is not under the notice";
    EXPECT_FALSE(preview->GetResolvedStyle().Visual.PointerEvents) << "the strip takes pointer input";

    const PreviewCount count = CountPreview(*preview);
    EXPECT_EQ(count.Stations, 6u);
    EXPECT_EQ(count.SpansAreGates, (std::vector<bool>{false, false, false, true, false}))
        << "the gate is not the fourth of five spans";
    EXPECT_GT(count.CrestBoxes, 0u) << "the strip draws no crest row";
}

// A Span Length edit through the row's own field redraws the strip in the next frame: at 4 m the run
// holds three spans between four stations, and the run has no fourth span for the gate.
TEST_F(SplineRecipeInspectorTest, ASpanLengthEditRedrawsTheFencePreviewInTheNextFrame)
{
    std::string skipReason;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(FenceWithAGateAndACrest(), skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    ASSERT_EQ(CountPreview(*preview).Stations, 6u);

    UIElement* row = FindRow(*previewRoot, "Span Length");
    ASSERT_NE(row, nullptr);
    std::vector<FloatField*> fields;
    CollectOfType(*row, fields);
    std::vector<TextInput*> inputs;
    CollectOfType(*row, inputs);
    ASSERT_EQ(fields.size(), 1u);
    ASSERT_EQ(inputs.size(), 1u);
    fields.front()->OnFocusChanged(true);
    inputs.front()->OnKey(Input::kKeyCode_A, Input::kModControl, nullptr);
    inputs.front()->OnChar(static_cast<unsigned char>('4'));
    inputs.front()->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    ASSERT_FLOAT_EQ(Fence().PostPitch, 4.0f);

    RunFrame();
    const PreviewCount count = CountPreview(*preview);
    EXPECT_EQ(count.Stations, 4u);
    EXPECT_EQ(count.SpansAreGates, (std::vector<bool>{false, false, false}));
}

// A part draws at a stand-in size until the scene measures its mesh. When the measured box lands,
// the strip lays the run out again: the 12 m run holds three 4 m spans, and the note hides.
TEST_F(SplineRecipeInspectorTest, TheFencePreviewRedrawsWhenTheSceneMeasuresAPart)
{
    std::string skipReason;
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    EditorUI::SplineFencePreview* preview = BuildFencePreview(fence, skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    UIElement* note = FindById(*preview, "spline-fence-preview-note");
    ASSERT_NE(note, nullptr);
    ASSERT_EQ(CountPreview(*preview).SpansAreGates.size(), 5u);
    EXPECT_FALSE(note->HasClass("hidden")) << "a stand-in part draws with no note";

    SplineRecipes::FenceMeshBounds measured{TestMesh(1), {}};
    measured.Bounds.Center = Mathematics::Vector3(0.0f, 0.5f, 0.0f);
    measured.Bounds.HalfExtents = Mathematics::Vector3(0.05f, 0.5f, 2.0f);
    preview->SetFence(Fence(), std::span<const SplineRecipes::FenceMeshBounds>(&measured, 1), {});
    const PreviewCount count = CountPreview(*preview);
    EXPECT_EQ(count.Stations, 4u);
    EXPECT_EQ(count.SpansAreGates.size(), 3u);
    EXPECT_TRUE(note->HasClass("hidden")) << "the note stays after the scene measured every part";
}

// A span piece longer than the reference run makes the run four of those pieces long, and the caption
// says so: a 15 m piece lays out four spans on a 60 m run.
TEST_F(SplineRecipeInspectorTest, ASpanLongerThanTheReferenceRunGrowsTheRunToFourSpans)
{
    std::string skipReason;
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    EditorUI::SplineFencePreview* preview = BuildFencePreview(fence, skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);

    SplineRecipes::FenceMeshBounds measured{TestMesh(1), {}};
    measured.Bounds.Center = Mathematics::Vector3(0.0f, 0.5f, 0.0f);
    measured.Bounds.HalfExtents = Mathematics::Vector3(0.05f, 0.5f, 7.5f);
    preview->SetFence(Fence(), std::span<const SplineRecipes::FenceMeshBounds>(&measured, 1), {});
    EXPECT_EQ(CountPreview(*preview).SpansAreGates.size(), 4u);
    const auto* caption = dynamic_cast<const Label*>(FindById(*preview, "spline-fence-preview-caption"));
    ASSERT_NE(caption, nullptr);
    EXPECT_EQ(caption->GetText(), "Reference run: 60 m (4 span lengths), flat, then a 1 in 4 slope");
}

// A gate longer than the run grows the run to four of the gate, and the caption names the gate.
TEST_F(SplineRecipeInspectorTest, AGateLongerThanTheReferenceRunGrowsTheRunToFourGates)
{
    std::string skipReason;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(FenceWithAGateAndACrest(), skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);

    SplineRecipes::FenceMeshBounds measured{TestMesh(2), {}};
    measured.Bounds.Center = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
    measured.Bounds.HalfExtents = Mathematics::Vector3(0.05f, 1.0f, 7.75f);
    preview->SetFence(Fence(), std::span<const SplineRecipes::FenceMeshBounds>(&measured, 1), {});
    const auto* caption = dynamic_cast<const Label*>(FindById(*preview, "spline-fence-preview-caption"));
    ASSERT_NE(caption, nullptr);
    EXPECT_EQ(caption->GetText(), "Reference run: 62 m (4 gate lengths), flat, then a 1 in 4 slope");
}

// The strip marks what the rows set: a seam between spans, the gate's outline and swing marks, a
// dashed line at each post, and the Span Length bar from the first post to the second.
TEST_F(SplineRecipeInspectorTest, TheFencePreviewMarksTheSeamsTheGateThePostsAndTheSpanLength)
{
    std::string skipReason;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(FenceWithAGateAndACrest(), skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);

    const PreviewCount count = CountPreview(*preview);
    EXPECT_EQ(count.SpansAreGates, (std::vector<bool>{false, false, false, true, false})) << "a span has no seam";
    EXPECT_GE(count.GateLines, 4u) << "the gate has no outline";
    EXPECT_GT(count.StationDashes, 2u * count.Stations) << "the post lines are not dashed";
    ASSERT_GE(count.StationXs.size(), 2u);
    const float spacing = count.StationXs[1] - count.StationXs[0];
    const bool hasSpanLengthBar =
        std::any_of(count.LabelLines.begin(), count.LabelLines.end(), [spacing](const UI::UIPrimitive& line) {
            return line.UvRect[1] == line.UvRect[3] && std::abs(std::abs(line.UvRect[2] - line.UvRect[0]) - spacing) < 1.0f;
        });
    EXPECT_TRUE(hasSpanLengthBar) << "no bar is as long as the first span";
}

// A Stepped span stays level and sinks its low end into the slope; the strip cuts it at the ground
// line, so the sunk part does not cover the ground.
TEST_F(SplineRecipeInspectorTest, TheFencePreviewCutsASteppedSpanAtTheGroundLine)
{
    std::string skipReason;
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    fence.SpanGrade = Components::SplineSpanGrade::Stepped;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(fence, skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);

    const PreviewCount count = CountPreview(*preview);
    ASSERT_EQ(count.GroundLines.size(), 2u);
    ASSERT_FALSE(count.SpanTriangles.empty());
    for (const UI::UIPrimitive& triangle : count.SpanTriangles)
    {
        const Mathematics::Vector2 corners[3] = {{triangle.UvRect[0], triangle.UvRect[1]},
                                                 {triangle.UvRect[2], triangle.UvRect[3]},
                                                 {triangle.BorderWidths[0], triangle.BorderWidths[1]}};
        for (const Mathematics::Vector2& corner : corners)
            EXPECT_LE(corner.y, GroundLineY(count.GroundLines, corner.x) + 0.5f) << "at x " << corner.x;
    }
}

// A span whose pivot is at its middle stands half under the ground. The strip fills only the part
// above the ground line, draws the part below it as an outline in the buried color, and draws the
// ground line over that outline.
TEST_F(SplineRecipeInspectorTest, TheFencePreviewDrawsThePartBelowTheGroundAsAnOutline)
{
    std::string skipReason;
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    EditorUI::SplineFencePreview* preview = BuildFencePreview(fence, skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    SplineRecipes::FenceMeshBounds measured{TestMesh(1), {}};
    measured.Bounds.Center = Mathematics::Vector3(0.0f, 0.0f, 0.0f);
    measured.Bounds.HalfExtents = Mathematics::Vector3(0.05f, 0.5f, 1.2f);
    preview->SetFence(Fence(), std::span<const SplineRecipes::FenceMeshBounds>(&measured, 1), {});

    const PreviewCount count = CountPreview(*preview);
    ASSERT_EQ(count.GroundLines.size(), 2u);
    ASSERT_FALSE(count.SpanTriangles.empty());
    for (const UI::UIPrimitive& triangle : count.SpanTriangles)
    {
        const Mathematics::Vector2 corners[3] = {{triangle.UvRect[0], triangle.UvRect[1]},
                                                 {triangle.UvRect[2], triangle.UvRect[3]},
                                                 {triangle.BorderWidths[0], triangle.BorderWidths[1]}};
        for (const Mathematics::Vector2& corner : corners)
            EXPECT_LE(corner.y, GroundLineY(count.GroundLines, corner.x) + 0.5f) << "a fill below the ground at x " << corner.x;
    }
    ASSERT_FALSE(count.BuriedLines.empty()) << "the part below the ground has no outline";
    // A racked span's low corner reaches past the end of the run, where the strip draws no ground line.
    for (const UI::UIPrimitive& line : count.BuriedLines)
    {
        for (const Mathematics::Vector2& end : {Mathematics::Vector2(line.UvRect[0], line.UvRect[1]),
                                                Mathematics::Vector2(line.UvRect[2], line.UvRect[3])})
        {
            const float ground = GroundLineY(count.GroundLines, end.x);
            if (ground != std::numeric_limits<float>::max())
                EXPECT_GE(end.y, ground - 0.5f) << "a buried line above the ground at x " << end.x;
        }
    }
    EXPECT_GT(count.FirstGroundLine, count.LastBuriedLine) << "the ground line is not drawn over the buried outline";
}

// A fence of posts and a gate: a 2 m post wider than a seam, and the fourth span made a gate.
Components::SplineFence FenceOfPostsAndAGate()
{
    using Components::SplineFencePartKind;
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), SplineFencePartKind::Post};
    fence.Parts[1] = {TestMesh(2), SplineFencePartKind::Gate};
    fence.Overrides[0] = {0u, 3u, Components::SplineSpanOverrideKind::Gate, 0u};
    return fence;
}

// The legend names only the kinds the fence has: a fence with no Span part lists no Span.
TEST_F(SplineRecipeInspectorTest, TheFencePreviewLegendListsOnlyTheKindsTheFenceHas)
{
    std::string skipReason;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(FenceOfPostsAndAGate(), skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    UIElement* post = FindById(*preview, "spline-fence-preview-legend-post");
    UIElement* span = FindById(*preview, "spline-fence-preview-legend-span");
    ASSERT_NE(post, nullptr);
    ASSERT_NE(span, nullptr);
    EXPECT_FALSE(post->HasClass("hidden"));
    EXPECT_TRUE(span->HasClass("hidden")) << "the legend names a Span the fence does not have";

    preview->SetFence(FenceWithAGateAndACrest(), {}, {});
    EXPECT_TRUE(post->HasClass("hidden")) << "the legend names a Post the fence does not have";
    EXPECT_FALSE(span->HasClass("hidden"));
}

// The scale bar stands left of the leftmost part, so a post wider than the strip's margin does not
// cover it, and it stays inside the strip.
TEST_F(SplineRecipeInspectorTest, TheFencePreviewScaleBarStandsClearOfAWidePost)
{
    std::string skipReason;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(FenceOfPostsAndAGate(), skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    SplineRecipes::FenceMeshBounds measured{TestMesh(1), {}};
    measured.Bounds.Center = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
    measured.Bounds.HalfExtents = Mathematics::Vector3(1.0f, 1.0f, 1.0f);
    preview->SetFence(Fence(), std::span<const SplineRecipes::FenceMeshBounds>(&measured, 1), {});

    const PreviewCount count = CountPreview(*preview);
    const UI::UIPrimitive* bar = nullptr;
    for (const UI::UIPrimitive& line : count.LabelLines)
    {
        const bool vertical = line.UvRect[0] == line.UvRect[2];
        if (vertical && (!bar || std::abs(line.UvRect[3] - line.UvRect[1]) > std::abs(bar->UvRect[3] - bar->UvRect[1])))
            bar = &line;
    }
    ASSERT_NE(bar, nullptr) << "the strip draws no scale bar";
    ASSERT_LT(count.PartsLeft, std::numeric_limits<float>::max());
    EXPECT_LT(bar->UvRect[0], count.PartsLeft) << "a post covers the scale bar";
    EXPECT_GE(bar->UvRect[0], 0.0f) << "the scale bar is outside the strip";
}

// A preview frame draws along and up at one scale, in a short plot and in a tall one, so a slope
// and a post show at their true angle whatever the panel's width.
TEST(SplineRecipePreviewFrameTest, TheFrameDrawsBothAxesAtOneScale)
{
    for (const float height : {60.0f, 400.0f})
    {
        const EditorUI::RecipePreviewFrame frame =
            EditorUI::FitRecipePreviewFrame(10.0f, 20.0f, 600.0f, height, 0.0f, 12.0f, -0.25f, 2.0f);
        const float along = (frame.X(12.0f) - frame.X(0.0f)) / 12.0f;
        const float up = (frame.Y(-0.25f) - frame.Y(2.0f)) / 2.25f;
        EXPECT_NEAR(along, up, 1e-3f) << "plot height " << height;
        EXPECT_GE(frame.X(0.0f), 10.0f - 1e-3f);
        EXPECT_LE(frame.X(12.0f), 610.0f + 1e-3f);
        EXPECT_GE(frame.Y(2.0f), 20.0f - 1e-3f);
        EXPECT_NEAR(frame.Y(-0.25f), 20.0f + height, 1e-3f) << "the run is not on the plot's floor";
    }
}

// The strip's text takes the editor's 12 px floor from its stylesheet.
TEST_F(SplineRecipeInspectorTest, TheFencePreviewTextIsAtLeastTwelvePixels)
{
    std::string skipReason;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(FenceWithAGateAndACrest(), skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);

    EXPECT_GE(preview->GetResolvedStyle().Visual.FontSize, 12.0f) << "the plot labels";
    std::vector<Label*> labels;
    CollectOfType(*preview, labels);
    ASSERT_FALSE(labels.empty());
    for (const Label* label : labels)
        EXPECT_GE(label->GetResolvedStyle().Visual.FontSize, 12.0f) << label->GetText();
}

// The Span bar gives the length the layout laid along the run's 12.12 m centerline. At Span Length
// 3.6 the run holds three spans stretched to 4.04 m, and the bar says so; at 2.42 the spans are laid
// as typed; a 2.5 m span mesh is shortened to 2.42 m to fit five spans.
TEST_F(SplineRecipeInspectorTest, TheSpanLengthBarGivesTheLaidLength)
{
    std::string skipReason;
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    fence.PostPitch = 3.6f;
    previewLayout.ViewportW = 1024;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(fence, skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    auto font = UITesting::LoadRobotoAtlas();
    ASSERT_NE(font, nullptr) << "Roboto-Regular.ttf must be staged beside the test executable";
    UI::UITextureRegistry textures(nullptr);

    EXPECT_TRUE(FindText(CountPreview(*preview, font.get(), &textures).Glyphs, "4.04 m laid (stretched from 3.6)", *font,
                         textures));

    fence.PostPitch = 2.42f;
    preview->SetFence(fence, {}, {});
    const std::vector<UI::UIPrimitive> asTyped = CountPreview(*preview, font.get(), &textures).Glyphs;
    EXPECT_TRUE(FindText(asTyped, "2.42 m", *font, textures));
    EXPECT_FALSE(FindText(asTyped, "laid", *font, textures)) << "a span laid as typed names a stretch";

    SplineRecipes::FenceMeshBounds measured{TestMesh(1), {}};
    measured.Bounds.Center = Mathematics::Vector3(0.0f, 0.5f, 0.0f);
    measured.Bounds.HalfExtents = Mathematics::Vector3(0.05f, 0.5f, 1.25f);
    preview->SetFence(fence, std::span<const SplineRecipes::FenceMeshBounds>(&measured, 1), {});
    EXPECT_TRUE(FindText(CountPreview(*preview, font.get(), &textures).Glyphs,
                         "2.42 m laid (2.5 m mesh, shortened)", *font, textures));
}

// A gate whose override places nothing on the spline draws dashed, with no fill, and says "Gate: not
// placed", as the override's warning says. The same gate on a span the fence placed stays solid.
TEST_F(SplineRecipeInspectorTest, AGateThatPlacesNothingDrawsDashedAndSaysSo)
{
    std::string skipReason;
    previewLayout.ViewportW = 1024;
    Build(FenceWithAGateAndACrest());
    for (const uint32_t ordinal : {0u, 1u, 2u})
        PlaceSpan(0u, ordinal);
    EditorUI::SplineFencePreview* preview = ShowFencePreview(skipReason, SplineRecipes::BuildState::Built);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    auto font = UITesting::LoadRobotoAtlas();
    ASSERT_NE(font, nullptr) << "Roboto-Regular.ttf must be staged beside the test executable";
    UI::UITextureRegistry textures(nullptr);

    const PreviewCount missing = CountPreview(*preview, font.get(), &textures);
    EXPECT_EQ(missing.SpansAreGates, (std::vector<bool>{false, false, false, false})) << "the gate draws a fill";
    EXPECT_GT(missing.GateLines, 8u) << "the gate's outline is not dashed";
    EXPECT_TRUE(FindText(missing.Glyphs, "Gate: not placed", *font, textures));
    EXPECT_FALSE(FindText(missing.Glyphs, "Gate: Span 3", *font, textures));

    PlaceSpan(0u, 3u);
    // The strip of the first build goes before the fixture replaces the device under it.
    frameCallbacks.clear();
    previewLayout.Ui.reset();
    preview = ShowFencePreview(skipReason, SplineRecipes::BuildState::Built);
    ASSERT_NE(preview, nullptr);
    const PreviewCount placed = CountPreview(*preview, font.get(), &textures);
    EXPECT_EQ(placed.SpansAreGates, (std::vector<bool>{false, false, false, true, false}));
    EXPECT_TRUE(FindText(placed.Glyphs, "Gate: Span 3", *font, textures));
}

// In a 560 px panel no label runs into a part: each stands above the parts under its whole width,
// and the marks under the ground keep the 8 m mark, also beside a long Span bar text.
TEST_F(SplineRecipeInspectorTest, TheFencePreviewLabelsRunIntoNoPartInANarrowPanel)
{
    std::string skipReason;
    previewLayout.ViewportW = 560;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(FenceWithAGateAndACrest(), skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    auto font = UITesting::LoadRobotoAtlas();
    ASSERT_NE(font, nullptr) << "Roboto-Regular.ttf must be staged beside the test executable";
    UI::UITextureRegistry textures(nullptr);

    Components::SplineFence fence = FenceWithAGateAndACrest();
    for (const float spanLength : {2.4f, 3.6f})
    {
        SCOPED_TRACE(spanLength);
        fence.PostPitch = spanLength;
        preview->SetFence(fence, {}, {});
        const PreviewCount count = CountPreview(*preview, font.get(), &textures);
        ASSERT_FALSE(count.Glyphs.empty());
        for (const UI::UIPrimitive& glyph : count.Glyphs)
        {
            for (const UI::UIPrimitive& part : count.PartTriangles)
            {
                const PixelRect glyphRect = GlyphRect(glyph);
                const PixelRect partRect = TriangleRect(part);
                EXPECT_FALSE(glyphRect.Overlaps(partRect, 0.5f))
                    << "a glyph at x " << glyphRect.Left << " y " << glyphRect.Top << " runs into a part at x "
                    << partRect.Left << " to " << partRect.Right << " y " << partRect.Top;
            }
        }
        EXPECT_TRUE(FindText(count.Glyphs, "8 m", *font, textures)) << "the 8 m mark is left out";
    }
}

// A Span part and a Gate part with no post or crest, and the fourth span of the run from point 0
// made a gate.
Components::SplineFence FenceOfSpansAndAGate()
{
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    fence.Parts[1] = {TestMesh(2), Components::SplineFencePartKind::Gate};
    fence.Overrides[0] = {0u, 3u, Components::SplineSpanOverrideKind::Gate, 0u};
    return fence;
}

// The measured bounds of a 2 m span and a 15.49 m gate whose pivot is at mid-height: the run grows
// to 62 m, and the layout lays 30 spans and the gate at one shared scale of about 0.83.
std::array<SplineRecipes::FenceMeshBounds, 2> ShortSpansAndALongGate()
{
    std::array<SplineRecipes::FenceMeshBounds, 2> measured{};
    measured[0].Mesh = TestMesh(1);
    measured[0].Bounds.Center = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
    measured[0].Bounds.HalfExtents = Mathematics::Vector3(0.05f, 1.0f, 1.0f);
    measured[1].Mesh = TestMesh(2);
    measured[1].Bounds.HalfExtents = Mathematics::Vector3(7.746f, 12.832f, 2.316f);
    return measured;
}

// On a 62 m run of short spans a span label is left out only where it would overlap another label:
// a sparse row of span names draws, no two labels overlap, no label runs into a part or out of the
// strip. The line under the plot gives the laid length of the gate the layout shortened.
TEST_F(SplineRecipeInspectorTest, OnALongRunOfShortSpansASpanLabelIsLeftOutOnlyWhereItOverlapsAnother)
{
    std::string skipReason;
    previewLayout.ViewportW = 1024;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(FenceOfSpansAndAGate(), skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    auto font = UITesting::LoadRobotoAtlas();
    ASSERT_NE(font, nullptr) << "Roboto-Regular.ttf must be staged beside the test executable";
    UI::UITextureRegistry textures(nullptr);
    const std::array<SplineRecipes::FenceMeshBounds, 2> measured = ShortSpansAndALongGate();
    preview->SetFence(Fence(), measured, {});

    const PreviewCount count = CountPreview(*preview, font.get(), &textures);
    ASSERT_EQ(count.SpansAreGates.size(), 31u) << "the run does not hold 30 spans and the gate";
    const auto* gateLength = dynamic_cast<const Label*>(FindById(*preview, "spline-fence-preview-gate-length"));
    ASSERT_NE(gateLength, nullptr);
    EXPECT_FALSE(gateLength->HasClass("hidden"));
    EXPECT_EQ(gateLength->GetText(), "Gate: Span 3, 12.85 m laid (15.49 m mesh, shortened)");
    const std::optional<PixelRect> gate = FindText(count.Glyphs, "Gate: Span 3", *font, textures);
    ASSERT_TRUE(gate) << "the gate has no label";
    // A shorter name is found again as the start of a longer one ("Span 1" in "Span 12"); the longer
    // one at the same place is the drawn label.
    std::vector<PixelRect> labels{*gate};
    for (uint32_t ordinal = 0; ordinal < 31u; ++ordinal)
    {
        const std::optional<PixelRect> found =
            FindText(count.Glyphs, "Span " + std::to_string(ordinal), *font, textures);
        if (!found || (found->Left >= gate->Left && found->Right <= gate->Right))
            continue;
        const auto samePlace = std::find_if(labels.begin(), labels.end(), [&found](const PixelRect& known) {
            return std::abs(known.Left - found->Left) < 0.5f;
        });
        if (samePlace == labels.end())
            labels.push_back(*found);
        else if (found->Right > samePlace->Right)
            *samePlace = *found;
    }
    EXPECT_GE(labels.size(), 3u) << "fewer than two span labels draw";
    for (std::size_t a = 0; a < labels.size(); ++a)
    {
        for (std::size_t b = a + 1; b < labels.size(); ++b)
            EXPECT_FALSE(labels[a].Overlaps(labels[b], 0.0f))
                << "labels at x " << labels[a].Left << " and " << labels[b].Left << " overlap";
    }
    const float stripRight = preview->GetLayoutX() + preview->GetLayoutWidth();
    for (const UI::UIPrimitive& glyph : count.Glyphs)
    {
        EXPECT_GE(glyph.X, preview->GetLayoutX()) << "a glyph is left of the strip";
        EXPECT_LE(glyph.X + glyph.W, stripRight) << "a glyph is right of the strip";
        for (const UI::UIPrimitive& part : count.PartTriangles)
            EXPECT_FALSE(GlyphRect(glyph).Overlaps(TriangleRect(part), 0.5f))
                << "a glyph at x " << glyph.X << " y " << glyph.Y << " runs into a part";
    }
}

// A label at an end of the run moves inside the strip, as far as it still covers its span's center:
// in a 240 px panel the last span's "Gate: not placed" keeps its full text and stays in the strip.
TEST_F(SplineRecipeInspectorTest, ALabelAtTheEndOfTheRunMovesInsideTheStrip)
{
    std::string skipReason;
    previewLayout.ViewportW = 240;
    Components::SplineFence fence = FenceWithAGateAndACrest();
    fence.Overrides[0].SpanOrdinal = 4u;
    Build(fence);
    for (const uint32_t ordinal : {0u, 1u, 2u, 3u})
        PlaceSpan(0u, ordinal);
    EditorUI::SplineFencePreview* preview = ShowFencePreview(skipReason, SplineRecipes::BuildState::Built);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    auto font = UITesting::LoadRobotoAtlas();
    ASSERT_NE(font, nullptr) << "Roboto-Regular.ttf must be staged beside the test executable";
    UI::UITextureRegistry textures(nullptr);

    const PreviewCount count = CountPreview(*preview, font.get(), &textures);
    const std::optional<PixelRect> label = FindText(count.Glyphs, "Gate: not placed", *font, textures);
    ASSERT_TRUE(label) << "the last span's label lost its full text";
    EXPECT_LE(label->Right, preview->GetLayoutX() + preview->GetLayoutWidth()) << "the label is right of the strip";
}

// The scale bar is the shortest of 1, 2, 5 and 10 m that stands at least 40 px tall, and its label
// names that length: 1 m on the 12 m run, longer on the 62 m run of a long gate.
TEST_F(SplineRecipeInspectorTest, TheScaleBarIsARoundLengthAtLeastFortyPixelsTall)
{
    std::string skipReason;
    EditorUI::SplineFencePreview* preview = BuildFencePreview(FenceWithAGateAndACrest(), skipReason);
    if (!skipReason.empty())
        GTEST_SKIP() << skipReason;
    ASSERT_NE(preview, nullptr);
    auto font = UITesting::LoadRobotoAtlas();
    ASSERT_NE(font, nullptr) << "Roboto-Regular.ttf must be staged beside the test executable";
    UI::UITextureRegistry textures(nullptr);

    SplineRecipes::FenceMeshBounds longGate{TestMesh(2), {}};
    longGate.Bounds.Center = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
    longGate.Bounds.HalfExtents = Mathematics::Vector3(0.05f, 1.0f, 7.75f);
    for (const float runLength : {12.0f, 62.0f})
    {
        SCOPED_TRACE(runLength);
        if (runLength > 12.0f)
            preview->SetFence(Fence(), std::span<const SplineRecipes::FenceMeshBounds>(&longGate, 1), {});
        const PreviewCount count = CountPreview(*preview, font.get(), &textures);
        ASSERT_FALSE(count.GroundLines.empty());
        const UI::UIPrimitive& flat = count.GroundLines.front();
        const float pixelsPerMeter = std::abs(flat.UvRect[2] - flat.UvRect[0]) / (runLength * 2.0f / 3.0f);
        float barPixels = 0.0f;
        for (const UI::UIPrimitive& line : count.LabelLines)
        {
            if (line.UvRect[0] == line.UvRect[2])
                barPixels = std::max(barPixels, std::abs(line.UvRect[3] - line.UvRect[1]));
        }
        const float barMeters = barPixels / pixelsPerMeter;
        const float steps[] = {1.0f, 2.0f, 5.0f, 10.0f};
        const auto step = std::find_if(std::begin(steps), std::end(steps),
                                       [barMeters](float meters) { return std::abs(barMeters - meters) < 0.02f * meters; });
        ASSERT_NE(step, std::end(steps)) << "the bar is " << barMeters << " m";
        EXPECT_GE(barPixels, 40.0f - 0.5f);
        if (step != std::begin(steps))
            EXPECT_LT(*(step - 1) * pixelsPerMeter, 40.0f) << "a shorter step is 40 px tall";
        const std::string label = std::to_string(static_cast<int>(*step)) + " m";
        EXPECT_TRUE(FindText(count.Glyphs, label, *font, textures)) << label;
    }
}

// With no recipe system running, the recipe has not built: the notice says so above the rows.
TEST_F(SplineRecipeInspectorTest, TheExtrudeShowsItsBuildStateAboveItsRows)
{
    Build<Components::SplineExtrude>();
    ASSERT_GT(parent.GetChildren().size(), 1u) << "the reflected rows come after the notice";
    EXPECT_EQ(parent.GetChildren().front()->GetId(), "spline-recipe-build-state");
    EXPECT_EQ(StateText(), kPendingText);
}

TEST_F(SplineRecipeInspectorTest, EveryRecipeKindShowsItsBuildState)
{
    Build<Components::SplineWall>();
    EXPECT_EQ(StateText(), kPendingText);
    parent.RemoveAllChildren();
    Build<Components::SplineFence>();
    EXPECT_EQ(StateText(), kPendingText);
    parent.RemoveAllChildren();
    Build<Components::SplinePlacement>();
    EXPECT_EQ(StateText(), kPendingText);
}

// A channel shows the rows of its cross-section and of its profile only; the region fill's rows
// stay out of the way.
TEST_F(SplineRecipeInspectorTest, AChannelExtrudeShowsOnlyTheRowsItsProfileUses)
{
    Build<Components::SplineExtrude>();
    EXPECT_TRUE(Shows(parent, "Width Mode"));
    EXPECT_TRUE(Shows(parent, "Profile"));
    EXPECT_TRUE(Shows(parent, "Edge Inset"));
    EXPECT_TRUE(Shows(parent, "End Taper"));
    EXPECT_FALSE(Shows(parent, "Crown Rise")) << "a Bevel has no crown";
    EXPECT_FALSE(Shows(parent, "Floor Height")) << "a channel fills no region";
    EXPECT_FALSE(Shows(parent, "Search Reach"));
    EXPECT_FALSE(Shows(parent, "Sea Level Floor"));
}

TEST_F(SplineRecipeInspectorTest, ACrownExtrudeShowsItsCrownRows)
{
    Components::SplineExtrude road;
    road.Profile = Components::SplineExtrudeProfile::Crown;
    Build(road);
    EXPECT_TRUE(Shows(parent, "Crown Rise"));
    EXPECT_TRUE(Shows(parent, "Shoulder Width"));
    EXPECT_FALSE(Shows(parent, "Edge Inset")) << "a Crown has no flat inset";
}

// A region fill shows its reach and its floor, and a floor that is off reads as one word.
TEST_F(SplineRecipeInspectorTest, AFitToBanksExtrudeShowsItsFloorAsNoneWhenItHasNone)
{
    Components::SplineExtrude river;
    river.WidthMode = Components::SplineExtrudeWidthMode::FitToBanks;
    Build(river);
    EXPECT_TRUE(Shows(parent, "Search Reach"));
    EXPECT_FALSE(Shows(parent, "Profile")) << "a filled region has no cross-section";
    EXPECT_FALSE(Shows(parent, "Tiles Per Meter U"));
    UIElement* floorRow = FindRow(parent, "Floor Height");
    ASSERT_NE(floorRow, nullptr);
    std::vector<TextInput*> inputs;
    CollectOfType(*floorRow, inputs);
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs.front()->GetValue(), "None");
}

// A floor that was set goes back to None when the author clears the field.
TEST_F(SplineRecipeInspectorTest, ClearingTheFloorHeightSetsItBackToNone)
{
    Components::SplineExtrude river;
    river.WidthMode = Components::SplineExtrudeWidthMode::FitToBanks;
    river.FloorHeight = 3.0f;
    Build(river);
    UIElement* floorRow = FindRow(parent, "Floor Height");
    ASSERT_NE(floorRow, nullptr);
    std::vector<FloatField*> fields;
    CollectOfType(*floorRow, fields);
    ASSERT_EQ(fields.size(), 1u);
    std::vector<TextInput*> inputs;
    CollectOfType(*floorRow, inputs);
    ASSERT_EQ(inputs.size(), 1u);
    fields.front()->OnFocusChanged(true);
    inputs.front()->OnKey(Input::kKeyCode_End, 0, nullptr);
    for (int i = 0; i < 16 && !inputs.front()->GetValue().empty(); ++i)
        inputs.front()->OnKey(Input::kKeyCode_Backspace, 0, nullptr);
    inputs.front()->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(world.GetComponent<Components::SplineExtrude>(entity)->FloorHeight, Components::kNoFloorHeight);
}

// Width Mode decides which rows follow, so it is the first row under the notice.
TEST_F(SplineRecipeInspectorTest, WidthModeIsTheExtrudesFirstRow)
{
    Build<Components::SplineExtrude>();
    std::vector<UIElement*> rows;
    CollectWithClass(parent, "inspector-row", rows);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(RowLabel(*rows.front()), "Width Mode");
}

// The three layouts of the extrude together show every reflected field, so a new field that no
// layout lists fails here.
TEST_F(SplineRecipeInspectorTest, TheExtrudeLayoutsTogetherShowEveryReflectedField)
{
    std::vector<std::string> shown;
    Components::SplineExtrude layouts[3];
    layouts[1].Profile = Components::SplineExtrudeProfile::Crown;
    layouts[2].WidthMode = Components::SplineExtrudeWidthMode::FitToBanks;
    for (const Components::SplineExtrude& layout : layouts)
    {
        parent.RemoveAllChildren();
        Build(layout);
        std::vector<UIElement*> rows;
        CollectWithClass(parent, "inspector-row", rows);
        for (UIElement* row : rows)
            shown.push_back(RowLabel(*row));
    }
    const auto fields = ECS::ComponentFieldRegistry::Get(ECS::GetComponentTypeId<Components::SplineExtrude>());
    ASSERT_FALSE(fields.empty());
    for (const ECS::FieldInfo& field : fields)
    {
        if (ECS::HasAnyFlag(field.Flags, ECS::FieldFlags::Hidden))
            continue;
        const std::string label = field.Label.empty() ? IdentifierToWords(field.Name) : std::string(field.Label);
        EXPECT_NE(std::find(shown.begin(), shown.end(), label), shown.end())
            << "no extrude layout shows " << field.Name << " (" << label << ")";
    }
}

// The placement's mode fields read as words through the shared enum rows.
TEST_F(SplineRecipeInspectorTest, ThePlacementShowsItsModesAsWords)
{
    Build<Components::SplinePlacement>();
    EXPECT_TRUE(Shows(parent, "Fit To Length"));
    EXPECT_TRUE(Shows(parent, "Height And Slope"));
    EXPECT_TRUE(Shows(parent, "Bounds Min"));
    EXPECT_FALSE(Shows(parent, "FitToLength"));
    EXPECT_FALSE(Shows(parent, "HeightAndSlope"));
}

// A fence saved with three parts loads with three part rows, each showing its kind, and no
// numbered slot labels.
TEST_F(SplineRecipeInspectorTest, ASavedFenceWithThreePartsLoadsWithThreePartRows)
{
    using Components::SplineFencePartKind;
    Components::SplineFence fence{};
    fence.Parts[0] = {{}, SplineFencePartKind::Post};
    fence.Parts[1] = {{}, SplineFencePartKind::Span};
    fence.Parts[2] = {{}, SplineFencePartKind::Crest};
    ECS::World saved;
    const ECS::EntityHandle savedEntity = saved.Create().GetHandle();
    saved.AddComponentImmediate(savedEntity, Components::SplineComponent{});
    saved.AddComponentImmediate(savedEntity, fence);
    const std::filesystem::path scenePath =
        std::filesystem::temp_directory_path() / "spline_recipe_inspector_three_parts.scene";
    ASSERT_TRUE(Scene::SaveSceneToFile(saved, scenePath, Scene::SaveOptions{}));
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    std::filesystem::remove(scenePath);

    ECS::EntityHandle loaded{};
    world.Query<ECS::Read<Components::SplineFence>>().Each(
        [&loaded](ECS::EntityHandle candidate, const Components::SplineFence&) { loaded = candidate; });
    ASSERT_TRUE(loaded.IsValid());
    RegisterSplineFenceInspector();
    BuildInspectorOf<Components::SplineFence>(loaded);

    std::vector<UIElement*> rows;
    CollectWithClass(parent, "spline-fence-part-row", rows);
    ASSERT_EQ(rows.size(), 3u);
    const char* kinds[] = {"Post", "Span", "Crest"};
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        std::vector<Dropdown*> dropdowns;
        CollectOfType(*rows[i], dropdowns);
        ASSERT_EQ(dropdowns.size(), 1u) << "row " << i;
        EXPECT_EQ(dropdowns.front()->GetSelectedLabel(), kinds[i]) << "row " << i;
    }
    EXPECT_FALSE(Shows(parent, "Post 1"));
    EXPECT_FALSE(Shows(parent, "Gate 1"));
}

// The kind dropdown offers the reflected kinds, without None, and the reflected tooltip.
TEST_F(SplineRecipeInspectorTest, ThePartKindOffersTheReflectedKinds)
{
    BuildFenceWithUndo(Components::SplineFence{});
    Dropdown* kind = RowKind(0);
    ASSERT_NE(kind, nullptr);
    const char* expected[] = {"Post", "Span", "Gate", "Crest", "Cap"};
    for (int i = 0; i < 5; ++i)
    {
        kind->SetSelectedIndexWithoutNotify(i);
        EXPECT_EQ(kind->GetSelectedLabel(), expected[i]) << "option " << i;
    }
    const ECS::FieldInfo* field =
        ECS::ComponentFieldRegistry::FindField(ECS::GetComponentTypeId<Components::SplineFencePart>(), "Kind");
    ASSERT_NE(field, nullptr);
    ASSERT_FALSE(field->Tooltip.empty());
    EXPECT_EQ(kind->GetTooltip(), field->Tooltip);
}

// Crest Pitch spaces the crest parts, so it shows only when the fence has one.
TEST_F(SplineRecipeInspectorTest, CrestPitchShowsOnlyWithACrestPart)
{
    Build<Components::SplineFence>();
    EXPECT_FALSE(Shows(parent, "Crest Pitch"));
    EXPECT_TRUE(Shows(parent, "Span Length"));
    parent.RemoveAllChildren();
    Components::SplineFence battlement{};
    battlement.Parts[1] = {{}, Components::SplineFencePartKind::Crest};
    Build(battlement);
    EXPECT_TRUE(Shows(parent, "Crest Pitch"));
}

// Add Part, Move Down, Move Up and Remove each make one undo step, which restores the list.
TEST_F(SplineRecipeInspectorTest, EachPartListEditIsOneUndoStep)
{
    using Components::SplineFencePartKind;
    Components::SplineFence fence{};
    fence.Parts[0] = {{}, SplineFencePartKind::Post};
    fence.Parts[1] = {{}, SplineFencePartKind::Span};
    BuildFenceWithUndo(fence);
    ASSERT_EQ(PartRows().size(), 2u);

    ASSERT_NE(AddPartButton(), nullptr);
    Click(*AddPartButton());
    EXPECT_EQ(Fence().Parts[2].Kind, SplineFencePartKind::Span) << "a new part takes the kind of the last";
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_EQ(Fence().Parts[2].Kind, SplineFencePartKind::None);

    RebuildFence();
    ASSERT_NE(RowAction(0, "list-item-move-down"), nullptr);
    Click(*RowAction(0, "list-item-move-down"));
    EXPECT_EQ(Fence().Parts[0].Kind, SplineFencePartKind::Span);
    EXPECT_EQ(Fence().Parts[1].Kind, SplineFencePartKind::Post);
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_EQ(Fence().Parts[0].Kind, SplineFencePartKind::Post);

    RebuildFence();
    ASSERT_NE(RowAction(1, "list-item-move-up"), nullptr);
    Click(*RowAction(1, "list-item-move-up"));
    EXPECT_EQ(Fence().Parts[0].Kind, SplineFencePartKind::Span);
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_EQ(Fence().Parts[0].Kind, SplineFencePartKind::Post);

    RebuildFence();
    ASSERT_NE(RowAction(0, "list-item-remove"), nullptr);
    Click(*RowAction(0, "list-item-remove"));
    EXPECT_EQ(Fence().Parts[0].Kind, SplineFencePartKind::Span);
    EXPECT_EQ(Fence().Parts[1].Kind, SplineFencePartKind::None);
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_EQ(Fence().Parts[0].Kind, SplineFencePartKind::Post);
    EXPECT_EQ(Fence().Parts[1].Kind, SplineFencePartKind::Span);
}

// A fence with no part shows no row, and Add Part gives it a Span.
TEST_F(SplineRecipeInspectorTest, RemovingTheLastPartThenAddingGivesASpan)
{
    BuildFenceWithUndo(Components::SplineFence{});
    ASSERT_EQ(PartRows().size(), 1u);
    ASSERT_NE(RowAction(0, "list-item-remove"), nullptr);
    Click(*RowAction(0, "list-item-remove"));
    RebuildFence();
    EXPECT_EQ(PartRows().size(), 0u);
    ASSERT_NE(AddPartButton(), nullptr);
    EXPECT_TRUE(AddPartButton()->IsEnabled());
    Click(*AddPartButton());
    EXPECT_EQ(Fence().Parts[0].Kind, Components::SplineFencePartKind::Span);
}

TEST_F(SplineRecipeInspectorTest, AFenceWithThirtyTwoPartsCannotAddAnother)
{
    Components::SplineFence fence{};
    for (Components::SplineFencePart& part : fence.Parts)
        part.Kind = Components::SplineFencePartKind::Crest;
    BuildFenceWithUndo(fence);
    EXPECT_EQ(PartRows().size(), Components::kSplineFenceMaxParts);
    ASSERT_NE(AddPartButton(), nullptr);
    EXPECT_FALSE(AddPartButton()->IsEnabled());
    Click(*AddPartButton());
    EXPECT_EQ(undo.GetUndoCount(), 0u);
    for (const Components::SplineFencePart& part : Fence().Parts)
        EXPECT_EQ(part.Kind, Components::SplineFencePartKind::Crest);
}

// An explicit override draws the n-th Span part with a mesh. A move, a removal or a kind change
// keeps it on the same part, in the same undo step, and a removed part leaves it with none.
TEST_F(SplineRecipeInspectorTest, PartListEditsKeepAnOverrideOnItsPart)
{
    using Components::SplineFencePartKind;
    const Components::ModelRef second = TestMesh(2);
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), SplineFencePartKind::Span};
    fence.Parts[1] = {second, SplineFencePartKind::Span};
    fence.Parts[2] = {TestMesh(3), SplineFencePartKind::Post};
    fence.Overrides[0] = {0u, 1u, Components::SplineSpanOverrideKind::ExplicitPart, 1u};
    BuildFenceWithUndo(fence);
    ASSERT_EQ(OverrideMesh(), second);

    ASSERT_NE(RowAction(1, "list-item-move-up"), nullptr);
    Click(*RowAction(1, "list-item-move-up"));
    EXPECT_EQ(Fence().Overrides[0].PoolSlot, 0u);
    EXPECT_EQ(OverrideMesh(), second) << "Move Up";
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_EQ(Fence().Overrides[0].PoolSlot, 1u);

    RebuildFence();
    ASSERT_NE(RowAction(0, "list-item-remove"), nullptr);
    Click(*RowAction(0, "list-item-remove"));
    EXPECT_EQ(OverrideMesh(), second) << "Remove of the part before it";
    undo.Undo();
    EXPECT_EQ(Fence().Overrides[0].PoolSlot, 1u);

    RebuildFence();
    ASSERT_NE(RowKind(0), nullptr);
    RowKind(0)->SetSelectedIndex(0);
    ASSERT_EQ(Fence().Parts[0].Kind, SplineFencePartKind::Post);
    EXPECT_EQ(OverrideMesh(), second) << "a kind change of the part before it";
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_EQ(Fence().Overrides[0].PoolSlot, 1u);

    RebuildFence();
    ASSERT_NE(RowAction(1, "list-item-remove"), nullptr);
    Click(*RowAction(1, "list-item-remove"));
    EXPECT_GE(Fence().Overrides[0].PoolSlot, Components::kSplineFenceMaxParts) << "its part is gone";
    RebuildFence();
    EXPECT_TRUE(Shows(parent, "Missing part"));
    undo.Undo();
    EXPECT_EQ(OverrideMesh(), second);
}

// Each override is one row of a list: its kind, its point, its span and its part, with no
// numbered slot label.
TEST_F(SplineRecipeInspectorTest, EachOverrideIsOneRowOfTheList)
{
    using Components::SplineFencePartKind;
    using Components::SplineSpanOverrideKind;
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), SplineFencePartKind::Span};
    fence.Parts[1] = {TestMesh(2), SplineFencePartKind::Gate};
    fence.Overrides[0] = {0u, 3u, SplineSpanOverrideKind::Gate, 0u};
    fence.Overrides[4] = {1u, 5u, Components::SplineSpanOverrideKind::ExplicitPart, 0u};
    BuildFenceWithUndo(fence);
    ASSERT_EQ(OverrideRows().size(), 2u);
    EXPECT_FALSE(Shows(parent, "Override 1"));
    const std::vector<IntField*> numbers = OverrideFields<IntField>(1);
    ASSERT_EQ(numbers.size(), 2u);
    EXPECT_EQ(numbers[0]->GetValue(), 1) << "the point";
    EXPECT_EQ(numbers[1]->GetValue(), 5) << "the span";
    const std::vector<Dropdown*> dropdowns = OverrideFields<Dropdown>(1);
    ASSERT_EQ(dropdowns.size(), 2u) << "the kind and the part";
    EXPECT_EQ(dropdowns[0]->GetSelectedLabel(), "Explicit Part");
}

// Add Override, a point edit, Move Down and Remove each make one undo step. An override keeps
// its part through each, and a part list edit still moves it with its part.
TEST_F(SplineRecipeInspectorTest, EachOverrideListEditIsOneUndoStep)
{
    using Components::SplineFencePartKind;
    using Components::SplineSpanOverride;
    using Components::SplineSpanOverrideKind;
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), SplineFencePartKind::Span};
    fence.Parts[1] = {TestMesh(2), SplineFencePartKind::Span};
    fence.Parts[2] = {TestMesh(3), SplineFencePartKind::Gate};
    fence.Overrides[0] = {0u, 1u, SplineSpanOverrideKind::Gate, 0u};
    fence.Overrides[1] = {0u, 2u, Components::SplineSpanOverrideKind::ExplicitPart, 1u};
    BuildFenceWithUndo(fence);
    ASSERT_EQ(OverrideRows().size(), 2u);

    ASSERT_NE(AddOverrideButton(), nullptr);
    Click(*AddOverrideButton());
    EXPECT_EQ(Fence().Overrides[2], (SplineSpanOverride{0u, 3u, Components::SplineSpanOverrideKind::ExplicitPart, 1u}))
        << "the last override's kind, point and part, on the next span that has no override";
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_EQ(Fence().Overrides[2].Kind, SplineSpanOverrideKind::None);

    RebuildFence();
    std::vector<IntField*> numbers = OverrideFields<IntField>(0);
    std::vector<TextInput*> inputs = OverrideFields<TextInput>(0);
    ASSERT_EQ(numbers.size(), 2u);
    ASSERT_EQ(inputs.size(), 2u) << "the point, then the span";
    numbers[0]->OnFocusChanged(true);
    inputs[0]->OnKey(Input::kKeyCode_End, 0, nullptr);
    inputs[0]->OnKey(Input::kKeyCode_Backspace, 0, nullptr);
    inputs[0]->OnChar('2');
    inputs[0]->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(Fence().Overrides[0].PointIndex, 2u);
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_EQ(Fence().Overrides[0].PointIndex, 0u);

    RebuildFence();
    ASSERT_NE(OverrideAction(0, "list-item-move-down"), nullptr);
    Click(*OverrideAction(0, "list-item-move-down"));
    EXPECT_EQ(Fence().Overrides[0], (SplineSpanOverride{0u, 2u, Components::SplineSpanOverrideKind::ExplicitPart, 1u}));
    EXPECT_EQ(Fence().Overrides[1], (SplineSpanOverride{0u, 1u, SplineSpanOverrideKind::Gate, 0u}));
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_EQ(Fence().Overrides[0].Kind, SplineSpanOverrideKind::Gate);

    RebuildFence();
    ASSERT_NE(OverrideAction(0, "list-item-remove"), nullptr);
    Click(*OverrideAction(0, "list-item-remove"));
    EXPECT_EQ(Fence().Overrides[0], (SplineSpanOverride{0u, 2u, Components::SplineSpanOverrideKind::ExplicitPart, 1u}));
    EXPECT_EQ(Fence().Overrides[1].Kind, SplineSpanOverrideKind::None);
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    RebuildFence();
    EXPECT_EQ(OverrideRows().size(), 1u);
    undo.Undo();
    EXPECT_EQ(Fence().Overrides[0].Kind, SplineSpanOverrideKind::Gate);
    EXPECT_EQ(Fence().Overrides[1].PoolSlot, 1u);

    RebuildFence();
    ASSERT_NE(RowAction(0, "list-item-remove"), nullptr);
    Click(*RowAction(0, "list-item-remove"));
    EXPECT_EQ(Fence().Overrides[1].PoolSlot, 0u) << "the removed part was before its part in the pool";
}

// A fence with no override and no Gate part adds an explicit part on span 0 of the segment from
// point 0. A full list cannot add another.
TEST_F(SplineRecipeInspectorTest, AddOverrideStartsAtTheFirstSpanAndStopsWhenFull)
{
    BuildFenceWithUndo(Components::SplineFence{});
    EXPECT_EQ(OverrideRows().size(), 0u);
    ASSERT_NE(AddOverrideButton(), nullptr);
    Click(*AddOverrideButton());
    EXPECT_EQ(Fence().Overrides[0],
              (Components::SplineSpanOverride{0u, 0u, Components::SplineSpanOverrideKind::ExplicitPart, 0u}))
        << "a gate with no Gate part is an opening";

    parent.RemoveAllChildren();
    Components::SplineFence full{};
    for (uint32_t slot = 0; slot < Components::kSplineFenceMaxSpanOverrides; ++slot)
        full.Overrides[slot] = {0u, static_cast<uint16_t>(slot), Components::SplineSpanOverrideKind::Gate, 0u};
    BuildFenceWithUndo(full);
    const std::size_t steps = undo.GetUndoCount();
    ASSERT_NE(AddOverrideButton(), nullptr);
    EXPECT_FALSE(AddOverrideButton()->IsEnabled());
    Click(*AddOverrideButton());
    EXPECT_EQ(undo.GetUndoCount(), steps);
}

// A fence that has not built says nothing about the span of an override: no build counted its
// spans. The override's own problem still shows.
TEST_F(SplineRecipeInspectorTest, AnOverrideOfAFenceThatHasNotBuiltNamesOnlyItsOwnProblem)
{
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    fence.Overrides[0] = {0u, 3u, Components::SplineSpanOverrideKind::ExplicitPart, 0u};
    fence.Overrides[1] = {0u, 4u, Components::SplineSpanOverrideKind::ExplicitPart, 6u};
    BuildFenceWithUndo(fence);
    EXPECT_FALSE(ShowsTextWith("no span"));
    EXPECT_FALSE(ShowsTextWith("places nothing"));
    EXPECT_TRUE(ShowsTextWith("not in the Parts list"));
}

// A Built fence whose rows have problems does not read as up to date: the notice counts the
// problems and names them.
TEST_F(SplineRecipeInspectorTest, ABuiltStateWithWarningsNamesThem)
{
    using SplineRecipes::BuildState;
    const std::vector<std::string> none;
    const BuildStateWords upToDate = DescribeBuildState(BuildState::Built, {}, none);
    EXPECT_EQ(upToDate.Title, "Built");
    EXPECT_EQ(upToDate.Text, "It is up to date.");

    const std::vector<std::string> one = {"The gate on span 3 of the segment from point 0 places nothing."};
    const BuildStateWords warned = DescribeBuildState(BuildState::Built, {}, one);
    EXPECT_EQ(warned.Title, "Built, 1 warning");
    EXPECT_EQ(warned.Text, one.front());
    EXPECT_EQ(warned.Kind, EditorUI::InspectorNotice::Kind::Warning);

    const std::vector<std::string> two = {"First.", "Second."};
    EXPECT_EQ(DescribeBuildState(BuildState::Built, {}, two).Title, "Built, 2 warnings");
    EXPECT_EQ(DescribeBuildState(BuildState::Built, {}, two).Text, "First.\nSecond.");
    EXPECT_EQ(DescribeBuildState(BuildState::Failed, "It failed.", two).Text, "It failed.")
        << "a failed build names its own reason, not the rows";
}

// Of two overrides on one span, the fence uses the first. The second names that problem.
TEST_F(SplineRecipeInspectorTest, ASecondOverrideOnOneSpanSaysItPlacesNothing)
{
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    fence.Overrides[0] = {0u, 2u, Components::SplineSpanOverrideKind::ExplicitPart, 0u};
    fence.Overrides[1] = {0u, 2u, Components::SplineSpanOverrideKind::ExplicitPart, 0u};
    BuildFenceWithUndo(fence);
    EXPECT_TRUE(ShowsTextWith("an override above it names the same span"));
}

// A fence with no Span part sends the author to Add Part, and a Span part with no mesh to its
// mesh. Neither says the fence picks a part by Seed: that needs a build.
TEST_F(SplineRecipeInspectorTest, AnOverrideWithNoSpanPartNamesTheFixThatExists)
{
    using Components::SplineFencePartKind;
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), SplineFencePartKind::Gate};
    fence.Overrides[0] = {0u, 1u, Components::SplineSpanOverrideKind::ExplicitPart, 0u};
    BuildFenceWithUndo(fence);
    EXPECT_TRUE(ShowsTextWith("Add a Span part with Add Part"));
    EXPECT_FALSE(ShowsTextWith("Choose one of the fence's Span parts"));
    EXPECT_FALSE(ShowsTextWith("by Seed"));

    parent.RemoveAllChildren();
    fence.Parts[1] = {Components::ModelRef{}, SplineFencePartKind::Span};
    BuildFenceWithUndo(fence);
    EXPECT_TRUE(ShowsTextWith("Give the Span part a mesh"));
    EXPECT_FALSE(ShowsTextWith("Add a Span part with Add Part"));

    parent.RemoveAllChildren();
    fence.Parts[1] = {TestMesh(2), SplineFencePartKind::Span};
    fence.Overrides[0].PoolSlot = 4u;
    BuildFenceWithUndo(fence);
    EXPECT_TRUE(ShowsTextWith("Choose one of the fence's Span parts"));
    EXPECT_FALSE(ShowsTextWith("by Seed")) << "the fence has not built";
}

// The part dropdown names each part by its kind and its mesh, not by a number that changes when a
// part above it is removed. Two parts of one kind and mesh are told apart by a count.
TEST_F(SplineRecipeInspectorTest, TheOverridePartNamesItsKindAndMesh)
{
    using Components::SplineFencePartKind;
    const Components::ModelRef gate = TestMesh(2);
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), SplineFencePartKind::Span};
    fence.Parts[1] = {gate, SplineFencePartKind::Gate};
    fence.Parts[2] = {TestMesh(1), SplineFencePartKind::Span};
    fence.Overrides[0] = {0u, 1u, Components::SplineSpanOverrideKind::Gate, 0u};
    fence.Overrides[1] = {0u, 2u, Components::SplineSpanOverrideKind::ExplicitPart, 1u};
    BuildFenceWithUndo(fence);
    const std::string gateName = "Gate: " + gate.ToGuid().ToString();
    const std::string spanName = "Span: " + TestMesh(1).ToGuid().ToString();
    std::vector<Dropdown*> dropdowns = OverrideFields<Dropdown>(0);
    ASSERT_EQ(dropdowns.size(), 2u);
    EXPECT_EQ(dropdowns[1]->GetSelectedLabel(), gateName);
    dropdowns = OverrideFields<Dropdown>(1);
    ASSERT_EQ(dropdowns.size(), 2u);
    EXPECT_EQ(dropdowns[1]->GetSelectedLabel(), spanName + " (2)");
    dropdowns[1]->SetSelectedIndexWithoutNotify(0);
    EXPECT_EQ(dropdowns[1]->GetSelectedLabel(), spanName);

    ASSERT_NE(RowAction(0, "list-item-remove"), nullptr);
    Click(*RowAction(0, "list-item-remove"));
    RebuildFence();
    dropdowns = OverrideFields<Dropdown>(0);
    ASSERT_EQ(dropdowns.size(), 2u);
    EXPECT_EQ(dropdowns[1]->GetSelectedLabel(), gateName) << "the same name after a part above it is removed";
}

// A span past the end of its segment is named once, with the segment's span count and the fix.
TEST_F(SplineRecipeInspectorTest, ASpanPastTheEndOfItsSegmentSaysHowManySpansItHas)
{
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    fence.Overrides[0] = {0u, 5u, Components::SplineSpanOverrideKind::ExplicitPart, 0u};
    const std::vector<std::pair<uint32_t, uint32_t>> standing = {{0u, 0u}, {0u, 1u}, {0u, 2u}, {0u, 3u}, {1u, 0u}};
    Editor::FenceBuildState built;
    built.Built = true;
    built.StandingSpans = standing;
    built.SegmentCount = 2u;
    EXPECT_EQ(Editor::SpanOverrideProblem(fence, 0u, built),
              "Span 5 is past the end of the segment from point 0 (spans 0 to 3). Change Span, or remove the "
              "override.");
}

// A point that starts no segment is named once, with the points that start one and the fix.
TEST_F(SplineRecipeInspectorTest, APointPastTheLastSegmentSaysWhichPointsStartOne)
{
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    fence.Overrides[0] = {7u, 0u, Components::SplineSpanOverrideKind::ExplicitPart, 0u};
    Editor::FenceBuildState built;
    built.SegmentCount = 3u;
    EXPECT_EQ(Editor::SpanOverrideProblem(fence, 0u, built),
              "Point 7 starts no segment of the spline (segments start at points 0 to 2). Change Point, or "
              "remove the override.");
}

// A Built fence hands the problems of its rows to the state notice, so it reads Built with a
// warning, not up to date.
TEST_F(SplineRecipeInspectorTest, TheFenceHandsItsRowProblemsToTheStateNotice)
{
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    fence.Parts[1] = {Components::ModelRef{}, Components::SplineFencePartKind::Gate};
    BuildFenceWithUndo(fence);
    RebuildBuiltFence();
    EXPECT_EQ(StateText(), "Built, 1 warning / Part 2, a Gate part, has no mesh, so it places nothing. Give it a "
                           "mesh, or remove it.");
}

// Under Pivot Plane a gate whose pivot is at its middle stands half under the ground. The notice
// names the depth and the plant mode. A span with the measured kit's planting skirt, 0.52 m of its
// 1.64 m under its pivot, gives no warning; under Bounds Min the gate gives none.
TEST_F(SplineRecipeInspectorTest, APartHalfUnderTheGroundNamesThePlantMode)
{
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    fence.Parts[1] = {TestMesh(2), Components::SplineFencePartKind::Gate};
    BuildFenceWithUndo(fence);
    SplineRecipes::FenceMeshBounds measured[2] = {{TestMesh(1), {}}, {TestMesh(2), {}}};
    measured[0].Bounds.Center = Mathematics::Vector3(0.0f, 0.29776f, 0.0f);
    measured[0].Bounds.HalfExtents = Mathematics::Vector3(0.06f, 0.81937f, 1.2f);
    measured[1].Bounds.Center = Mathematics::Vector3(0.0f, 0.0f, 0.0f);
    measured[1].Bounds.HalfExtents = Mathematics::Vector3(0.05f, 7.7f, 7.75f);
    RebuildBuiltFence(measured);
    EXPECT_EQ(StateText(), "Built, 1 warning / Part 2, a Gate part, reaches 7.7 m below the ground line (Plant Mode: "
                           "Pivot Plane). Set Plant Mode to Bounds Min to stand it on the ground.");

    fence.PlantMode = Components::SplinePlantMode::BoundsMin;
    BuildFenceWithUndo(fence);
    RebuildBuiltFence(measured);
    EXPECT_EQ(StateText(), "Built / It is up to date.");
}

// On a Built fence, one click on Add Override puts the override on a span the fence placed, so
// the fence stays Built with no warning.
TEST_F(SplineRecipeInspectorTest, AddOverrideOnABuiltFenceLandsOnASpanItPlaced)
{
    Components::SplineFence fence{};
    fence.Parts[0] = {TestMesh(1), Components::SplineFencePartKind::Span};
    fence.Overrides[0] = {0u, 1u, Components::SplineSpanOverrideKind::ExplicitPart, 0u};
    BuildFenceWithUndo(fence);
    PlaceSpan(0u, 0u);
    PlaceSpan(0u, 1u);
    PlaceSpan(1u, 0u);
    RebuildBuiltFence();
    ASSERT_EQ(StateText(), "Built / It is up to date.");
    ASSERT_NE(AddOverrideButton(), nullptr);
    Click(*AddOverrideButton());
    EXPECT_EQ(Fence().Overrides[1],
              (Components::SplineSpanOverride{1u, 0u, Components::SplineSpanOverrideKind::ExplicitPart, 0u}))
        << "span 1 is the last span of the segment from point 0";
    RebuildBuiltFence();
    EXPECT_EQ(StateText(), "Built / It is up to date.");
    EXPECT_EQ(OverrideRows().size(), 2u);
}

} // namespace GameEngine
