#include "UI/Interaction/TooltipOverlay.h"

#include "Logger/Logger.h"
#include "UI/Controls/Label.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/Internal/LayoutAccess.h"

#include "UI/UIPrimitive.h"
#include "../BubblePaint.h"
#include "../Layout/TooltipBubblePlacement.h"
#include "../UIManager/UIManager_Internal.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace GameEngine::UI::Interaction
{

static float ClampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Logical px.
constexpr float kArrowGap = 8.0f;        // from the source's edge to the arrow's tip
constexpr float kArrowFillHeight = 8.0f; // the arrow's fill, from the bubble's edge to the tip
constexpr float kArrowFillBase = 9.0f;   // the fill's base on the bubble's edge
constexpr float kEdgePad = 6.0f;         // the bubble's least distance from the window edge

// The bubble's border on the edge the arrow sits on: the arrow's tip points away from that
// edge. 0 when the bubble draws no border on that edge.
static float FacingBorderWidth(const ResolvedStyle& style, UI::ArrowDirection direction)
{
    const BoxSide edge = UI::ArrowEdge(direction);
    if (style.Visual.BorderStyle == BorderStyle::None || (BorderSideColor(style.Visual.BorderColor, edge) >> 24) == 0)
        return 0.0f;
    return BorderSideWidth(style.Layout.BorderWidth, edge);
}

namespace
{
struct ArrowSize
{
    float Height = 0.0f;    // from the bubble's edge to the tip
    float BaseWidth = 0.0f; // the base on the bubble's edge
};
} // namespace

// The arrow's outline around a fill of kArrowFillHeight by kArrowFillBase: the bubble's
// border moves the two slanted sides out at the same slope, so the fill keeps its size at
// every border width. Logical px.
static ArrowSize ArrowSizeForBorder(float border)
{
    const float slope = kArrowFillBase * 0.5f / kArrowFillHeight;
    const float height = kArrowFillHeight + border * std::sqrt(1.0f + slope * slope) / slope;
    return {height, height * 2.0f * slope};
}

// From the source's edge to the bubble's edge when the arrow points `direction`.
static float ReachFor(const ResolvedStyle& style, UI::ArrowDirection direction)
{
    return kArrowGap + ArrowSizeForBorder(FacingBorderWidth(style, direction)).Height;
}

// The arrow points from the bubble back at its anchor.
static UI::ArrowDirection ArrowDirectionFor(UIElement::TooltipPlacement side)
{
    switch (side)
    {
    case UIElement::TooltipPlacement::Above:
        return UI::ArrowDirection::Down;
    case UIElement::TooltipPlacement::Right:
        return UI::ArrowDirection::Left;
    case UIElement::TooltipPlacement::Left:
        return UI::ArrowDirection::Right;
    case UIElement::TooltipPlacement::Below:
    default:
        return UI::ArrowDirection::Up;
    }
}

// True when `element` would draw: no element on its DFS-parent chain is display: none, its
// own resolved visibility (which inherits) is visible, and the chain ends at `root`. A Mount
// target swapped out of its dock tab ends its chain at itself.
static bool IsShownUnder(const UIElement& element, const UIElement& root)
{
    if (!element.GetResolvedStyle().Visual.Visible)
        return false;
    for (const UIElement* e = &element; e; e = e->GetDfsParent())
    {
        if (e->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
            return false;
    }
    return UIElementReachesRoot(&element, &root);
}

// The arrow's center along a bubble edge that starts at `edgeStart`: at `anchor`, kept where
// the arrow's base stays on the straight part of the edge between its two rounded corners.
// `halfBase` is half the base's widest extent.
static float ClampAlongEdge(float anchor, float edgeStart, float edgeLength, float startRadius, float endRadius,
                            float halfBase)
{
    const float low = edgeStart + startRadius + halfBase;
    const float high = edgeStart + edgeLength - endRadius - halfBase;
    if (low > high)
        return edgeStart + edgeLength * 0.5f;
    return ClampF(anchor, low, high);
}

static void ShiftLayoutSubtree(UIElement* el, float dx, float dy)
{
    if (!el)
        return;
    UILayoutAccess::SetLastLayoutRect(*el, el->GetLayoutX() + dx, el->GetLayoutY() + dy,
                          el->GetLayoutWidth(), el->GetLayoutHeight());
    for (const auto& child : el->GetChildren())
        ShiftLayoutSubtree(child.get(), dx, dy);
}

void TooltipOverlay::ResetIfDetached()
{
    if (m_Wrapper && m_Wrapper->GetParent() == nullptr)
    {
        m_Wrapper  = nullptr;
        m_Tooltip  = nullptr;
        m_Label    = nullptr;
        m_TemplateParts.clear();
        m_TemplateClasses.clear();
        m_TemplateSourceId = 0;
        m_FailedTemplateSourceId = 0;
        m_SourceStylesheets.clear();
        m_LastText.clear();
        m_Visible   = false;
        m_WarmingUp = false;
        m_HasShownTooltip = false;
        m_HasArrow  = false;
        m_Parked    = false;
        m_HasLastPlaced = false;
    }
}

void TooltipOverlay::EnsureElements(UIElement* root)
{
    if (!root)
        return;

    if (m_Wrapper)
        return;

    if (UIElement* existing = root->FindById("ui-tooltip-wrapper"_sid))
    {
        m_Wrapper = existing;
        m_Tooltip = m_Wrapper->FindById("ui-tooltip"_sid);
        m_Label   = dynamic_cast<Label*>(m_Wrapper->FindById("ui-tooltip-text"_sid));
        // The wrapper holds only the sheets a source document lent it.
        m_SourceStylesheets = m_Wrapper->GetStylesheets();
    }
    else
    {
        auto wrapper = std::make_unique<UIElement>();
        wrapper->SetId("ui-tooltip-wrapper");
        wrapper->AddClass("tooltip-wrapper");

        auto tip = std::make_unique<UIElement>();
        tip->SetId("ui-tooltip");
        tip->AddClass("tooltip");

        auto lbl = std::make_unique<Label>();
        lbl->SetId("ui-tooltip-text");
        lbl->AddClass("tooltip-text");
        m_Label = lbl.get();
        tip->AddChild(std::move(lbl));

        m_Tooltip = tip.get();
        wrapper->AddChild(std::move(tip));

        m_Wrapper = wrapper.get();
        root->AddChild(std::move(wrapper));
    }

    // One-shot constant styles, applied on creation and adoption alike.
    // Re-applying these every frame would dirty the wrapper and force a
    // full primitive regen (the values never change during its lifetime).
    //
    // Overlay ordering, rather than local z-index, decides precedence once
    // a dropdown/modal escapes its panel stacking context, so hover help
    // lives on the final system layer.
    m_Wrapper->SetOverlayLayer(OverlayLayer::HoverTooltip);
    m_Wrapper->Style()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PointerEvents, false)
        .Set(Style::ZIndex, 9600);

    // Cap the bubble width so long tooltip text wraps. Yoga clamps the
    // label's measure constraint against the cap, so the wrapped size lands
    // in a single layout pass. Engine-side (not editor CSS) so every
    // UIManager host gets sane wrapping. The label must NOT carry a percent
    // width: it would resolve against the pre-clamp available width and let
    // the text escape the bubble.
    if (m_Tooltip)
        m_Tooltip->Style().Set(Style::MaxWidth, StyleLength::Px(kMaxTooltipWidth));
}

void TooltipOverlay::ParkOffscreen()
{
    if (!m_Wrapper)
        return;
    m_HasArrow = false;
    if (m_Parked)
        return;
    m_Wrapper->Style()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::PositionLeft, StyleLength::Px(-9999.0f))
        .Set(Style::PositionTop,  StyleLength::Px(-9999.0f));
    m_Parked = true;
}

