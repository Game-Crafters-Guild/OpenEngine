#pragma once

namespace GameEngine
{
class CurveField;
struct InspectorContext;

// The camera inspector's Compensation Curve row (Components::Camera::AutoExposureCompensationCurve):
// the shared curve editor over the metered scene EV100, keys in stops, with the selected key's EV100
// and stops as fields beneath it. A drag is one undo step on the camera and on every other selected
// camera. Returns the graph, or null when the context holds no camera.
CurveField* AddExposureCompensationCurveRow(const InspectorContext& ctx);

} // namespace GameEngine
