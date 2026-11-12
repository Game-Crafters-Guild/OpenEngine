#pragma once

#include "Mathematics/Vector2.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
class Label;

namespace UI
{
struct UIPrimitive;
enum class ArrowDirection : uint8_t;
}

namespace UI::Interaction
{

// The hover tooltip, owned by UIManager: the bubble (.tooltip) shows the text or the template
// of the hovered element or its nearest ancestor with a tooltip. The arrow is not an element:
// it is two triangles painted with the bubble's background and border.
class TooltipOverlay final
{
public:
    // Call once per frame after hover chain and events have been resolved.
    // root     - the UI root element (tooltip is appended here)
    // hovered  - the currently hovered element (may be nullptr)
    // mouseX/Y - current mouse position in root-space pixels
    // time     - current UIManager time in seconds (used for hover delay)
    void Update(UIElement* root, UIElement* hovered, float mouseX, float mouseY, float time);

    // Pins `source`'s tooltip (UIManager::PinTooltip): it shows at once, anchored to
    // `source`, with no hover and no delay, until Unpin(source) or until `source` is
    // destroyed. One element holds the pin; pinning another moves it.
    void Pin(UIElement& source);
    // Ends the pin `source` holds and hides the bubble; does nothing when another
    // element holds it.
    void Unpin(const UIElement& source);

    // Call once per frame before Update. Returns false when no element holds the pin;
    // the caller then runs the hover path. While the pin is held, shows the pinned
    // element's tooltip whenever the element is shown: not hidden (display: none on it
    // or an ancestor, visibility: hidden on it) and reachable from `root` (a dock tab
    // swapped out is not). Otherwise the bubble is parked, and comes back when the
    // element shows again. Empty text shows nothing. A destroyed element ends the pin
    // and parks the bubble.
    bool UpdatePinned(UIElement* root);

    // Called when middle-mouse button is pressed — shows tooltip immediately if the
    // hovered element (or an ancestor) has tooltip text.
    void OnMiddleMousePressed(UIElement* hovered);

    // Force-hide the tooltip immediately (e.g. on mouse leave from window).
    void Hide();

    // Runtime configuration (called each frame by the editor from TooltipSettings).
    void SetHoverDelay(float seconds) { m_HoverDelaySeconds = seconds; }
    void SetHoverResetDelay(float seconds) { m_HoverResetDelaySeconds = seconds; }

    // Appends the arrow's triangles, to draw over the placed bubble. contentScale
    // converts the arrow's logical px (PlaceArrow) to physical px.
    void EmitArrowPrimitives(std::vector<UI::UIPrimitive>& prims, float opacity, float contentScale) const;

    // True once after the arrow moved, turned or appeared. A bubble that stays in place asks for
    // no layout, so the caller asks for the primitive regen that emits the arrow.
    bool ConsumeArrowChange();

private:
    void EnsureElements(UIElement* root);
    void ResetIfDetached();
    void ParkOffscreen();

    // A tooltip to show: a template or a text, and the element it belongs to.
    struct TooltipContent
    {
        UIElement* Source = nullptr;
        const std::string* Text = nullptr;
        const UIElement::TooltipTemplate* Template = nullptr;

        bool IsEmpty() const { return !Template && (!Text || Text->empty()); }
    };
    enum class ContentUpdate : uint8_t
    {
        Unchanged,
        // The bubble's content changed: park it for a frame, so it measures before it is placed.
        Changed,
        // The template did not mount: show nothing.
        Failed,
    };

    // Walks up the element chain to the nearest tooltip. The walk stops at a popup (an element on an
    // overlay layer), and a control with an open popup has no tooltip.
    static TooltipContent FindTooltip(UIElement* element);
    // The tooltip of `source` itself, for a pin.
    static TooltipContent OwnTooltip(UIElement& source);

    // A tooltip starts to show for `source`: its template mounts and binds again, and the
    // wrapper takes the stylesheets of the source's document.
    void BeginShow(const UIElement* source);
    void AdoptSourceStylesheets(const UIElement* source);
    ContentUpdate UpdateContent(const TooltipContent& content);
    ContentUpdate MountTemplate(UIElement& source, const UIElement::TooltipTemplate& tooltipTemplate);
    void UnmountTemplate();

    void PlaceTooltip(UIElement* root, UIElement* source, float mouseX, float mouseY);
    // Puts the arrow on the bubble's edge that faces the source, at `anchor` along that
    // edge, clear of the bubble's rounded corners. The bubble rect is in logical px.
    void PlaceArrow(UI::ArrowDirection direction, float anchor, float bubbleX, float bubbleY, float bubbleW,
                    float bubbleH);

    // The element whose tooltip is pinned; empty when none is.
    UIElement::WeakRef<> m_Pinned;
    bool m_HasPin = false;

    UIElement* m_Wrapper  = nullptr; // outer container (position/z-index, no clipping)
    UIElement* m_Tooltip  = nullptr; // visible box (background, border-radius, shadow)
    Label*     m_Label    = nullptr;
    // While a template shows: its parts, children of m_Tooltip after the hidden label, and the
    // classes that the layout's root and the binder added to m_Tooltip. Empty in text mode.
    std::vector<UIElement*> m_TemplateParts;
    std::vector<std::string> m_TemplateClasses;
    // The element whose template is mounted, and the one whose template did not mount; 0 when none.
    uint64_t   m_TemplateSourceId = 0;
    uint64_t   m_FailedTemplateSourceId = 0;
    // The last layout that did not mount, so its warning is logged once.
    std::string m_FailedTemplatePath;
    // The source document's stylesheets the wrapper holds (AdoptSourceStylesheets).
    std::vector<StylesheetHandle> m_SourceStylesheets;

    std::string m_LastText;
    bool        m_Visible   = false;
    bool        m_WarmingUp = false;
    bool        m_HasShownTooltip = false;
    // Track whether the wrapper is currently parked offscreen so subsequent
    // ParkOffscreen() calls during the hover delay become no-ops instead of
    // re-applying the same Display/Position values (which would dirty the
    // wrapper every frame and force a full primitive regen).
    bool        m_Parked    = false;
    // Last placed position so PlaceTooltip can short-circuit when the
    // tooltip stays put across frames (e.g. stationary cursor over a
    // tooltip-bearing button).
    float       m_LastPlacedX = 0.0f;
    float       m_LastPlacedY = 0.0f;
    bool        m_HasLastPlaced = false;

    // The arrow: placed by PlaceArrow (logical px), emitted by EmitArrowPrimitives.
    bool                 m_HasArrow = false;
    UI::ArrowDirection   m_ArrowDirection{}; // Up
    Mathematics::Vector2 m_ArrowTip;
    bool                 m_ArrowChanged = false; // read by ConsumeArrowChange

    // Hover delay tracking
    uint64_t    m_HoveredId                = 0;
    float       m_HoverStartTime           = 0.0f;
    float       m_LastVisibleTime          = 0.0f;
    float       m_HoverDelaySeconds        = 0.5f;
    float       m_HoverResetDelaySeconds   = 0.5f;
    // Max bubble width (logical px), applied as a Style::MaxWidth override on
    // the tooltip box so long text wraps; short text keeps its natural width.
    static constexpr float kMaxTooltipWidth = 512.0f;
    bool  m_MiddleMousePending = false;
};

} // namespace UI::Interaction
} // namespace GameEngine
