#pragma once

#include "ECS/Entity.h"
#include "SplineECS/SplineNetwork.h"
#include "SplineRecipes/Paving/PavingPlan.h"
#include "Types/Types.h"

#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
enum class MaterialPatternSymmetry : uint8_t;
}

namespace GameEngine::SplineRecipes
{

// What a paving does at one junction on its spline.
enum class PavingJunctionState : uint8
{
    // It paves the junction.
    Paved = 0,
    // Another paving's region holds the junction (PavingJunctionStatus::PavedBy).
    PavedByOther,
    // The two heights differ by more than a road joins within: nothing is paved there.
    HeightsDiffer,
    // The road is a member with Ignore: nothing is paved there.
    Ignored,
    // The road is a member with Exclude: the paving is cut out along its strip.
    CutOut,
    // The road is too narrow where it meets: nothing is paved there.
    TooNarrow,
};

// One junction a paving reads, as its last rebuild found it.
struct PavingJunctionStatus
{
    // The junction's id in the spline network (SplineECS::SplineNode::Id).
    uint32 NodeId = 0;
    SplineECS::SplineNodeKind Kind = SplineECS::SplineNodeKind::Crossing;
    // The other spline at the junction.
    ECS::EntityHandle Road{};
    // Meters along the other spline from its start to the junction.
    float32 RoadMeters = 0.0f;
    PavingJunctionState State = PavingJunctionState::Paved;
    // The paving that builds the junction, when State is PavedByOther.
    ECS::EntityHandle PavedBy{};
    // When State is HeightsDiffer: the meters the road lies above this spline; negative below.
    float32 RoadAboveMeters = 0.0f;
    // The Blend seam between the road and this paving draws as a hard cut: the road's strip ends near
    // that seam, so the band has no depth.
    bool SeamDrawsHardCut = false;

    bool operator==(const PavingJunctionStatus&) const = default;
};

// A paving as its last rebuild left it: what it does, its junctions, and the notices that
// state each fix.
struct PavingStatus
{
    ECS::EntityHandle Entity{};
    PavingRole Role = PavingRole::NothingToPave;
    // The paving that builds every region it reaches, when Role is PavedBy.
    ECS::EntityHandle PavedBy{};
    std::vector<PavingJunctionStatus> Junctions;
    std::vector<std::string> Notices;
    // The pattern symmetry of the material that draws this paving's Seam at its last rebuild: the
    // material of the paving that owns the lid. It selects the Blend form (MaterialDocument.h). Empty
    // when no seam takes this paving's Seam, or when SeamPatternMixed is set.
    std::optional<MaterialPatternSymmetry> SeamPatternSymmetry;
    // The seams that take this paving's Seam read materials with different pattern symmetries.
    bool SeamPatternMixed = false;
    // The paving whose Material draws every seam that takes this paving's Seam; invalid when no seam
    // takes it or when the seams lie in the lids of more than one paving.
    ECS::EntityHandle SeamMaterialPaving{};

    bool operator==(const PavingStatus&) const = default;
};

} // namespace GameEngine::SplineRecipes