void TooltipOverlay::BeginShow(const UIElement* source)
{
    // The previous template's parts and classes leave the bubble before new content mounts.
    UnmountTemplate();
    m_FailedTemplateSourceId = 0;
    AdoptSourceStylesheets(source);
}

// The wrapper sits under the root, outside the source's document, so it takes the
// stylesheets attached to the source and its ancestors below the root, outermost first:
// a document's .tooltip rule and the classes of its templates reach the bubble.
void TooltipOverlay::AdoptSourceStylesheets(const UIElement* source)
{
    if (!m_Wrapper)
        return;
    const UIElement* root = m_Wrapper->GetParent();
    std::vector<StylesheetHandle> sheets;
    for (const UIElement* element = source; element && element != root; element = element->GetDfsParent())
    {
        const std::vector<StylesheetHandle>& own = element->GetStylesheets();
        sheets.insert(sheets.begin(), own.begin(), own.end());
    }
    if (sheets == m_SourceStylesheets)
        return;
    for (const StylesheetHandle& sheet : m_SourceStylesheets)
        m_Wrapper->RemoveStylesheet(sheet);
    for (const StylesheetHandle& sheet : sheets)
        m_Wrapper->AddStylesheet(sheet);
    m_SourceStylesheets = std::move(sheets);
}

