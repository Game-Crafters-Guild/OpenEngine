#include "SplineRecipes/Paving/PavingSources.h"

#include "Components/Name.h"
#include "Components/SceneEntityTag.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Mathematics/Geometry.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineNetwork.h"
#include "SplineECS/SplineService.h"
#include "SplineRecipes/Paving/PavingRoadTrim.h"
#include "SplineRecipes/Paving/SplinePavingSanitize.h"
#include "SplineRecipes/SplineExtrudeController.h"
#include "SplineRecipes/SplineSurfaceConform.h"
#include "Types/FormatNumber.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Matrix4x4;
using Mathematics::Vector2;
using Mathematics::Vector3;

// The network's sampling: a sample at most every quarter meter, at least eight per segment.
constexpr float32 kCurveSpacingMeters = 0.25f;
constexpr uint32 kCurveSamplesPerSegment = 8;
constexpr uint32 kMaxCurveSamples = 8192;

std::string_view NameOf(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* name = world.GetComponent<Components::Name>(entity);
    return name ? name->View() : std::string_view("a spline");
}

struct SplineRead
{
    const Spline::SplineData* Data = nullptr;
    Matrix4x4 WorldMatrix;
    SplineObservedInputs Observed{};
    // The strip the spline sweeps, if it has one: a road's width is the top of its strip.
    std::optional<Components::SplineExtrude> Strip;
};

// The rank keys of spline `entity`, whose sampled curve is `curve`.
PavingRank RankOf(const ECS::World& world, ECS::EntityHandle entity, const PavingRoadCurve& curve)
{
    PavingRank rank;
    rank.LengthMeters = curve.Length;
    rank.FirstPoint = Vector2(curve.Points.front().x, curve.Points.front().z);
    if (const auto* tag = world.GetComponent<Components::SceneEntityTag>(entity))
        rank.SceneId = std::string(tag->View());
    rank.Entity = entity.id;
    return rank;
}

// Reads splines and observes each once for the rebuild gate.
class SplineReader
{
public:
    SplineReader(ECS::World& world, PavingWorld& out) : m_World(world), m_Out(out) {}

    // Records the rank keys of road `entity` once.
    void RecordRank(ECS::EntityHandle entity, const PavingRoadCurve& curve)
    {
        const bool known = std::any_of(m_Out.RoadRanks.begin(), m_Out.RoadRanks.end(),
                                       [entity](const PavingRank& rank) { return rank.Entity == entity.id; });
        if (!known)
            m_Out.RoadRanks.push_back(RankOf(m_World, entity, curve));
    }

    std::optional<SplineRead> Read(ECS::EntityHandle entity)
    {
        const auto found = std::find_if(m_Read.begin(), m_Read.end(),
                                        [entity](const auto& entry) { return entry.first == entity.id; });
        if (found != m_Read.end())
            return found->second.Data ? std::optional<SplineRead>(found->second) : std::nullopt;
        SplineRead read;
        auto* service = SplineECS::SplineService::TryGet();
        const auto* spline = m_World.GetComponent<Components::SplineComponent>(entity);
        const auto* transform = m_World.GetComponent<Components::WorldTransform>(entity);
        if (service && spline && transform && m_World.IsValid(entity))
        {
            read.WorldMatrix = transform->ToMatrix4x4(transform->GetPosition() - Mathematics::WorldPosition{});
            read.Observed = ObserveSpline(*service, spline->SplineDataIndex, spline->SplineDataGeneration,
                                          read.WorldMatrix);
            const Spline::SplineData* data =
                service->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
            if (data && data->IsValid() && data->TotalArcLength > 0.0f)
                read.Data = data;
            if (const auto* strip = m_World.GetComponent<Components::SplineExtrude>(entity))
                read.Strip = *strip;
        }
        if (read.Data)
        {
            for (const Spline::SplineControlPoint& knot : read.Data->Points)
                m_Out.CeilingY = std::max(m_Out.CeilingY, read.WorldMatrix.TransformPoint(knot.Position).y);
            m_Out.Bounds.Expand(
                RunWorldBounds(*service, spline->SplineDataIndex, spline->SplineDataGeneration, read.WorldMatrix));
        }
        m_Out.Splines.push_back({entity.id, read.Observed, read.Strip});
        m_Read.emplace_back(entity.id, read);
        return read.Data ? std::optional<SplineRead>(read) : std::nullopt;
    }

private:
    ECS::World& m_World;
    PavingWorld& m_Out;
    std::vector<std::pair<uint32, SplineRead>> m_Read;
};

