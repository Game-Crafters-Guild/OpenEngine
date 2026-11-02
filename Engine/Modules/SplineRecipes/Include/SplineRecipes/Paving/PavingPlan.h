#pragma once

#include "ECS/Entity.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "SplineGeometry/PavedFootprint.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <vector>

namespace GameEngine::SplineRecipes
{

// How far an arm reads its road out from the node, in world meters. Past it the road goes on and
// the arm's cap is Open wherever its stop falls; a stop never lies this far out.
inline constexpr float32 kPavedArmReachMeters = 64.0f;

// A member road's centerline sampled in world space.
struct PavingRoadCurve
{
    ECS::EntityHandle Road{};
    std::vector<Mathematics::Vector3> Points;
    // World meters from the road's start to each point: the network's arc lengths.
    std::vector<float32> ArcLengths;
    // Meters to each side of the centerline at each point: the top of the road's strip where it has one.
    std::vector<float32> HalfWidths;
    bool Closed = false;
    float32 Length = 0.0f;
};

// The arm that leaves the node at `nodeArcLength` along `road` toward its end (+1) or its start
// (-1): the road's points out to kPavedArmReachMeters, or to its end, which the arm then ends.
SplineGeometry::PavedArm BuildPavedArm(const PavingRoadCurve& road, float32 nodeArcLength, int8 direction,
                                       const Mathematics::Vector2& node);

// A spline's keys under the network's ordering rule: the greater length, then the smaller x and z of
// the first point, then the smaller saved scene id, then the smaller entity id.
struct PavingRank
{
    float32 LengthMeters = 0.0f;
    Mathematics::Vector2 FirstPoint{};
    std::string SceneId;
    uint32 Entity = 0;
};

// One paving as the plan reads it: its rank keys, its outline and the arms of its junctions.
struct PavingSource
{
    ECS::EntityHandle Entity{};
    PavingRank Rank;
    // An Area's outline, paved whole; empty on a Road.
    SplineGeometry::AreaRing Outline;
    // The arms of every junction it paves; PavedArm::Road is the road's entity id.
    std::vector<SplineGeometry::PavedArm> Arms;
    // Per arm, the junction it leaves, by its index in PavingCandidate::Junctions.
    std::vector<uint32> ArmJunctions;
    // The closed members it cuts out (Exclude), and the roads it does not pave: its Ignore and Exclude
    // members and the roads whose heights differ where they meet it. Each is cut out along its strip.
    std::vector<SplineGeometry::AreaRing> Excludes;
    std::vector<uint32> UnpavedRoads;
    // Meters the border reaches out from the paving's edge.
    float32 BorderWidth = 0.0f;
    // Meters of the arcs that round each mouth where a road leaves Outline; 0 keeps a sharp corner.
    float32 MouthRadius = 0.0f;
};

// A source arm the plan does not pave: its road is in PavingSource::UnpavedRoads.
inline constexpr uint32 kNoPavedArm = ~0u;

// What a paving does in the plan.
enum class PavingRole : uint8
{
    // It owns at least one region and builds it.
    Owns = 0,
    // Every region it reaches is another paving's (PavingPlan::PavedBy).
    PavedBy,
    // A Road with no junction: nothing to pave.
    NothingToPave,
};

// The regions of every paving in a world, each with one owner.
struct PavingPlan
{
    SplineGeometry::PavedFootprint Footprint;
    // Per region, the source that builds it.
    std::vector<uint32> RegionOwners;
    // Per footprint arm, its road's entity id; Footprint.Arms[i] is that arm's stop.
    std::vector<uint32> ArmRoads;
    // Per footprint arm, the arm the footprint was built from.
    std::vector<SplineGeometry::PavedArm> Arms;
    // Per footprint outline, its ring: PavedRegion::Outlines holds these indices.
    std::vector<SplineGeometry::AreaRing> Outlines;
    // Per source, per arm of it, the footprint arm it became, or kNoPavedArm.
    std::vector<std::vector<uint32>> SourceArms;
    // Per source, its role, and for PavedBy the source that builds its first region.
    std::vector<PavingRole> Roles;
    std::vector<uint32> PavedBy;
};

// Whether `a` ranks before `b` under the network's ordering rule.
bool RanksBefore(const PavingRank& a, const PavingRank& b);

// The regions of `sources` as one footprint: a shared arm counts once and overlapping junctions make one
// region. An unpaved road is cut out with a curb beside it on an outline, and flush on a Road. An
// outline's source owns its region (the first by rank); the first-ranked arm source owns any other.
PavingPlan PlanPavings(std::span<const PavingSource> sources);

} // namespace GameEngine::SplineRecipes
