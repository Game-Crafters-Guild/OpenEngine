#pragma once

#include "UI/Controls/Widgets/SceneView/SceneViewMeasureOverlay.h"

namespace GameEngine
{

/// The measure tool's settings a host pushes to each of its panes: one choice for every pane of the host.
struct SceneViewportMeasureSettings
{
    bool Enabled = false;
    SceneViewMeasureOverlay::UnitSystem Units = SceneViewMeasureOverlay::UnitSystem::Metric;
    SceneViewMeasureOverlay::TwoDMode TwoDMode = SceneViewMeasureOverlay::TwoDMode::Triangle;
    /// A finished measure drag creates a measure entity in the pane's world.
    bool CreateEntities = false;
};

} // namespace GameEngine