TooltipOverlay::ContentUpdate TooltipOverlay::UpdateContent(const TooltipContent& content)
{
    if (!m_Tooltip || !m_Label)
        return ContentUpdate::Failed;
    if (content.Template)
        return MountTemplate(*content.Source, *content.Template);

    const bool wasTemplate = m_TemplateSourceId != 0;
    UnmountTemplate();
    if (!wasTemplate && m_LastText == *content.Text)
        return ContentUpdate::Unchanged;
    m_LastText = *content.Text;
    m_Label->SetText(m_LastText);
    m_WarmingUp = false;
    m_Visible   = false;
    m_HasArrow  = false;
    return ContentUpdate::Changed;
}

TooltipOverlay::ContentUpdate TooltipOverlay::MountTemplate(UIElement& source,
                                                            const UIElement::TooltipTemplate& tooltipTemplate)
{
    const uint64_t sourceId = source.GetInstanceId();
    if (m_TemplateSourceId == sourceId)
        return ContentUpdate::Unchanged;
    if (m_FailedTemplateSourceId == sourceId)
        return ContentUpdate::Failed;

    UnmountTemplate();
    m_LastText.clear();
    m_WarmingUp = false;
    m_Visible   = false;
    m_HasArrow  = false;

    // The layout's children join the bubble after the label, and its root's classes join the
    // bubble's, so a document can style the bubble of one template apart from the others.
    const size_t firstPart = m_Tooltip->GetChildren().size();
    const std::vector<std::string> classesBefore = m_Tooltip->GetClasses();
    UIManager* ui = m_Tooltip->GetOwnerManager();
    if (!ui || !ui->InstantiateLayoutChildrenFromAssetPath(m_Tooltip, tooltipTemplate.LayoutPath))
    {
        m_FailedTemplateSourceId = sourceId;
        if (m_FailedTemplatePath != tooltipTemplate.LayoutPath)
        {
            m_FailedTemplatePath = tooltipTemplate.LayoutPath;
            Logger::Log::Warning("UI: the tooltip template '{}' did not load, so its tooltip does not show. Check "
                                 "that the layout exists under the project's assets and that the path is relative "
                                 "to them.",
                                 tooltipTemplate.LayoutPath);
        }
        return ContentUpdate::Failed;
    }

    for (size_t i = firstPart; i < m_Tooltip->GetChildren().size(); ++i)
        m_TemplateParts.push_back(m_Tooltip->GetChildren()[i].get());
    m_Label->Style().Set(Style::Display, DisplayMode::None);
    m_TemplateSourceId = sourceId;
    if (tooltipTemplate.Binder)
        tooltipTemplate.Binder(*m_Tooltip);
    // The classes that the layout's root and the binder add leave with the template.
    for (const std::string& name : m_Tooltip->GetClasses())
    {
        if (std::find(classesBefore.begin(), classesBefore.end(), name) == classesBefore.end())
            m_TemplateClasses.push_back(name);
    }
    return ContentUpdate::Changed;
}

