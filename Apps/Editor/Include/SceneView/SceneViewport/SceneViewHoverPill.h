#pragma once

#include "Mathematics/Vector2.h"
#include "SceneView/SceneViewport/SceneViewHoverPillLabels.h"
#include "UI/Controls/Label.h"

#include <string>

namespace GameEngine
{

/// The hover pill of one Scene View pane, declared as `<SceneViewHoverPill />` in the pane's layout: the name of
/// the object under the pointer, or the text of the active tool. Its parent is the pane. Main thread only.
class SceneViewHoverPill final : public Label
{
public:
    /// The top-left corner of a pill of `pillSize` in a pane of `paneSize`, in pane pixels, rounded to whole pixels.
    /// Beside the pointer, the pill sits below and to the right of `pointer`; it goes above the pointer when the
    /// pill below would leave the pane or meet a label. At the bottom center, it sits 14 px above the pane's bottom.
    /// Then a pill that meets a label moves up above that label. A pill meets a label when it comes closer than
    /// 4 px to it, and it stays 4 px inside the pane's edges.
    static Mathematics::Vector2 Place(bool besidePointer, const Mathematics::Vector2& pointer,
                                      const Mathematics::Vector2& paneSize, const Mathematics::Vector2& pillSize,
                                      const SceneViewHoverPillLabels& labels);

    /// Shows `text` beside `pointer` (pane pixels), or at the bottom center of the pane when `besidePointer` is
    /// false, clear of `labels` (Place). The pill moves only when the text, the placement or the pill width changes,
    /// or when the pane size, a label box or the pointer moves by 1 px or more.
    void Show(const std::string& text, bool besidePointer, const Mathematics::Vector2& pointer,
              const SceneViewHoverPillLabels& labels);
    /// Hides the pill. The next Show places it again.
    void Hide();

private:
    // The placement of the last Show; a pane width of -1 means that the pill is not placed.
    Mathematics::Vector2 m_PlacedPaneSize{-1.0f, -1.0f};
    float m_PlacedPillWidth = -1.0f;
    SceneViewHoverPillLabels m_PlacedLabels;
    bool m_PlacedBesidePointer = false;
    Mathematics::Vector2 m_PlacedPointer;
};

} // namespace GameEngine
