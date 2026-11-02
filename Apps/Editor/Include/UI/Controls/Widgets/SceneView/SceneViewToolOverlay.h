#pragma once

#include "UI/UIElement.h"

namespace GameEngine
{

/// The Scene View's floating tool strip. In its default place the stylesheet centers it on the
/// viewport's height; after layout it moves up to end one inset above the FPS label, and pins one
/// inset below the viewport's top when it cannot fit. A strip the user dragged keeps its place.
class SceneViewToolOverlay final : public UIElement
{
public:
    void OnPostLayout() override;

private:
    // The lowest viewport-local bottom edge the strip may have: one inset above the viewport's
    // bottom, or above the FPS label when the label is visible in the strip's column.
    float LowestBottom(UIElement& viewport, float inset) const;

    // The top this control set in code, or none while the stylesheet centers the strip.
    bool m_HasPinnedTop = false;
    float m_PinnedTop = 0.0f;
};

} // namespace GameEngine
