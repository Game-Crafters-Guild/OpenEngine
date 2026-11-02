#pragma once

#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplinePaving.h"
#include "ECS/ECS.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "SplineRecipes/Paving/PavingPlan.h"
#include "SplineRecipes/Paving/PavingStatus.h"
#include "SplineRecipes/SplineRebuildGate.h"

#include <optional>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::SplineRecipes
{

// A spline the paving reads, as the rebuild gate observes it.
struct PavingSplineObservation
{
    uint32 Entity = 0;
    SplineObservedInputs Spline{};
    // The strip it sweeps (SplineExtrude), if any: the top of the strip sets the road's width.
    std::optional<Components::SplineExtrude> Strip;

    bool operator==(const PavingSplineObservation&) const = default;
};

// One paving of a world, read for the plan.
struct PavingCandidate
{
    ECS::EntityHandle Entity{};
    Components::SplinePaving Authored{};
    Components::SplinePaving Recipe{};
    Mathematics::Matrix4x4 WorldMatrix;
    // The junctions it does not pave and the members that are gone, in the words the author reads,
    // each stating the fix.
    std::vector<std::string> Notices;
    // Every junction it reads: HeightsDiffer where the heights differ, else Paved until the plan
    // says who paves it.
    std::vector<PavingJunctionStatus> Junctions;
};

// Every paving of a world with its source for the plan, in entity order.
struct PavingWorld
{
    std::vector<PavingCandidate> Candidates;
    std::vector<PavingSource> Sources;
    // Every spline a source read (its own, the roads of its junctions, its Exclude members), by
    // entity id, each once: what a rebuild depends on besides the recipes and the ground.
    std::vector<PavingSplineObservation> Splines;
    // The rank keys of every road a junction arm reads, each once; read at Full depth only.
    std::vector<PavingRank> RoadRanks;
    // Every junction a source read, as (id << 8) | (kind << 1) | split level, in reading order: a
    // junction gained, lost or split changes it.
    std::vector<uint64> Junctions;
    // The highest point of every spline read, in world meters: a conform ray starts above it.
    float32 CeilingY = 0.0f;
    // The world bounds of every spline read: the ground a rebuild reads lies in them.
    Mathematics::AABB Bounds = Mathematics::AABB::Empty();
};

// How much GatherPavings reads.
enum class PavingGatherDepth : uint8
{
    // The candidates' recipes, the splines, the junction keys and the bounds: what the gate compares.
    Observe = 0,
    // All of it, with the sources, the junctions and the notices a rebuild reads.
    Full,
};

// The pavings on network members (SplineNetworkMember) of `world`: on an Area, its outline and
// the arms of every road that enters it; on a Road, the arms of every road that crosses it or ends
// on it. A junction whose heights differ (SplineNode::IsSplitLevel) is not paved and is reported,
// except a road that enters an area that grades its ground: the ground meets the road at its height.
PavingWorld GatherPavings(ECS::World& world, PavingGatherDepth depth);

// Whether the heights at `node` keep its road apart: the state of `junction`, the node's record in the
// paving that reads it; where no paving reads the node (null), the network's heights decide.
bool JunctionHeightsDiffer(const SplineECS::SplineNode& node, const PavingJunctionStatus* junction);

// The notice for a paving that has nothing to pave.
std::string NothingToPaveNotice(std::string_view name);

} // namespace GameEngine::SplineRecipes
