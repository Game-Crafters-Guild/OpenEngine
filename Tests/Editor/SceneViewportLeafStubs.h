#pragma once

#include "Mathematics/Vector2.h"
#include "UI/Controls/Widgets/SceneView/SceneViewMeasureOverlay.h"

#include <cstddef>
#include <string>

namespace GameEngine
{
class SceneViewController;
class SceneViewport;
class UIElement;
} // namespace GameEngine

namespace GameEngine::Editor::TestStubs
{

/// The text that SceneViewController::GetOverlayHoverText returns in SceneViewportTests, where
/// SceneViewportLeafStubs.cpp defines it. It is empty by default. A test that sets it clears it at the end.
std::string& OverlayHoverText();

/// What one overlay of a pane last read in SceneViewportTests: the stub overlays of SceneViewportLeafStubs.cpp
/// write it on Tick and on each setter call.
struct OverlayRecord
{
    int Ticks = 0;
    const SceneViewController* Controller = nullptr;
    // The rotation gizmo's pane and slot index.
    const SceneViewport* Pane = nullptr;
    std::size_t ViewportIndex = 0;
    // The ruler's cursor.
    Mathematics::Vector2 Cursor;
    bool CursorInside = false;
    // The measure overlay's settings.
    bool MeasureEnabled = false;
    SceneViewMeasureOverlay::UnitSystem Units = SceneViewMeasureOverlay::UnitSystem::Metric;
    SceneViewMeasureOverlay::TwoDMode TwoDMode = SceneViewMeasureOverlay::TwoDMode::Triangle;
};

/// The record of `overlay`, made empty on the first call for that overlay.
OverlayRecord& RecordOf(const UIElement& overlay);
/// The record of `overlay`, or null when the overlay has not written one.
const OverlayRecord* FindRecord(const UIElement* overlay);
/// Removes every record. A test that reads records calls it first: a new overlay can take a freed address.
void ClearRecords();

} // namespace GameEngine::Editor::TestStubs
