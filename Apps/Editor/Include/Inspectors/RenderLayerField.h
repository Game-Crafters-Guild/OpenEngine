#pragma once

#include "Events/Event.h"
#include "Rendering/RenderLayerMask.h"
#include "Types/Types.h"

#include <string>

namespace GameEngine
{
class MaskField;
class UIElement;
} // namespace GameEngine

namespace GameEngine::Editor
{

/// The header tooltip of a renderer's Render Layers row at Nothing.
inline constexpr const char* kRenderLayersNothingTooltip =
    "Drawn by no view. To hide an object, use the entity's enable toggle.";
/// The header tooltip of a reflected layer row at Nothing. Those rows are filters (a probe's or an ocean
/// capture's layers) as well as membership, so the text says only what the value is.
inline constexpr const char* kLayerRowNothingTooltip = "No layer.";
/// The header tooltip of a camera's Visible Layers row at Nothing.
inline constexpr const char* kVisibleLayersNothingTooltip = "Sees no layer.";

/// Gives `field` the project's 32 render layer names as its options and keeps them current: after a rename
/// on Settings > Project > Render Layers, RefreshRenderLayerFields gives every live field fed here the new
/// names. Main thread.
void FeedRenderLayerNames(MaskField& field);

/// Gives every live field FeedRenderLayerNames fed the project's current names, then raises
/// RenderLayerFieldsRefreshed. The settings page calls it after a rename, and the project's render settings
/// apply calls it when a project opens.
void RefreshRenderLayerFields();

/// Raised by RefreshRenderLayerFields, after the names are fed, for a field that shows a value of the open
/// project's own (the Scene View filter). Main thread.
Event<>& RenderLayerFieldsRefreshed();

} // namespace GameEngine::Editor

namespace GameEngine::InspectorUI
{

/// An inspector row that picks render layers by the project's names: the label cell and a MaskField fed
/// with FeedRenderLayerNames. `mixedBits` are the layers a multi-selection disagrees on. The caller wires
/// SetOnBitsChanged.
MaskField* AddRenderLayersRow(UIElement* parent, const std::string& label, Rendering::RenderLayerMask value,
                              uint32 mixedBits, const char* tooltip, const char* nothingTooltip);

} // namespace GameEngine::InspectorUI