void TooltipOverlay::UnmountTemplate()
{
    if (m_TemplateSourceId == 0)
        return;
    for (UIElement* part : m_TemplateParts)
        m_Tooltip->RemoveChild(part);
    for (const std::string& name : m_TemplateClasses)
        m_Tooltip->RemoveClass(name);
    m_TemplateParts.clear();
    m_TemplateClasses.clear();
    m_TemplateSourceId = 0;
    m_Label->Style().Set(Style::Display, DisplayMode::Flex);
}

// True when a child of `element` is a popup on an overlay layer that shows now, as a dropdown's open list is.
static bool HasOpenPopup(const UIElement& element)
{
    for (const auto& child : element.GetChildren())
    {
        if (child->GetOverlayLayer() == OverlayLayer::None)
            continue;
        const ResolvedStyle& style = child->GetResolvedStyle();
        if (style.Layout.DisplayMode != DisplayMode::None && style.Visual.Visible)
            return true;
    }
    return false;
}

TooltipOverlay::TooltipContent TooltipOverlay::OwnTooltip(UIElement& source)
{
    return {&source, &source.GetTooltip(), source.GetTooltipTemplate()};
}

TooltipOverlay::TooltipContent TooltipOverlay::FindTooltip(UIElement* element)
{
    // An authored tooltip anywhere on the ancestor chain is the answer. Only
    // when the whole chain has none does a label truncated by
    // text-overflow: ellipsis stand in as its own tooltip, so hover reveals
    // the full text without per-call-site wiring.
    TooltipContent truncated;

    for (UIElement* el = element; el; el = el->GetParent())
    {
        if (!el->GetTooltip().empty() || el->GetTooltipTemplate())
        {
            // A control with an open popup shows no tooltip, so the tooltip does not cover the popup.
            if (HasOpenPopup(*el))
                return {};
            return OwnTooltip(*el);
        }
        if (const ITextMeasurable* tm = truncated.Text ? nullptr : el->GetTextMeasurable();
            tm && tm->WasLastRunEllipsized())
        {
            // GetTextContent is the same accessor emission truncated, and it
            // returns stable storage, so the pointer outlives this walk.
            const std::string& content = el->GetTextContent();
            if (!content.empty())
                truncated = {el, &content, nullptr};
        }
        // A popup does not show the tooltip of the control that opened it.
        if (el->GetOverlayLayer() != OverlayLayer::None)
            break;
    }
    return truncated;
}

