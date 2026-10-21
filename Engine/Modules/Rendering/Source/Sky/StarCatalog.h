#pragma once

#include "Rendering/Sky/StarCatalog.h"

#include <array>
#include <cstdint>
#include <vector>

namespace GameEngine::Rendering
{

/// The faintest star the procedural field holds. Limiting Magnitude may not exceed it.
inline constexpr float kStarFieldFaintestMagnitude = 8.0f;

/// A procedural star field with the statistics of the real naked-eye sky, brightest star first:
/// - counts follow N(< m) = 9,000 x 10^(0.5 (m - 6.5)) over the whole sphere (about 9,000 stars
///   to magnitude 6.5, the naked-eye limit), from Sirius's -1.46 down to kStarFieldFaintestMagnitude;
/// - directions are uniform over the sphere;
/// - colours are blackbodies drawn from the naked-eye spectral mix (B 20 %, A 22 %, F 13 %,
///   G 14 %, K 27 %, M 4 %).
/// The field is seeded: every run, on every platform, draws the same sky.
std::vector<StarInstanceGPU> BuildStarField();

/// How many stars of BuildStarField are at least as bright as the given magnitude: the field is
/// sorted brightest first, so this is the instance count that draws exactly those stars.
uint32_t StarFieldCountBrighterThan(float magnitude);

/// The linear Rec.709 colour of a blackbody at the given temperature in kelvin, scaled to
/// luminance 1: Planck's law integrated against the CIE 1931 2-degree observer (Wyman, Sloan and
/// Shirley's 2013 fit), converted from XYZ, with out-of-gamut negatives clipped.
std::array<float, 3> BlackbodyLinearRec709(float kelvin);

} // namespace GameEngine::Rendering