PavingRoadCurve SampleCurve(ECS::EntityHandle entity, const SplineRead& read)
{
    const Spline::SplineData& data = *read.Data;
    PavingRoadCurve curve;
    curve.Road = entity;
    curve.Closed = data.IsEffectivelyClosed();
    const float32 scale = RoadArcScale(read.WorldMatrix);
    // The strip's top half-width is affine in the width channel: its value at 0 plus a slope.
    float32 topAtZero = 0.0f;
    float32 topPerMeter = 1.0f;
    if (read.Strip)
    {
        topAtZero = SplineExtrudeController::StripTopHalfWidth(*read.Strip, 0.0f);
        topPerMeter = SplineExtrudeController::StripTopHalfWidth(*read.Strip, 1.0f) - topAtZero;
    }
    curve.Length = data.TotalArcLength * scale;
    const uint32 bySpacing = static_cast<uint32>(std::ceil(curve.Length / kCurveSpacingMeters));
    const uint32 intervals =
        std::clamp(std::max(bySpacing, data.GetSegmentCount() * kCurveSamplesPerSegment), 1u, kMaxCurveSamples);
    for (uint32 i = 0; i <= intervals; ++i)
    {
        const float32 distance = data.TotalArcLength * static_cast<float32>(i) / static_cast<float32>(intervals);
        const Spline::SplineFrame frame = Spline::EvaluateAtDistance(data, distance);
        curve.Points.push_back(read.WorldMatrix.TransformPoint(frame.Position));
        curve.ArcLengths.push_back(distance * scale);
        curve.HalfWidths.push_back((topAtZero + topPerMeter * std::max(frame.Radius, 0.0f)) * scale);
    }
    return curve;
}

// A closed spline's ring on the ground, counter-clockwise from above: a linear spline's knots, a
// curved one's samples.
SG::AreaRing ClosedRing(const SplineRead& read, const PavingRoadCurve& curve)
{
    SG::AreaRing ring;
    if (read.Data->Type == Spline::SplineType::Linear)
    {
        for (const Spline::SplineControlPoint& knot : read.Data->Points)
        {
            const Vector3 point = read.WorldMatrix.TransformPoint(knot.Position);
            ring.emplace_back(point.x, point.z);
        }
    }
    else
    {
        for (size_t i = 0; i + 1 < curve.Points.size(); ++i)
            ring.emplace_back(curve.Points[i].x, curve.Points[i].z);
    }
    if (Mathematics::PolygonDoubledSignedArea(ring) < 0.0f)
        std::reverse(ring.begin(), ring.end());
    return ring;
}

// Whether the paving refuses to join `node`. A paved area that grades its ground meets each entering
// road at the road's height, so only a SplitLevel rule keeps the road apart; elsewhere the heights decide.
bool RefusesJoin(const SplineECS::SplineNode& node, bool areaGradesGround)
{
    if (areaGradesGround && node.Kind == SplineECS::SplineNodeKind::Entry)
        return node.Rule == Components::SplineNodeRule::SplitLevel;
    return node.IsSplitLevel();
}

// The meters `road` lies above the other member of `node`; negative below.
float32 RoadAboveMeters(const SplineECS::SplineNode& node, ECS::EntityHandle road)
{
    // HeightDifferenceMetres is the second member's height minus the first's.
    return node.Members[0] == road ? -node.HeightDifferenceMetres : node.HeightDifferenceMetres;
}

std::string SplitLevelNotice(const ECS::World& world, const SplineECS::SplineNode& node, ECS::EntityHandle road,
                             ECS::EntityHandle body)
{
    const float32 roadMinusBody = RoadAboveMeters(node, road);
    const std::string_view roadName = NameOf(world, road);
    const std::string_view bodyName = NameOf(world, body);
    return std::format("\"{}\" is not paved into \"{}\". Its end is {} {} \"{}\", and a road joins only within {}. "
                       "Raise or lower the road's last point, or choose Join at Grade to pave \"{}\" into \"{}\" at "
                       "the height of \"{}\".",
                       roadName, bodyName, FormatMeters(std::fabs(roadMinusBody), 2),
                       roadMinusBody < 0.0f ? "below" : "above", bodyName,
                       FormatMeters(SplineECS::kSplitLevelToleranceMetres, 2), roadName, bodyName, bodyName);
}