void TooltipOverlay::PlaceTooltip(UIElement* root, UIElement* source, float mouseX, float mouseY)
{
    if (!m_Wrapper || !m_Tooltip || !root)
        return;

    const float vpW = root->GetLayoutWidth();
    const float vpH = root->GetLayoutHeight();
    const float tipW = m_Tooltip->GetLayoutWidth();
    const float tipH = m_Tooltip->GetLayoutHeight();
    const ResolvedStyle& style = m_Tooltip->GetResolvedStyle();

    constexpr float kGapAbove = 10.0f; // extra gap above the cursor when there is no source element

    const UIElement::TooltipPlacement placement =
        source ? source->GetTooltipPlacement() : UIElement::TooltipPlacement::Auto;
    // Canvas-style controls draw many virtual items inside one large UIElement (node graph
    // nodes and wires): an Auto tooltip there points at the cursor, not at the canvas edge.
    const bool atCursor =
        !source || (placement == UIElement::TooltipPlacement::Auto && source->HasClass("node-canvas"));
    Layout::PopupRect anchorRect{mouseX, mouseY - kGapAbove, 0.0f, kGapAbove};
    Layout::PopupPosition aim{mouseX, mouseY};
    if (!atCursor)
    {
        // The bubble is a window-level overlay: it anchors where the source and its anchor
        // are drawn, which a CSS transform moves away from their layout rects.
        const Mathematics::Rect src = source->GetWindowRect();
        aim = {src.X + src.Width * 0.5f, src.Y + src.Height * 0.5f};
        const UIElement* anchor = source->GetTooltipAnchor();
        const bool anchored = anchor && IsShownUnder(*anchor, *root);
        const Mathematics::Rect frame = anchored ? anchor->GetWindowRect() : src;
        anchorRect = {frame.X, frame.Y, frame.Width, frame.Height};
    }
    const Layout::TooltipReach reach{ReachFor(style, UI::ArrowDirection::Up), ReachFor(style, UI::ArrowDirection::Down),
                                     ReachFor(style, UI::ArrowDirection::Left),
                                     ReachFor(style, UI::ArrowDirection::Right)};
    const Layout::TooltipBubblePlacement placed = Layout::PlaceTooltipBubble(
        {0.0f, 0.0f, vpW, vpH}, anchorRect, aim, tipW, tipH, reach, kEdgePad, placement);

    const float finalX = std::round(placed.Position.X);
    const float finalY = std::round(placed.Position.Y);
    // An anchored bubble's arrow lines up with the source along the anchor's facing edge: in a
    // grid of tiles it shows the source's column or row, and the source's hover style shows the tile.
    PlaceArrow(ArrowDirectionFor(placed.Side), placed.ArrowAim, finalX, finalY, tipW, tipH);

    // Skip the Set chain when nothing actually moved. Re-applying the same
    // Display/Position values would dirty the wrapper every frame.
    if (!m_Parked && m_HasLastPlaced && finalX == m_LastPlacedX && finalY == m_LastPlacedY)
        return;

    m_Wrapper->Style()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::PositionLeft, StyleLength::Px(finalX))
        .Set(Style::PositionTop,  StyleLength::Px(finalY));
    m_Parked = false;
    m_LastPlacedX = finalX;
    m_LastPlacedY = finalY;
    m_HasLastPlaced = true;

    const float curX = m_Wrapper->GetLayoutX();
    const float curY = m_Wrapper->GetLayoutY();
    ShiftLayoutSubtree(m_Wrapper, finalX - curX, finalY - curY);
}

void TooltipOverlay::PlaceArrow(UI::ArrowDirection direction, float anchor, float bubbleX, float bubbleY,
                                float bubbleW, float bubbleH)
{
    const ResolvedStyle& style = m_Tooltip->GetResolvedStyle();
    const CornerRadiiTLTRBRBL radii = UsedBorderRadius(style.Visual, bubbleW, bubbleH);
    // The border triangle's base reaches across the bubble's border (UI::MakePointerArrow),
    // where the arrow is widest.
    const float border = FacingBorderWidth(style, direction);
    const ArrowSize size = ArrowSizeForBorder(border);
    const float halfBase = size.BaseWidth * 0.5f * (size.Height + border) / size.Height;
    Mathematics::Vector2 tip;
    switch (direction)
    {
    case UI::ArrowDirection::Down:
        tip = {ClampAlongEdge(anchor, bubbleX, bubbleW, radii.BottomLeft.X, radii.BottomRight.X, halfBase),
               bubbleY + bubbleH + size.Height};
        break;
    case UI::ArrowDirection::Left:
        tip = {bubbleX - size.Height,
               ClampAlongEdge(anchor, bubbleY, bubbleH, radii.TopLeft.Y, radii.BottomLeft.Y, halfBase)};
        break;
    case UI::ArrowDirection::Right:
        tip = {bubbleX + bubbleW + size.Height,
               ClampAlongEdge(anchor, bubbleY, bubbleH, radii.TopRight.Y, radii.BottomRight.Y, halfBase)};
        break;
    case UI::ArrowDirection::Up:
    default:
        tip = {ClampAlongEdge(anchor, bubbleX, bubbleW, radii.TopLeft.X, radii.TopRight.X, halfBase),
               bubbleY - size.Height};
        break;
    }
    if (!m_HasArrow || direction != m_ArrowDirection || !(tip == m_ArrowTip))
        m_ArrowChanged = true;
    m_ArrowTip = tip;
    m_ArrowDirection = direction;
    m_HasArrow = true;
}

