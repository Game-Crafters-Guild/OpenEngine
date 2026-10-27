#pragma once

#include "Components/Spline/SplineFence.h"
#include "SplineLayout/FenceLayout.h"

#include <span>
#include <string_view>

namespace GameEngine::SplineRecipes
{

// The meshes of a fence's parts of one kind, in list order: that role's pool.
// Parts without a mesh are not in it. Index n is the PoolSlot an override
// names.
struct FencePool
{
    Components::ModelRef Meshes[Components::kSplineFenceMaxParts];
    uint32 Count = 0;
};

[[nodiscard]] FencePool FencePoolOf(const Components::SplineFence& recipe, Components::SplineFencePartKind kind);

// The bounds a fence rebuild measured one part's mesh by, keyed by the mesh.
struct FenceMeshBounds
{
    Components::ModelRef Mesh;
    SplineLayout::FencePieceBounds Bounds;
};

// Why a fence with no Span or Post part with a mesh places nothing: kNoFencePartReason when it has
// no such part, else kNoFencePartMeshReason.
[[nodiscard]] std::string_view FenceNothingToPlaceReason(const Components::SplineFence& recipe);

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