float32 MemberArcLength(const SplineECS::SplineNode& node, ECS::EntityHandle member)
{
    return node.Members[0] == member ? node.ArcLengthMetres.x : node.ArcLengthMetres.y;
}

// The arms the junction `node` gives a paving on `self`: on an Area, the entering road's;
// on a Road, every arm of the junction. Each arm records `junction`, the junction's index.
void AddJunctionArms(SplineReader& reader, const SplineECS::SplineNode& node, ECS::EntityHandle self,
                     uint32 junction, PavingSource& source)
{
    const Vector2 nodeXZ(node.Position.x, node.Position.z);
    for (uint32 a = 0; a < node.ArmCount; ++a)
    {
        const SplineECS::SplineNodeArm& arm = node.Arms[a];
        if (node.Kind == SplineECS::SplineNodeKind::Entry && arm.Member == self)
            continue;
        const std::optional<SplineRead> read = reader.Read(arm.Member);
        if (!read)
            continue;
        const PavingRoadCurve curve = SampleCurve(arm.Member, *read);
        reader.RecordRank(arm.Member, curve);
        SG::PavedArm built = BuildPavedArm(curve, MemberArcLength(node, arm.Member), arm.Direction, nodeXZ);
        if (built.Stations.size() < 2u)
            continue;
        source.Arms.push_back(std::move(built));
        source.ArmJunctions.push_back(junction);
    }
}

// Whether `recipe` lists `spline` as an Exclude member.
bool IsExcluded(const Components::SplinePaving& recipe, ECS::EntityHandle spline)
{
    const auto end = recipe.Members + std::min(recipe.MemberCount, Components::kMaxPavingMembers);
    return std::any_of(recipe.Members, end, [spline](const Components::SplinePavingMember& member) {
        return member.Entity == spline && member.Mode == Components::SplinePavingMemberMode::Exclude;
    });
}

} // namespace

bool JunctionHeightsDiffer(const SplineECS::SplineNode& node, const PavingJunctionStatus* junction)
{
    return junction ? junction->State == PavingJunctionState::HeightsDiffer : node.IsSplitLevel();
}

std::string NothingToPaveNotice(std::string_view name)
{
    return std::format("Nothing to pave: {} meets no other road. Draw a road across or into it.", name);
}

// Reads the roads of the junction `node` for the gate, as AddJunctionArms reads them.
void ObserveJunctionArms(SplineReader& reader, const SplineECS::SplineNode& node, ECS::EntityHandle self)
{
    for (uint32 a = 0; a < node.ArmCount; ++a)
    {
        if (node.Kind != SplineECS::SplineNodeKind::Entry || node.Arms[a].Member != self)
            reader.Read(node.Arms[a].Member);
    }
}

