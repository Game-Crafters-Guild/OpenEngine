#include "PavingSeam.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Interpolation.h"
#include "PavingLidPieces.h"
#include "PavingRoadUV.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::SplineRecipes
{
namespace
{

using Mathematics::Vector2;
using Mathematics::Vector4;

// The validity rule's limits: the angle between the axes (cos 30 degrees) and the pitch difference.
constexpr float32 kMinAxisCosine = 0.8660254f;
constexpr float32 kMaxPitchDifference = 0.01f;
// Half the step, in meters, of the central differences that read a field's gradient.
constexpr float32 kGradientStepMeters = 0.05f;
// A road's lid more than this far past a seam takes the field across it.
constexpr float32 kClipSlackMeters = 2.0e-3f;
// A lid point this close to a seam is on it, for the validity rule's last term.
constexpr float32 kOnSeamMeters = 0.01f;
constexpr float32 kJointLineMeters = 0.03f;
// Cuts across the band: per tile of band for the projected form, which bends the UV; two for the
// crossfade, whose weight is linear.
constexpr float32 kProjectedCutsPerTile = 6.0f;
constexpr uint32 kCrossfadeCuts = 2u;
constexpr float32 kMinimumPitch = 1.0e-6f;
// A band shallower than 5 cm is not cut: it is a sliver that reads as a hard cut, so the seam draws one.
constexpr float32 kMinimumBandMeters = 0.05f;
// The share of the band a crossfade takes: its fade reads as a blur, so it stays short.
constexpr float32 kCrossfadeBandShare = 0.5f;
// Points read across a strip cap, edge to edge.
constexpr uint32 kCapSamples = 5u;
// The course's tint when it draws the paving's own material.
const Vector4 kJointLineTint(0.38f, 0.38f, 0.40f, 1.0f);
const Vector4 kStoneCourseTint(1.25f, 1.25f, 1.25f, 1.0f);

Vector2 Scale(const Vector2& a, const Vector2& b)
{
    return Vector2(a.x * b.x, a.y * b.y);
}

Vector2 Unscale(const Vector2& a, const Vector2& b)
{
    return Vector2(a.x / b.x, a.y / b.y);
}

// The cosine between `moved` and `yielding` when their lengths agree within the pitch limit, else -1.
float32 AlignedCosine(const Vector2& moved, const Vector2& yielding)
{
    const float32 movedPitch = moved.Length();
    const float32 yieldingPitch = yielding.Length();
    if (movedPitch < kMinimumPitch || yieldingPitch < kMinimumPitch)
        return -1.0f;
    if (std::abs(movedPitch - yieldingPitch) > kMaxPitchDifference * std::max(movedPitch, yieldingPitch))
        return -1.0f;
    return Vector2::Dot(moved, yielding) / (movedPitch * yieldingPitch);
}

// `road`'s centerline point, its direction and its top's half-widths at arc length `distance`.
struct RoadStation
{
    Vector2 Point{};
    Vector2 Forward{1.0f, 0.0f};
    float32 HalfLeft = 0.0f;
    float32 HalfRight = 0.0f;
};

RoadStation StationAt(const PavingRoadUV& road, float32 distance)
{
    RoadStation station;
    for (size_t i = 0; i + 1 < road.Points.size(); ++i)
    {
        const float32 length = road.Distance[i + 1] - road.Distance[i];
        if (distance > road.Distance[i + 1] && i + 2 < road.Points.size())
            continue;
        const float32 t = length > kMinimumPitch ? Math::Clamp01((distance - road.Distance[i]) / length) : 0.0f;
        const Vector2 delta = road.Points[i + 1] - road.Points[i];
        station.Point = road.Points[i] + delta * t;
        station.Forward = delta.Length() > kMinimumPitch ? delta.Normalize() : station.Forward;
        station.HalfLeft = Math::Lerp(road.HalfLeft[i], road.HalfLeft[i + 1], t);
        station.HalfRight = Math::Lerp(road.HalfRight[i], road.HalfRight[i + 1], t);
        break;
    }
    return station;
}

Vector2 Round(const Vector2& tiles)
{
    return Vector2(std::round(tiles.x), std::round(tiles.y));
}

} // namespace

Vector2 SeamAxisMove::Apply(const Vector2& tiles) const
{
    return Scale(Swap ? Vector2(tiles.y, tiles.x) : tiles, Sign);
}

std::optional<SeamAxisMove> SolveSeamAxisMove(MaterialPatternSymmetry symmetry, const SeamFieldFrame& yielding,
                                              const SeamFieldFrame& other)
{
    if (symmetry == MaterialPatternSymmetry::None)
        return std::nullopt;
    const Vector2 kSigns[] = {Vector2(1.0f, 1.0f), Vector2(-1.0f, 1.0f), Vector2(1.0f, -1.0f), Vector2(-1.0f, -1.0f)};
    std::optional<SeamAxisMove> best;
    float32 bestCosine = -std::numeric_limits<float32>::max();
    for (const bool swap : {false, true})
    {
        if (swap && symmetry != MaterialPatternSymmetry::Square)
            continue;
        for (const Vector2& sign : kSigns)
        {
            const Vector2 movedU = (swap ? other.GradientV : other.GradientU) * sign.x;
            const Vector2 movedV = (swap ? other.GradientU : other.GradientV) * sign.y;
            const float32 cosineU = AlignedCosine(movedU, yielding.GradientU);
            const float32 cosineV = AlignedCosine(movedV, yielding.GradientV);
            if (cosineU < kMinAxisCosine || cosineV < kMinAxisCosine || cosineU + cosineV <= bestCosine)
                continue;
            bestCosine = cosineU + cosineV;
            best = SeamAxisMove{swap, sign};
        }
    }
    return best;
}

PavingSeams::PavingSeams(std::span<const PavingRoadUV> roads, std::span<const PavingRoadSeam> seams,
                         std::span<const SplineGeometry::AreaRing> outlines, const PavingOwnMapping& own,
                         const PavingSeamMaterial& material)
    : m_Roads(roads), m_Seams(seams), m_Outlines(outlines), m_Own(own), m_Material(material),
      m_BandMeters(roads.size() * (roads.size() + 1u), 0.0f),
      m_CrossfadeMeters(roads.size() * (roads.size() + 1u), 0.0f), m_Projections(roads.size() * (roads.size() + 1u))
{
    for (uint32 road = 0; road < RoadCount() && road < m_Seams.size(); ++road)
    {
        const float32 tilesPerMeter = std::max(kMinimumPitch, m_Roads[road].TilesPerMeterU * m_Material.TilesPerUV.x);
        const float32 bandMeters = m_Seams[road].BandTiles / tilesPerMeter;
        const std::vector<float32> capMeters = CapMeters(road);
        for (uint32 partner = 0; partner <= OwnField(); ++partner)
        {
            const float32 band = std::min(bandMeters, capMeters[partner]);
            const float32 crossfade = std::min(bandMeters * kCrossfadeBandShare, capMeters[partner]);
            m_BandMeters[SlotOf(road, partner)] = band >= kMinimumBandMeters ? band : 0.0f;
            m_CrossfadeMeters[SlotOf(road, partner)] = crossfade >= kMinimumBandMeters ? crossfade : 0.0f;
        }
    }
}

bool PavingSeams::DrawsHardCut(uint32 road, uint32 partner) const
{
    const uint32 field = partner == kOwnMapping ? OwnField() : partner;
    if (road >= RoadCount() || road >= m_Seams.size() || field > OwnField() || !HasPartner(road, field) ||
        m_Seams[road].Look != Components::SplinePavingSeam::Blend)
        return false;
    // A band of 0 tiles is the setting's own hard cut, not the strip's.
    if (m_Seams[road].BandTiles <= 0.0f)
        return false;
    const bool projected = m_Projections[SlotOf(road, field)].has_value();
    return (projected ? BandMeters(road, field) : CrossfadeMeters(road, field)) <= 0.0f;
}

void PavingSeams::Cut(LidPieces& pieces)
{
    for (uint32 road = 0; road < RoadCount() && road < m_Seams.size(); ++road)
    {
        const Components::SplinePavingSeam look = m_Seams[road].Look;
        if (look == Components::SplinePavingSeam::HardCut)
            continue;
        ClipOnSeams(pieces, road);
        if (look == Components::SplinePavingSeam::Blend)
        {
            SolveProjections(pieces, road);
            CutBand(pieces, road);
            continue;
        }
        CutCourse(pieces, road);
    }
}

PavingSeamVertex PavingSeams::VertexOf(uint32 mapping, const Vector2& xz) const
{
    PavingSeamVertex vertex;
    if (mapping == kOwnMapping)
    {
        vertex.UV = FieldUV(OwnField(), xz);
        vertex.UDirection = m_Own.UAxis;
        return vertex;
    }
    const uint32 roads = RoadCount();
    if (mapping < roads)
    {
        const PavingRoadPoint located = LocateOnRoad(m_Roads[mapping], xz);
        vertex.UV = BlendUV(mapping, xz);
        vertex.UDirection = located.Forward;
        if (vertex.UV == located.UV)
            return vertex;
        // A projected band bends U: the tangent follows its rise.
        const Vector2 dx(kGradientStepMeters, 0.0f);
        const Vector2 dz(0.0f, kGradientStepMeters);
        const Vector2 rise(BlendUV(mapping, xz + dx).x - BlendUV(mapping, xz - dx).x,
                           BlendUV(mapping, xz + dz).x - BlendUV(mapping, xz - dz).x);
        if (rise.Length() > kMinimumPitch)
            vertex.UDirection = rise.Normalize();
        return vertex;
    }
    if (mapping < 2u * roads)
    {
        const uint32 road = mapping - roads;
        const NearestSeam nearest = NearestSeamOf(road, xz);
        const float32 width = CourseMeters(road, nearest.Partner);
        const SeamAlong along = AlongSeam(road, nearest.Partner, xz);
        const float32 across = std::clamp(nearest.Meters, 0.0f, width);
        const Vector2 uvPerMeter = CourseUVPerMeter(road, nearest.Partner, width);
        if (!m_Seams[road].HasCourseMaterial)
            vertex.Tint = m_Seams[road].Look == Components::SplinePavingSeam::JointLine ? kJointLineTint : kStoneCourseTint;
        if (!TurnsStones(road))
        {
            vertex.UV = Scale(Vector2(along.Meters, across), uvPerMeter);
            vertex.UDirection = along.Direction;
            return vertex;
        }
        // A quarter turn of the other field, which keeps its handedness: U rises across the seam.
        vertex.UV = Scale(Vector2(across, -along.Meters), uvPerMeter);
        vertex.UDirection = AcrossSeam(nearest.Partner, xz, along.Direction);
        return vertex;
    }
    const uint32 road = mapping - 2u * roads;
    const NearestSeam nearest = NearestSeamOf(road, xz);
    const PavingRoadPoint located = LocateOnRoad(m_Roads[road], xz);
    vertex.UV = located.UV;
    vertex.UDirection = located.Forward;
    vertex.SecondUV = FieldUV(nearest.Partner, xz);
    const float32 crossfade = CrossfadeMeters(road, nearest.Partner);
    vertex.Tint.w = crossfade > 0.0f ? 1.0f - Math::Clamp01(nearest.Meters / crossfade) : 0.0f;
    return vertex;
}

bool PavingSeams::IsProjected(uint32 road, uint32 partner) const
{
    const uint32 field = partner == kOwnMapping ? OwnField() : partner;
    return road < RoadCount() && field <= OwnField() && m_Projections[SlotOf(road, field)].has_value();
}

uint32 PavingSeams::MappingOfField(uint32 field) const
{
    return field == OwnField() ? kOwnMapping : field;
}

Vector2 PavingSeams::FieldUV(uint32 field, const Vector2& xz) const
{
    if (field < RoadCount())
        return LocateOnRoad(m_Roads[field], xz).UV;
    return Vector2(Vector2::Dot(xz, m_Own.UAxis), Vector2::Dot(xz, m_Own.VAxis)) + m_Own.Phase;
}

Vector2 PavingSeams::FieldTiles(uint32 field, const Vector2& xz) const
{
    return Scale(FieldUV(field, xz), m_Material.TilesPerUV);
}

SeamFieldFrame PavingSeams::FrameOf(uint32 field, const Vector2& xz) const
{
    const Vector2 dx(kGradientStepMeters, 0.0f);
    const Vector2 dz(0.0f, kGradientStepMeters);
    const Vector2 alongX = (FieldTiles(field, xz + dx) - FieldTiles(field, xz - dx)) / (2.0f * kGradientStepMeters);
    const Vector2 alongZ = (FieldTiles(field, xz + dz) - FieldTiles(field, xz - dz)) / (2.0f * kGradientStepMeters);
    return SeamFieldFrame{Vector2(alongX.x, alongZ.x), Vector2(alongX.y, alongZ.y)};
}

float32 PavingSeams::SeamMeters(uint32 partner, const Vector2& xz) const
{
    if (partner < RoadCount())
        return -LocateOnRoad(m_Roads[partner], xz).Inside;
    return OutsideOutlines(m_Outlines, xz);
}

PavingSeams::NearestSeam PavingSeams::NearestSeamOf(uint32 road, const Vector2& xz) const
{
    NearestSeam nearest{OwnField(), std::numeric_limits<float32>::max()};
    for (uint32 partner = 0; partner <= OwnField(); ++partner)
    {
        if (!HasPartner(road, partner))
            continue;
        const float32 meters = SeamMeters(partner, xz);
        if (meters < nearest.Meters)
            nearest = NearestSeam{partner, meters};
    }
    return nearest;
}

std::vector<float32> PavingSeams::NearestSeamMeters(const LidPieces& pieces, uint32 road) const
{
    std::vector<float32> meters(pieces.Points.size());
    for (size_t p = 0; p < pieces.Points.size(); ++p)
        meters[p] = NearestSeamOf(road, Vector2(pieces.Points[p].x, pieces.Points[p].z)).Meters;
    return meters;
}

std::vector<float32> PavingSeams::CapMeters(uint32 road) const
{
    const PavingRoadUV& uv = m_Roads[road];
    std::vector<float32> nearest(OwnField() + 1u, std::numeric_limits<float32>::max());
    if (uv.Points.size() < 2u)
        return nearest;
    // Each span ends at the node and at a strip cap. The node lies past the seam, so only the cap
    // reads a distance.
    for (const Vector2& span : uv.Spans)
    {
        for (const float32 end : {span.x, span.y})
        {
            const RoadStation station = StationAt(uv, end);
            const Vector2 left(-station.Forward.y, station.Forward.x);
            for (uint32 sample = 0; sample < kCapSamples; ++sample)
            {
                const float32 across = static_cast<float32>(sample) / static_cast<float32>(kCapSamples - 1u);
                const Vector2 point =
                    station.Point + left * Math::Lerp(station.HalfLeft, -station.HalfRight, across);
                const NearestSeam seam = NearestSeamOf(road, point);
                if (seam.Meters > 0.0f)
                    nearest[seam.Partner] = std::min(nearest[seam.Partner], seam.Meters);
            }
        }
    }
    return nearest;
}

float32 PavingSeams::CourseMeters(uint32 road, uint32 partner) const
{
    const PavingRoadSeam& seam = m_Seams[road];
    if (seam.CourseWidth > 0.0f)
        return seam.CourseWidth;
    if (seam.Look == Components::SplinePavingSeam::JointLine)
        return kJointLineMeters;
    // Half a tile of the field across the seam, on the axis that runs across the course: V, or U where
    // the stones turn. The own mapping is in meters.
    const bool turned = TurnsStones(road);
    const float32 uvPerMeter =
        partner < RoadCount() ? (turned ? m_Roads[partner].TilesPerMeterU : m_Roads[partner].TilesPerMeterV) : 1.0f;
    return 0.5f / std::max(kMinimumPitch, uvPerMeter * (turned ? m_Material.TilesPerUV.x : m_Material.TilesPerUV.y));
}

Vector2 PavingSeams::CourseUVPerMeter(uint32 road, uint32 partner, float32 width) const
{
    if (m_Seams[road].HasCourseMaterial)
        return Vector2(1.0f, 1.0f) / width;
    // Over the paving's material the stones are the size of the other field's tiles.
    if (partner < RoadCount())
        return Vector2(m_Roads[partner].TilesPerMeterU, m_Roads[partner].TilesPerMeterV);
    // The own mapping is in meters on the ground.
    return Vector2(1.0f, 1.0f);
}

bool PavingSeams::TurnsStones(uint32 road) const
{
    return m_Seams[road].Look == Components::SplinePavingSeam::StoneCourse && !m_Seams[road].HasCourseMaterial;
}

Vector2 PavingSeams::AcrossSeam(uint32 partner, const Vector2& xz, const Vector2& along) const
{
    const Vector2 side(-along.y, along.x);
    const Vector2 step = side * kGradientStepMeters;
    return SeamMeters(partner, xz + step) >= SeamMeters(partner, xz - step) ? side : side * -1.0f;
}

PavingSeams::SeamAlong PavingSeams::AlongSeam(uint32 road, uint32 partner, const Vector2& xz) const
{
    if (partner < RoadCount())
    {
        const PavingRoadUV& across = m_Roads[partner];
        const PavingRoadPoint located = LocateOnRoad(across, xz);
        return SeamAlong{located.UV.x / std::max(kMinimumPitch, across.TilesPerMeterU), located.Forward};
    }
    // An outline seam runs across the yielding road.
    const Vector2 forward = LocateOnRoad(m_Roads[road], xz).Forward;
    const Vector2 side(-forward.y, forward.x);
    return SeamAlong{Vector2::Dot(xz, side), side};
}

Vector2 PavingSeams::BlendUV(uint32 road, const Vector2& xz) const
{
    const Vector2 uv = LocateOnRoad(m_Roads[road], xz).UV;
    if (m_Seams[road].Look != Components::SplinePavingSeam::Blend)
        return uv;
    const NearestSeam nearest = NearestSeamOf(road, xz);
    const float32 band = BandMeters(road, nearest.Partner);
    if (band <= 0.0f)
        return uv;
    const std::optional<Projection>& projection = m_Projections[SlotOf(road, nearest.Partner)];
    if (!projection || nearest.Meters >= band)
        return uv;
    // F = F_B + smoothstep(1 - s / L) (G - F_B): the other field's lines on the seam, this road's at the
    // band's far edge with zero slope.
    const float32 weight = Math::SmoothStep(0.0f, 1.0f, 1.0f - std::max(nearest.Meters, 0.0f) / band);
    const Vector2 moved =
        Unscale(projection->Move.Apply(FieldTiles(nearest.Partner, xz)) + projection->ShiftTiles, m_Material.TilesPerUV);
    return Mathematics::Lerp(uv, moved, weight);
}

std::optional<Vector2> PavingSeams::CrossingOf(uint32 road, uint32 partner) const
{
    const std::vector<Vector2>& points = m_Roads[road].Points;
    for (size_t i = 0; i + 1 < points.size(); ++i)
    {
        const float32 from = SeamMeters(partner, points[i]);
        const float32 to = SeamMeters(partner, points[i + 1]);
        if ((from < 0.0f) == (to < 0.0f))
            continue;
        return points[i] + (points[i + 1] - points[i]) * (from / (from - to));
    }
    return std::nullopt;
}

size_t PavingSeams::SlotOf(uint32 road, uint32 partner) const
{
    return static_cast<size_t>(road) * (RoadCount() + 1u) + partner;
}

bool PavingSeams::HasPartner(uint32 road, uint32 partner) const
{
    return partner == OwnField() ? !m_Outlines.empty() : partner < road;
}

void PavingSeams::ClipOnSeams(LidPieces& pieces, uint32 road) const
{
    std::vector<float32> margin(pieces.Points.size());
    for (uint32 partner = 0; partner <= OwnField(); ++partner)
    {
        if (!HasPartner(road, partner))
            continue;
        margin.resize(pieces.Points.size());
        for (size_t p = 0; p < pieces.Points.size(); ++p)
            margin[p] = -SeamMeters(partner, Vector2(pieces.Points[p].x, pieces.Points[p].z)) - kClipSlackMeters;
        CutAlongZero(pieces, margin, road, MappingOfField(partner));
    }
}

void PavingSeams::SolveProjections(const LidPieces& pieces, uint32 road)
{
    // The road's lid points on a seam, read once for every partner.
    std::vector<std::pair<Vector2, uint32>> onSeam;
    std::vector<bool> visited(pieces.Points.size(), false);
    for (size_t i = 0; i < pieces.Triangles.size(); ++i)
    {
        const uint32 point = pieces.Triangles[i];
        if (pieces.TriangleMapping[i / 3u] != road || visited[point])
            continue;
        visited[point] = true;
        const Vector2 xz(pieces.Points[point].x, pieces.Points[point].z);
        const NearestSeam nearest = NearestSeamOf(road, xz);
        if (std::abs(nearest.Meters) <= kOnSeamMeters)
            onSeam.emplace_back(xz, nearest.Partner);
    }

    for (uint32 partner = 0; partner <= OwnField(); ++partner)
    {
        std::optional<Projection>& projection = m_Projections[SlotOf(road, partner)];
        projection.reset();
        if (!HasPartner(road, partner))
            continue;
        const std::optional<Vector2> crossing = CrossingOf(road, partner);
        if (!crossing)
            continue;
        const std::optional<SeamAxisMove> move =
            SolveSeamAxisMove(m_Material.Symmetry, FrameOf(road, *crossing), FrameOf(partner, *crossing));
        if (!move)
            continue;
        const Vector2 shift = Round(FieldTiles(road, *crossing) - move->Apply(FieldTiles(partner, *crossing)));
        bool holds = true;
        for (const auto& [xz, seamPartner] : onSeam)
        {
            if (seamPartner != partner)
                continue;
            const Vector2 offset = move->Apply(FieldTiles(partner, xz)) + shift - FieldTiles(road, xz);
            holds &= std::abs(offset.x) < kSeamMaxOffsetTiles && std::abs(offset.y) < kSeamMaxOffsetTiles;
        }
        if (holds)
            projection = Projection{*move, shift};
    }
}

void PavingSeams::CutBand(LidPieces& pieces, uint32 road) const
{
    // The cut levels of each partner's seam: the projection's across its band, the crossfade's across its
    // shorter one. A level cuts the whole lid of the road; a cut near another seam only adds points.
    const uint32 projectedCuts =
        std::max(2u, static_cast<uint32>(std::ceil(m_Seams[road].BandTiles * kProjectedCutsPerTile)));
    std::vector<float32> levels;
    for (uint32 partner = 0; partner <= OwnField(); ++partner)
    {
        if (!HasPartner(road, partner))
            continue;
        const bool bends = m_Projections[SlotOf(road, partner)].has_value();
        const float32 depth = bends ? BandMeters(road, partner) : CrossfadeMeters(road, partner);
        const uint32 cuts = bends ? projectedCuts : kCrossfadeCuts;
        if (depth <= 0.0f)
            continue;
        for (uint32 cut = 1; cut <= cuts; ++cut)
            levels.push_back(depth * static_cast<float32>(cut) / static_cast<float32>(cuts));
    }
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

    std::vector<float32> meters = NearestSeamMeters(pieces, road);
    std::vector<float32> margin;
    for (const float32 level : levels)
    {
        margin.resize(meters.size());
        for (size_t p = 0; p < meters.size(); ++p)
            margin[p] = level - meters[p];
        const size_t pointsBefore = pieces.Points.size();
        CutAlongZero(pieces, margin, road, road);
        for (size_t p = pointsBefore; p < pieces.Points.size(); ++p)
            meters.push_back(NearestSeamOf(road, Vector2(pieces.Points[p].x, pieces.Points[p].z)).Meters);
    }

    // A band the projection does not bend takes the crossfade. A crossfade of no depth has no band.
    for (size_t t = 0; t < pieces.TriangleMapping.size(); ++t)
    {
        if (pieces.TriangleMapping[t] != road)
            continue;
        const Mathematics::Vector3 centroid =
            (pieces.Points[pieces.Triangles[t * 3u]] + pieces.Points[pieces.Triangles[t * 3u + 1u]] +
             pieces.Points[pieces.Triangles[t * 3u + 2u]]) / 3.0f;
        const NearestSeam nearest = NearestSeamOf(road, Vector2(centroid.x, centroid.z));
        const float32 crossfade = CrossfadeMeters(road, nearest.Partner);
        if (crossfade > 0.0f && nearest.Meters < crossfade && !m_Projections[SlotOf(road, nearest.Partner)])
            pieces.TriangleMapping[t] = BandMapping(road);
    }
}

void PavingSeams::CutCourse(LidPieces& pieces, uint32 road) const
{
    std::vector<float32> margin(pieces.Points.size());
    for (size_t p = 0; p < pieces.Points.size(); ++p)
    {
        const NearestSeam nearest = NearestSeamOf(road, Vector2(pieces.Points[p].x, pieces.Points[p].z));
        margin[p] = CourseMeters(road, nearest.Partner) - nearest.Meters;
    }
    CutAlongZero(pieces, margin, road, CourseMapping(road));
}

} // namespace GameEngine::SplineRecipes