bool TooltipOverlay::ConsumeArrowChange()
{
    const bool changed = m_ArrowChanged;
    m_ArrowChanged = false;
    return changed;
}

void TooltipOverlay::EmitArrowPrimitives(std::vector<UI::UIPrimitive>& prims, float opacity, float contentScale) const
{
    if (!m_HasArrow || !m_Visible || !m_Tooltip)
        return;
    // Don't emit the arrow if the tooltip body hasn't been measured yet.
    if (m_Tooltip->GetLayoutWidth() <= 0.0f || m_Tooltip->GetLayoutHeight() <= 0.0f)
        return;

    const float cs = (contentScale > 0.0f) ? contentScale : 1.0f;
    const ResolvedStyle& style = m_Tooltip->GetResolvedStyle();
    UI::PointerArrow arrow;
    arrow.Tip = m_ArrowTip * cs;
    arrow.Direction = m_ArrowDirection;
    const float border = FacingBorderWidth(style, m_ArrowDirection);
    const ArrowSize size = ArrowSizeForBorder(border);
    arrow.BaseWidth = size.BaseWidth * cs;
    arrow.Height = size.Height * cs;
    arrow.FillColor = UI::PackFromARGB(style.Visual.BackgroundColor);
    arrow.BorderColor = UI::PackFromARGB(BorderSideColor(style.Visual.BorderColor, UI::ArrowEdge(m_ArrowDirection)));
    arrow.BorderWidth = border * cs;
    const UI::PointerArrowPrimitives triangles = UI::MakePointerArrow(arrow);
    for (uint32_t i = 0; i < triangles.Count; ++i)
    {
        UI::UIPrimitive triangle = triangles.Triangles[i];
        UI::SetOpacity(triangle, opacity);
        prims.push_back(triangle);
    }
}

void TooltipOverlay::Update(UIElement* root, UIElement* hovered, float mouseX, float mouseY, float time)
{
    ResetIfDetached();

    if (!root)
        return;

    const TooltipContent content = hovered ? FindTooltip(hovered) : TooltipContent{};
    UIElement* tooltipSource = content.Source;

    if (content.IsEmpty())
    {
        m_HoveredId      = 0;
        m_HoverStartTime = 0.0f;
        m_WarmingUp      = false;
        if (m_Visible)
        {
            m_LastVisibleTime = time;
            m_Visible = false;
            m_HasArrow = false;
            // Park offscreen (keep Display::Flex) so Yoga continues to measure
            // the wrapper. This ensures dimensions are valid when the next
            // tooltip needs to hop-show immediately.
            ParkOffscreen();
        }
        return;
    }

    // Use the tooltip source element's ID (not the hovered element) so that
    // moving the mouse between a button and its child text label doesn't
    // restart the hover delay.
    const uint64_t currentId = tooltipSource ? tooltipSource->GetInstanceId() : 0;

    if (currentId != m_HoveredId)
    {
        const bool wasVisible = m_Visible;
        if (wasVisible)
            m_LastVisibleTime = time;

        const bool hop = wasVisible ||
            (m_HasShownTooltip && (time - m_LastVisibleTime) < m_HoverResetDelaySeconds);

        m_HoveredId      = currentId;
        m_HoverStartTime = hop ? 0.0f : time;

        EnsureElements(root);
        BeginShow(tooltipSource);
        if (UpdateContent(content) != ContentUpdate::Unchanged)
        {
            ParkOffscreen();
            return;
        }

        if (hop)
        {
            // Show immediately during hop if the wrapper has valid dimensions.
            // If not (was Display::None), park for 1 frame so Yoga measures.
            const float cw = m_Tooltip ? m_Tooltip->GetLayoutWidth() : 0.0f;
            const float ch = m_Tooltip ? m_Tooltip->GetLayoutHeight() : 0.0f;
            if (cw > 0.0f && ch > 0.0f)
            {
                m_Visible = true;
                m_HasShownTooltip = true;
                m_WarmingUp = false;
                PlaceTooltip(root, tooltipSource, mouseX, mouseY);
                return;
            }
            // Dimensions stale — warm up for 1 frame.
            m_WarmingUp = true;
            ParkOffscreen();
            return;
        }

        m_WarmingUp      = false;
        if (m_Visible)
        {
            m_Visible = false;
            m_HasArrow = false;
        }
        ParkOffscreen();
        return;
    }

    if (!m_MiddleMousePending && (time - m_HoverStartTime) < m_HoverDelaySeconds)
    {
        EnsureElements(root);
        UpdateContent(content);
        ParkOffscreen();
        return;
    }

    m_MiddleMousePending = false;

    EnsureElements(root);
    if (UpdateContent(content) != ContentUpdate::Unchanged)
    {
        ParkOffscreen();
        return;
    }

    const float curW = m_Wrapper ? m_Wrapper->GetLayoutWidth()  : 0.0f;
    const float curH = m_Wrapper ? m_Wrapper->GetLayoutHeight() : 0.0f;

    if (curW <= 0.0f || curH <= 0.0f || m_WarmingUp)
    {
        m_WarmingUp = false;
        ParkOffscreen();
        return;
    }

    m_WarmingUp = false;
    m_Visible   = true;
    m_HasShownTooltip = true;
    PlaceTooltip(root, tooltipSource, mouseX, mouseY);
}