PavingWorld GatherPavings(ECS::World& world, PavingGatherDepth depth)
{
    const bool full = depth == PavingGatherDepth::Full;
    PavingWorld out;
    out.CeilingY = -std::numeric_limits<float32>::max();
    SplineReader reader(world, out);
    auto* network = SplineECS::SplineNetwork::TryGet();
    std::vector<ECS::EntityHandle> pavings;
    world.Query<ECS::Read<Components::SplinePaving>, ECS::Read<Components::SplineComponent>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle e, const Components::SplinePaving&, const Components::SplineComponent&,
                  const Components::WorldTransform&) { pavings.push_back(e); });

    for (const ECS::EntityHandle entity : pavings)
    {
        // A paving finds its junctions through the network: off it, it paves nothing.
        const auto* member = world.GetComponent<Components::SplineNetworkMember>(entity);
        const Components::SplineNetworkMemberKind kind = member ? member->Kind : Components::SplineNetworkMemberKind::Road;
        PavingCandidate candidate;
        if (!member && full)
            candidate.Notices.push_back(std::format(
                "{} is not a network member, so it finds no junction. Add Spline Network Member to it.", NameOf(world, entity)));
        candidate.Entity = entity;
        candidate.Authored = *world.GetComponent<Components::SplinePaving>(entity);
        candidate.Recipe = SanitizePavingRecipe(candidate.Authored);
        PavingSource source;
        source.Entity = entity;
        const std::optional<SplineRead> self = reader.Read(entity);
        if (!self)
        {
            out.Candidates.push_back(std::move(candidate));
            out.Sources.push_back(std::move(source));
            continue;
        }
        candidate.WorldMatrix = self->WorldMatrix;
        const bool isArea = kind == Components::SplineNetworkMemberKind::Area;
        const bool areaGradesGround =
            isArea && Components::ResolveGroundAuthority(candidate.Recipe) != Components::SplineGroundAuthority::None;
        if (full)
        {
            const PavingRoadCurve selfCurve = SampleCurve(entity, *self);
            source.Rank = RankOf(world, entity, selfCurve);
            if (isArea && selfCurve.Closed)
                source.Outline = ClosedRing(*self, selfCurve);
        }

        const Components::SplinePaving& recipe = candidate.Recipe;
        source.BorderWidth = recipe.BorderWidth;
        source.MouthRadius = recipe.Mouth == Components::SplinePavingMouth::Flared ? recipe.MouthRadius : 0.0f;
        for (uint32 m = 0; m < recipe.MemberCount; ++m)
        {
            const Components::SplinePavingMember& entry = recipe.Members[m];
            if (entry.Mode == Components::SplinePavingMemberMode::Ignore)
            {
                source.UnpavedRoads.push_back(entry.Entity.id);
                continue;
            }
            const std::optional<SplineRead> read =
                world.IsValid(entry.Entity) ? reader.Read(entry.Entity) : std::nullopt;
            if (!full)
                continue;
            if (!read || !read->Data->IsEffectivelyClosed())
            {
                candidate.Notices.push_back(std::format(
                    "A spline that {} leaves out is not a closed spline in this scene. Switch its row on under "
                    "Splines it touches.",
                    NameOf(world, entity)));
                continue;
            }
            source.Excludes.push_back(ClosedRing(*read, SampleCurve(entry.Entity, *read)));
            source.UnpavedRoads.push_back(entry.Entity.id);
        }

        if (network && member)
        {
            for (const SplineECS::SplineNode* node : network->FindNodes(world, entity))
            {
                const bool paves = isArea ? node->Kind == SplineECS::SplineNodeKind::Entry && node->Members[1] == entity
                                          : node->Kind == SplineECS::SplineNodeKind::Crossing ||
                                                node->Kind == SplineECS::SplineNodeKind::Attachment;
                if (!paves)
                    continue;
                const bool refused = RefusesJoin(*node, areaGradesGround);
                out.Junctions.push_back((uint64{node->Id} << 8) | (uint64{static_cast<uint8>(node->Kind)} << 1) |
                                        (refused ? 1u : 0u));
                if (!full)
                {
                    ObserveJunctionArms(reader, *node, entity);
                    continue;
                }
                const ECS::EntityHandle other = node->Members[0] == entity ? node->Members[1] : node->Members[0];
                PavingJunctionStatus& junction = candidate.Junctions.emplace_back();
                junction.NodeId = node->Id;
                junction.Kind = node->Kind;
                junction.Road = other;
                junction.RoadMeters = MemberArcLength(*node, other);
                const uint32 junctionIndex = static_cast<uint32>(candidate.Junctions.size() - 1u);
                if (refused)
                {
                    junction.State = PavingJunctionState::HeightsDiffer;
                    junction.RoadAboveMeters = RoadAboveMeters(*node, other);
                    candidate.Notices.push_back(SplitLevelNotice(world, *node, other, entity));
                    // The outline is cut under the road, so the road keeps its own height and shares no surface.
                    if (source.Outline.empty())
                        continue;
                    source.UnpavedRoads.push_back(other.id);
                }
                else if (IsExcluded(recipe, other))
                {
                    junction.State = PavingJunctionState::CutOut;
                }
                AddJunctionArms(reader, *node, entity, junctionIndex, source);
            }
        }
        out.Candidates.push_back(std::move(candidate));
        out.Sources.push_back(std::move(source));
    }
    std::sort(out.Splines.begin(), out.Splines.end(),
              [](const PavingSplineObservation& a, const PavingSplineObservation& b) { return a.Entity < b.Entity; });
    return out;
}

} // namespace GameEngine::SplineRecipes
