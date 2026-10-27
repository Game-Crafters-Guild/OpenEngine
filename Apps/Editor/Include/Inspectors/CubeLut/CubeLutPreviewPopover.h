#pragma once

#include "UI/Interaction/DismissablePopup.h"
#include "UI/UIElement.h"

#include <memory>

namespace GameEngine
{

class CubeLutPreview;

/// The enlarged view of a Cube LUT preview: the same bands and probes at a size where the probes
/// read, in a popover above everything in the window, placed under the preview it enlarges (above
/// it when there is no room below). It closes on a click (inside or outside it) and on Escape, and
/// when the preview it enlarges is destroyed. Its bands name the same LUT, look, stage and
/// intensity as that preview's, so they follow the same commits.
///
/// The element itself is a zero-size anchor at its window's origin, so window coordinates place
/// its panel, and a press anywhere outside the panel is outside the popup.
class CubeLutPreviewPopover final : public UIElement, public DismissablePopup
{
  public:
    /// Opens the enlarged view of `preview` in the preview's window; null when the preview is not
    /// in a window yet.
    static CubeLutPreviewPopover* Open(CubeLutPreview& preview);
    ~CubeLutPreviewPopover() override;

    void OnOwnerManagerChanged(UIManager* owner) override { UpdatePopupRegistration(owner); }
    bool IsPopupOpen() const override { return m_Open; }
    void DismissPopup() override { Close(); }

    void Close();
    /// Follows an Intensity edit of the preview it enlarges.
    void SetIntensity(float intensity);
    /// The preview it enlarges is going away.
    void CloseForGoneOwner();

    UIElement* GetPanel() const { return m_Panel; }

  private:
    CubeLutPreviewPopover();
    void Place(float anchorLeft, float anchorTop, float anchorBottom, float windowWidth, float windowHeight);

    CubeLutPreview* m_Owner = nullptr;
    std::shared_ptr<CubeLutPreview> m_Enlarged;
    UIElement* m_Panel = nullptr;
    bool m_Open = false;
};

} // namespace GameEngine
