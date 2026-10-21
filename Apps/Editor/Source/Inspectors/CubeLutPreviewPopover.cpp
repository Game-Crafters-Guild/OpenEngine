#include "Inspectors/CubeLutPreviewPopover.h"

#include "Components/Rendering/Look.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"
#include "Inspectors/CubeLutPreview.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Layout/PopupPlacement.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{
// Wide enough for the probes to read (each probe's three exposures about 80 px each); narrower
// windows shrink it to fit.
constexpr float kPanelWidth = 1100.0f;
// The panel's height at the enlarged band sizes (CubeLutPreview.css), for placement only.
constexpr float kPanelHeightEstimate = 250.0f;
constexpr float kAnchorGap = 6.0f;
constexpr float kEdgePadding = 8.0f;

// What the bands show, in one line above them.
std::string Caption(const CubeLutStripRequest& request, bool showsLut)
{
    const auto look = static_cast<Components::Look>(std::clamp(request.Look, 0, Components::kLookCount - 1));
    std::string caption = std::string(Components::LookName(look)) + " look";
    if (!showsLut)
        return caption + "; the LUT effect is off";
    const auto stage = request.Stage == static_cast<uint32_t>(Components::CubeLutStage::Grade)
                           ? Components::CubeLutStage::Grade
                           : Components::CubeLutStage::Finish;
    char intensity[16];
    std::snprintf(intensity, sizeof(intensity), "%.2f", static_cast<double>(request.Intensity));
    return caption + ", LUT at " + std::string(Components::CubeLutStageName(stage)) + ", intensity " + intensity;
}
} // namespace

CubeLutPreviewPopover::CubeLutPreviewPopover()
    : DismissablePopup(this)
{
    // Above the dock panels and their clipping, beneath modals. The anchor's geometry is in
    // CubeLutPreview.css (.cube-lut-preview-popover).
    SetOverlayLayer(OverlayLayer::Dropdown);
    AddClass("cube-lut-preview-popover");
    RequestSubtreeStyleAssetPath(kCubeLutPreviewStyleAssetPath, "editor");
}

CubeLutPreviewPopover::~CubeLutPreviewPopover()
{
    // A view removed without Close (its window torn down first) must not leave the preview
    // pointing at it.
    if (m_Owner)
        m_Owner->m_Enlarged = nullptr;
}

CubeLutPreviewPopover* CubeLutPreviewPopover::Open(CubeLutPreview& preview)
{
    UIElement* anchor = preview.GetRoot();
    UIManager* ui = anchor ? anchor->GetOwnerManager() : nullptr;
    UIElement* window = ui ? ui->GetRootElement() : nullptr;
    if (!window)
        return nullptr;

    std::unique_ptr<CubeLutPreviewPopover> popover(new CubeLutPreviewPopover());
    popover->m_Owner = &preview;

    auto panel = std::make_unique<UIElement>();
    panel->AddClass("popover-panel");
    panel->AddClass("cube-lut-preview-popover-panel");
    const float width = std::min(kPanelWidth, std::max(window->GetLayoutWidth() - 2.0f * kEdgePadding, 0.0f));
    panel->Overrides().Set(Style::Width, StyleLength::Px(width)); // the window's width, computed
    UIElement* panelRaw = panel.get();
    popover->m_Panel = panelRaw;
    InspectorUI::AddTextBlock(panelRaw, Caption(preview.GetRequest(), preview.ShowsLut()), "cube-lut-preview-caption");
    // A click anywhere on the panel closes it; a press outside it and Escape reach the manager's
    // dismissal (DismissablePopup).
    CubeLutPreviewPopover* popoverRaw = popover.get();
    auto armed = std::make_shared<bool>(false);
    panelRaw->RegisterEventHandler(kEventMouseDown, [armed](UIEvent&) { *armed = true; });
    panelRaw->RegisterEventHandler(kEventMouseCancel, [armed](UIEvent&) { *armed = false; });
    panelRaw->RegisterEventHandler(kEventMouseUp,
                                   [armed, popoverRaw](UIEvent& e)
                                   {
                                       if (!std::exchange(*armed, false))
                                           return;
                                       e.Stop();
                                       popoverRaw->Close();
                                   });
    popover->m_Enlarged =
        CubeLutPreview::Build(panelRaw, preview.GetRequest(), preview.ShowsLut(), CubeLutPreviewSize::Enlarged, nullptr);
    popover->AddChild(std::move(panel));

    popover->m_Open = true;
    popover->Place(anchor->GetLayoutX(), anchor->GetLayoutY(), anchor->GetLayoutY() + anchor->GetLayoutHeight(),
                   window->GetLayoutWidth(), window->GetLayoutHeight());
    window->AddChild(std::move(popover));
    return popoverRaw;
}

void CubeLutPreviewPopover::Place(float anchorLeft, float anchorTop, float anchorBottom, float windowWidth,
                                  float windowHeight)
{
    const float width = m_Panel->GetLayoutWidth() > 0.0f ? m_Panel->GetLayoutWidth()
                                                         : std::min(kPanelWidth, windowWidth - 2.0f * kEdgePadding);
    // Below the preview when it fits, otherwise above it; the window clamps both.
    const bool fitsBelow = anchorBottom + kAnchorGap + kPanelHeightEstimate + kEdgePadding <= windowHeight;
    const float desiredY = fitsBelow ? anchorBottom + kAnchorGap : anchorTop - kAnchorGap - kPanelHeightEstimate;
    const auto placed = UI::Layout::ClampPopupToViewport({0.0f, 0.0f, windowWidth, windowHeight}, anchorLeft, desiredY,
                                                         width, kPanelHeightEstimate, kEdgePadding);
    m_Panel->Overrides()
        .Set(Style::PositionLeft, StyleLength::Px(placed.X))
        .Set(Style::PositionTop, StyleLength::Px(placed.Y));
}

void CubeLutPreviewPopover::SetIntensity(float intensity)
{
    if (m_Enlarged)
        m_Enlarged->SetIntensity(intensity);
}

void CubeLutPreviewPopover::Close()
{
    if (!m_Open)
        return;
    m_Open = false;
    if (m_Owner)
        m_Owner->m_Enlarged = nullptr;
    m_Owner = nullptr;
    Overrides().Set(Style::Display, DisplayMode::None);
    // Removed at the next safe point, never inside the call: the manager's dismissal and the
    // panel's own click handler both run on this popup.
    if (UIElement* parent = GetParent())
        (void)parent->PostSafeAction([parent, self = static_cast<UIElement*>(this)] { parent->RemoveChild(self); });
}

void CubeLutPreviewPopover::CloseForGoneOwner()
{
    m_Owner = nullptr;
    Close();
}

} // namespace GameEngine
