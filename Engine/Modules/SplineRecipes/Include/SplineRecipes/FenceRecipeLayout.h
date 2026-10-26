#pragma once

#include "Components/Spline/SplineFence.h"
#include "SplineLayout/FenceLayout.h"

#include <span>

namespace GameEngine::SplineRecipes
{

// The bounds a fence recipe's resolved pools present to the layout, each in
// active-slot order. The post family lays its stations out on one footprint —
// a pool is a set of interchangeable cross-sections — so a post pool with
// members hands over its first; null when the post pool is empty.
struct FenceRecipePieces
{
    const SplineLayout::FencePieceBounds* PostFootprint = nullptr;
    std::span<const SplineLayout::FencePieceBounds> Spans;
    std::span<const SplineLayout::FencePieceBounds> Gates;
    std::span<const SplineLayout::FencePieceBounds> Crests;
    std::span<const SplineLayout::FencePieceBounds> Caps;
};

// Everything a fence layout takes from its recipe, over the recipe's resolved
// pieces: pitches, stretch, grade, planting, seed, the span overrides and every
// pool. The caller adds what the scene decides — the run boundaries, closure,
// the ground probe — and the piece budget. The returned spans point into
// `recipe` and `pieces`, which must outlive the layout call.
SplineLayout::FenceLayoutParams FenceLayoutParamsOf(const Components::SplineFence& recipe,
                                      const FenceRecipePieces& pieces);

} // namespace GameEngine::SplineRecipes