void TooltipOverlay::Pin(UIElement& source)
{
    m_Pinned = UIElement::MakeWeakRef(&source);
    m_HasPin = true;
}

void TooltipOverlay::Unpin(const UIElement& source)
{
    if (m_Pinned.Get() != &source)
        return;
    m_Pinned = {};
    m_HasPin = false;
    Hide();
}

bool TooltipOverlay::UpdatePinned(UIElement* root)
{
    if (!m_HasPin)
        return false;
    UIElement* source = m_Pinned.Get();
    if (!source)
    {
        m_HasPin = false;
        Hide();
        return false;
    }

    ResetIfDetached();
    if (!root)
        return true;

    const TooltipContent content = OwnTooltip(*source);
    if (content.IsEmpty() || !IsShownUnder(*source, *root))
    {
        m_Visible = false;
        ParkOffscreen();
        return true;
    }

    EnsureElements(root);
    if (m_HoveredId != source->GetInstanceId())
        BeginShow(source);
    m_HoveredId = source->GetInstanceId();
    m_HoverStartTime = 0.0f;
    m_MiddleMousePending = false;
    // New content parks for one frame: it measures before the bubble is placed.
    if (UpdateContent(content) != ContentUpdate::Unchanged || !m_Tooltip || m_Tooltip->GetLayoutWidth() <= 0.0f ||
        m_Tooltip->GetLayoutHeight() <= 0.0f)
    {
        ParkOffscreen();
        return true;
    }

    m_WarmingUp = false;
    m_Visible = true;
    m_HasShownTooltip = true;
    PlaceTooltip(root, source, 0.0f, 0.0f);
    return true;
}

void TooltipOverlay::OnMiddleMousePressed(UIElement* hovered)
{
    if (!hovered)
        return;
    if (FindTooltip(hovered).IsEmpty())
        return;
    m_MiddleMousePending = true;
    m_HoveredId = hovered->GetInstanceId();
}

void TooltipOverlay::Hide()
{
    ResetIfDetached();
    m_HoveredId          = 0;
    m_HoverStartTime     = 0.0f;
    m_Visible            = false;
    m_WarmingUp          = false;
    m_MiddleMousePending = false;
    m_HasArrow           = false;
    ParkOffscreen();
}

} // namespace GameEngine::UI::Interaction
