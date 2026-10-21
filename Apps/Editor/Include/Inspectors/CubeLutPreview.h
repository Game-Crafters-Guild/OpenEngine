#pragma once

#include "Thumbnails/CubeLutStripRequest.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace GameEngine
{

class CubeLutPreviewPopover;
class UIElement;

/// The stylesheet of the preview and its enlarged view, loaded by both.
inline constexpr const char* kCubeLutPreviewStyleAssetPath = "UI/controls/CubeLutPreview/CubeLutPreview.css";

/// The two sizes a Cube LUT preview is drawn at: under an inspector field, and enlarged.
enum class CubeLutPreviewSize : uint8_t
{
    Inspector,
    Enlarged,
};

/// A Cube LUT preview built into a parent: the neutral exposure ramp and the four saturated probes,
/// each as the look alone over the look with the LUT at its stage and intensity, with each band's
/// name beside it, a mid-gray tick under the ramp and the probes' names under them. Every band is a
/// texture CubeLutThumbnailHandler renders through the real passes. The bands that show the LUT
/// follow an Intensity edit in place (SetIntensity), and so does an open enlarged view.
///
/// A click on the preview runs `onClick` with it: the inspector's opens the enlarged view
/// (OpenEnlarged), the enlarged view's closes it. The preview owns its enlarged view: destroying the preview (an
/// inspector rebuild) closes it, so it never shows settings the inspector no longer shows.
class CubeLutPreview
{
  public:
    /// `request` names the LUT, its owner, the look, the stage and the intensity; its content,
    /// source and size are set per band. A preview without `showLut` (a disabled effect) draws the
    /// look bands only.
    static std::shared_ptr<CubeLutPreview> Build(UIElement* parent, const CubeLutStripRequest& request, bool showLut,
                                                 CubeLutPreviewSize size,
                                                 std::function<void(CubeLutPreview&)> onClick);
    ~CubeLutPreview();

    CubeLutPreview(const CubeLutPreview&) = delete;
    CubeLutPreview& operator=(const CubeLutPreview&) = delete;

    /// Re-points the bands that show the LUT at the textures for a new intensity.
    void SetIntensity(float intensity);

    /// Opens the enlarged view of this preview in its window, or keeps the open one.
    void OpenEnlarged();

    UIElement* GetRoot() const { return m_Root; }
    const CubeLutStripRequest& GetRequest() const { return m_Request; }
    bool ShowsLut() const { return m_ShowLut; }
    CubeLutPreviewPopover* GetEnlarged() const { return m_Enlarged; }

  private:
    friend class CubeLutPreviewPopover;
    CubeLutPreview() = default;

    UIElement* m_Root = nullptr;
    CubeLutStripRequest m_Request{};
    bool m_ShowLut = false;
    // The bands that show the LUT, with the request each one names.
    std::vector<std::pair<UIElement*, CubeLutStripRequest>> m_LutBands;
    CubeLutPreviewPopover* m_Enlarged = nullptr; // cleared by the popover when it closes
};

} // namespace GameEngine
