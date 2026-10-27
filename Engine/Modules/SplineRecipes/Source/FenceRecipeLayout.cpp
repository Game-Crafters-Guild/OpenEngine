#include "SplineRecipes/FenceRecipeLayout.h"

#include "SplineRecipes/RecipeBuildStates.h"

#include <algorithm>

namespace GameEngine::SplineRecipes
{

FencePool FencePoolOf(const Components::SplineFence& recipe, Components::SplineFencePartKind kind)
{
    FencePool pool;
    for (const Components::SplineFencePart& part : recipe.Parts)
    {
        if (part.Kind == kind && !part.Mesh.IsNull())
            pool.Meshes[pool.Count++] = part.Mesh;
    }
    return pool;
}

std::string_view FenceNothingToPlaceReason(const Components::SplineFence& recipe)
{
    const bool hasPart = std::any_of(std::begin(recipe.Parts), std::end(recipe.Parts),
                                     [](const Components::SplineFencePart& part)
                                     {
                                         return part.Kind == Components::SplineFencePartKind::Span ||
                                                part.Kind == Components::SplineFencePartKind::Post;
                                     });
    return hasPart ? kNoFencePartMeshReason : kNoFencePartReason;
}

SplineLayout::FenceLayoutParams FenceLayoutParamsOf(const Components::SplineFence& recipe, const FenceRecipePieces& pieces)
{
    SplineLayout::FenceLayoutParams layout;
    layout.PostPitch = recipe.PostPitch;
    layout.SpanMaxStretch = recipe.SpanMaxStretch;
    layout.Seed = recipe.Seed;
    layout.SpanGrade = recipe.SpanGrade;
    layout.PlantMode = recipe.PlantMode;
    layout.AlignToSurfaceNormal =
        recipe.ConformMode == Components::SplinePlacementConform::HeightAndSlope;
    layout.SlopeBlend = recipe.SlopeBlend;
    layout.HasPostMesh = pieces.PostFootprint != nullptr;
    if (pieces.PostFootprint)
        layout.PostPiece = *pieces.PostFootprint;
    // Spans, gates, crests and caps DO vary in length within a pool — that is
    // the point of the fill, of the cell rule's pitch floor and of the cap fit —
    // so each active slot carries its own bounds.
    layout.SpanPieces = pieces.Spans;
    layout.GatePieces = pieces.Gates;
    layout.SpanOverrides = recipe.Overrides;
    layout.CrestPieces = pieces.Crests;
    layout.CapPieces = pieces.Caps;
    layout.CrestPitch = recipe.CrestPitch;
    return layout;
}

} // namespace GameEngine::SplineRecipes
