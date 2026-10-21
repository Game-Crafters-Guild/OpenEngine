#pragma once

#include "Mathematics/Curve.h"

namespace GameEngine::Components
{

// Auto exposure's compensation curve: stops added to the exposure (+ brightens) as a function of the
// scene EV100 the meter reads, before the Min/Max EV clamps. Key Time is the metered EV100, key Value
// the compensation in stops; outside the keys the curve holds its end values. It applies on top of
// Camera::ExposureCompensation, after the clamps, so it still acts when the exposure is held at the
// floor.
//
// The default, keyed on the Picture meter's readings of the Basic 3D template (Game View camera):
// - EV100 0.5 and brighter: 0. Daylight, shade, dusk and a lamp-lit room (one 60 W lamp reads 0.8) are
//   left as metered.
// - EV100 -4.25 (a full moon at 21:00) and darker: -2.5, so the moonlit ground sits about 4 stops under
//   middle gray, the moon reads bright and the stars keep their contrast. A moonless night reads below
//   the Auto Min EV floor (-7), so it is held there and shows darker still.
// Two smooth keys with flat tangents: the curve is monotone.
inline Math::Curve DefaultExposureCompensationCurve()
{
    Math::Curve curve{};
    curve.TryInsert(Math::CurveKey{-4.25f, -2.5f, 0.0f, 0.0f, Math::CurveInterp::Smooth, Math::CurveTangentMode::Flat});
    curve.TryInsert(Math::CurveKey{0.5f, 0.0f, 0.0f, 0.0f, Math::CurveInterp::Smooth, Math::CurveTangentMode::Flat});
    return curve;
}

} // namespace GameEngine::Components
