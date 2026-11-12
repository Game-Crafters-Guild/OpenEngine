#pragma once

#include "Mathematics/Rect.h"

namespace GameEngine
{

/// The boxes of the labels over one Scene View pane that its hover pill keeps clear of, in pixels from the pane's
/// top-left corner. An empty box is no label.
struct SceneViewHoverPillLabels
{
    /// The pane's own label. Its top is the top of the pane's bottom label row.
    Mathematics::Rect PaneLabel;
    /// The host's label over the pane (the FPS readout), at any place in the pane.
    Mathematics::Rect HostLabel;
};

} // namespace GameEngine
