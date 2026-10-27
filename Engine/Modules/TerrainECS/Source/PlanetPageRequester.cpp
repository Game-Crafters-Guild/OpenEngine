#include "TerrainECS/PlanetPageRequester.h"

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "Types/Types.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>

namespace GameEngine::TerrainECS
{
namespace
{

// A direction or a position relative to the planet's center, in float64 (Mathematics has no
// float64 vector).
struct Planet64
{
    float64 X = 0.0;
    float64 Y = 0.0;
    float64 Z = 0.0;
};

float64 Dot(const Planet64& a, const Planet64& b)
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

Planet64 Cross(const Planet64& a, const Planet64& b)
{
    return Planet64{a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}

float64 Length(const Planet64& a)
{
    return std::sqrt(Dot(a, a));
}

Planet64 Scaled(const Planet64& a, float64 scale)
{
    return Planet64{a.X * scale, a.Y * scale, a.Z * scale};
}

// The angle between two directions, accurate at every size.
float64 AngleBetween(const Planet64& a, const Planet64& b)
{
    return std::atan2(Length(Cross(a, b)), Dot(a, b));
}

// The unit direction of level-0 sample (x, z) of `face`: the direction the page's samples use.
Planet64 SampleDirection(uint8 face, uint32 x, uint32 z, uint32 samples)
{
    const Mathematics::Vector3 direction = PageStreaming::EqualAngleSampleDirection(face, x, z, samples);
    const Planet64 wide{direction.x, direction.y, direction.z};
    return Scaled(wide, 1.0 / Length(wide));
}

// The angle from unit direction `u` to the shorter great-circle arc from `a` to `b`, whose plane
// has the unit normal `normal` along a x b.
float64 AngleToArc(const Planet64& u, const Planet64& a, const Planet64& b, const Planet64& normal)
{
    const float64 offPlane = Dot(u, normal);
    const Planet64 inPlane{u.X - offPlane * normal.X, u.Y - offPlane * normal.Y, u.Z - offPlane * normal.Z};
    if (Length(inPlane) > 0.0 && Dot(Cross(a, inPlane), normal) >= 0.0 && Dot(Cross(inPlane, b), normal) >= 0.0)
        return std::asin(std::min(std::abs(offPlane), 1.0));
    return std::min(AngleBetween(u, a), AngleBetween(u, b));
}

// A page's geometry on the sphere: its corners in order around it (its sides great-circle arcs,
// less than a hemisphere), each side's unit normal, its height shell and its texel.
struct PageShape
{
    std::array<Planet64, 4> Corners;
    std::array<Planet64, 4> SideNormals; ///< along Corners[side] x Corners[side + 1]
    float64 Winding = 1.0;               ///< +1 when the region lies along the side normals, else -1
    float64 Low = 0.0;                   ///< R + the page's lowest height
    float64 High = 0.0;                  ///< R + the page's highest height
    float64 PointReach = 0.0;            ///< acos(floor / High): the horizon's reach past the floor
    float32 Texel = 0.0f;
    uint64 LastFrame = 0;                ///< the last Build that visited the page
};

// The angle from unit direction `u` to the page's region: 0 inside.
float64 AngleToRegion(const Planet64& u, const PageShape& shape)
{
    bool inside = true;
    for (uint32 side = 0; side < 4u && inside; ++side)
        inside = shape.Winding * Dot(shape.SideNormals[side], u) >= 0.0;
    if (inside)
        return 0.0;
    float64 nearest = std::numeric_limits<float64>::max();
    for (uint32 side = 0; side < 4u; ++side)
        nearest = std::min(nearest, AngleToArc(u, shape.Corners[side], shape.Corners[(side + 1u) % 4u],
                                               shape.SideNormals[side]));
    return nearest;
}

// The geometry of `address` on `planet`, whose floor sphere has radius `floorRadius`.
PageShape BuildPageShape(const PageStreaming::PageAddress& address, const PlanetPageRequestTerrain& planet,
                         float64 floorRadius)
{
    const uint32 samples = planet.FaceSamples;
    const uint32 span = PageStreaming::kPageOwnedSamples << address.Level;
    // The last page of a row owns only the face's last sample: its region is the last interval.
    const uint32 x1 = std::min(address.X * span + span, samples - 1u);
    const uint32 z1 = std::min(address.Z * span + span, samples - 1u);
    const uint32 x0 = std::min(address.X * span, x1 - 1u);
    const uint32 z0 = std::min(address.Z * span, z1 - 1u);
    PageShape shape;
    std::array<Planet64, 4>& corners = shape.Corners;
    corners = {SampleDirection(address.Face, x0, z0, samples), SampleDirection(address.Face, x1, z0, samples),
               SampleDirection(address.Face, x1, z1, samples), SampleDirection(address.Face, x0, z1, samples)};
    for (uint32 side = 0; side < 4u; ++side)
    {
        const Planet64 across = Cross(corners[side], corners[(side + 1u) % 4u]);
        shape.SideNormals[side] = Scaled(across, 1.0 / Length(across));
    }
    // The winding is read against the region's own center, so the antipode is never inside.
    const Planet64 center{corners[0].X + corners[2].X, corners[0].Y + corners[2].Y, corners[0].Z + corners[2].Z};
    shape.Winding = Dot(shape.SideNormals[0], center) >= 0.0 ? 1.0 : -1.0;
    const PageStreaming::PageHeightRange heights = planet.Heights->PlanetPageHeightRange(address);
    shape.Low = planet.ReferenceRadius + heights.Min;
    shape.High = planet.ReferenceRadius + heights.Max;
    shape.PointReach = std::acos(std::clamp(floorRadius / shape.High, -1.0, 1.0));
    // The largest level-0 sample spacing along the page's sides, at its highest radius.
    const float64 alongX = std::max(AngleBetween(corners[0], corners[1]), AngleBetween(corners[3], corners[2])) /
                           static_cast<float64>(std::max(x1 - x0, 1u));
    const float64 alongZ = std::max(AngleBetween(corners[0], corners[3]), AngleBetween(corners[1], corners[2])) /
                           static_cast<float64>(std::max(z1 - z0, 1u));
    shape.Texel = static_cast<float32>(std::max(alongX, alongZ) * shape.High);
    return shape;
}

uint64 PageKey(const PageStreaming::PageAddress& address)
{
    return static_cast<uint64>(address.Face) << 56u | static_cast<uint64>(address.Level) << 48u |
           static_cast<uint64>(address.X) << 24u | static_cast<uint64>(address.Z);
}

// The cache holds at most this many times the pages one Build visits; past it, the pages the last
// Build did not visit are dropped.
constexpr std::size_t kShapeCacheWorkingSets = 4u;

} // namespace

uint32 PlanetFaceSamples(float64 referenceRadius, float32 level0Texel)
{
    if (!(referenceRadius > 0.0) || !(level0Texel > 0.0f))
        return 0;
    const float64 intervals = std::ceil(0.5 * std::numbers::pi * referenceRadius / static_cast<float64>(level0Texel));
    if (intervals >= static_cast<float64>(PageStreaming::kPageStoreMaxSamplesPerAxis))
        return 0;
    // The power of two that leaves at most kPageOwnedSamples intervals, then the fewest of those.
    uint64 power = 1u;
    while (static_cast<float64>(power * PageStreaming::kPageOwnedSamples) < intervals)
        power *= 2u;
    const uint64 multiple = std::max<uint64>(static_cast<uint64>(std::ceil(intervals / static_cast<float64>(power))), 4u);
    const uint64 samples = multiple * power + 1u;
    return samples > PageStreaming::kPageStoreMaxSamplesPerAxis ? 0u : static_cast<uint32>(samples);
}

struct PlanetPageRequester::PageShapes
{
    FastHashMap<uint64, PageShape> Pages;
    uint64 Frame = 0;
    std::size_t Visited = 0; ///< the pages this Build visited
};

PlanetPageRequester::PlanetPageRequester()
    : m_Shapes(std::make_unique<PageShapes>())
{
}

PlanetPageRequester::~PlanetPageRequester() = default;
PlanetPageRequester::PlanetPageRequester(PlanetPageRequester&&) noexcept = default;
PlanetPageRequester& PlanetPageRequester::operator=(PlanetPageRequester&&) noexcept = default;

void PlanetPageRequester::Configure(const PlanetPageRequestTerrain& planet)
{
    m_Planet = planet;
    m_Shapes->Pages.clear();
    assert(planet.Heights && planet.Heights->FaceCount() == PageStreaming::kPageStoreMaxFaces &&
           planet.Heights->Levels().front().SamplesX == planet.FaceSamples &&
           "a planet's requester reads its page heights from its generator");
    m_Levels = PageStreaming::BuildPageStoreLevels(planet.FaceSamples, planet.FaceSamples);
    float32 lowest = 0.0f;
    const uint8 top = static_cast<uint8>(m_Levels.size() - 1u);
    for (uint8 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
        lowest = std::min(lowest, planet.Heights->PlanetPageHeightRange(PageStreaming::PageAddress{face, top, 0, 0}).Min);
    m_FloorRadius = planet.ReferenceRadius + lowest;
    const float64 level0Arc = 0.5 * std::numbers::pi / static_cast<float64>(std::max(planet.FaceSamples, 2u) - 1u);
    m_FirstPinned = FirstPinnedPageLevel(m_Levels, PageStreaming::kPageStoreMaxFaces,
                                         static_cast<float32>(level0Arc * planet.ReferenceRadius), planet.CacheSlots);
}

bool PlanetPageRequester::NearestReach(const PageStreaming::PageAddress& address, float32& outDistance,
                                       float32& outTexel)
{
    const auto [entry, added] = m_Shapes->Pages.try_emplace(PageKey(address));
    PageShape& shape = entry->second;
    if (added)
        shape = BuildPageShape(address, m_Planet, m_FloorRadius);
    if (shape.LastFrame != m_Shapes->Frame)
        ++m_Shapes->Visited;
    shape.LastFrame = m_Shapes->Frame;
    outTexel = shape.Texel;

    float64 nearest = std::numeric_limits<float64>::max();
    for (const Camera& camera : m_Cameras)
    {
        const Planet64 position{camera.X, camera.Y, camera.Z};
        const Planet64 up = camera.Length > 0.0 ? Scaled(position, 1.0 / camera.Length) : shape.Corners[0];
        const float64 angle = AngleToRegion(up, shape);
        if (!camera.UnderFloor && angle > camera.FloorAngle + shape.PointReach)
            continue;
        // The nearest point at that angle: the radius nearest the camera's foot, within the shell.
        const float64 cosine = std::cos(angle);
        const float64 radius = std::clamp(camera.Length * cosine, shape.Low, shape.High);
        const float64 squared = camera.Length * camera.Length + radius * radius - 2.0 * camera.Length * radius * cosine;
        nearest = std::min(nearest, std::sqrt(std::max(squared, 0.0)));
    }
    outDistance = static_cast<float32>(std::min(nearest, static_cast<float64>(std::numeric_limits<float32>::max())));
    return nearest != std::numeric_limits<float64>::max();
}

void PlanetPageRequester::Build(std::span<const Mathematics::Vector3> cameras, const PageLevelView& view,
                                std::vector<PageStreaming::PageWant>& out)
{
    out.clear();
    if (m_Levels.empty() || cameras.empty())
        return;
    ++m_Shapes->Frame;
    m_Shapes->Visited = 0;
    m_Cameras.clear();
    for (const Mathematics::Vector3& position : cameras)
    {
        Camera& camera = m_Cameras.emplace_back(Camera{position.x, position.y, position.z});
        camera.Length = Length(Planet64{camera.X, camera.Y, camera.Z});
        camera.UnderFloor = camera.Length <= m_FloorRadius;
        if (!camera.UnderFloor)
            camera.FloorAngle = std::acos(m_FloorRadius / camera.Length);
    }

    // Every page of the pinned levels on every face, coarsest first; the walk descends from the
    // finest pinned level, and a page enters the list only after its parent (closed under parents).
    m_Frontier.clear();
    float32 distance = 0.0f;
    float32 texel = 0.0f;
    for (uint8 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
        for (uint32 level = static_cast<uint32>(m_Levels.size()); level-- > m_FirstPinned;)
            for (uint32 z = 0; z < m_Levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < m_Levels[level].PagesX; ++x)
                {
                    const PageStreaming::PageAddress address{face, static_cast<uint8>(level), x, z};
                    NearestReach(address, distance, texel);
                    out.push_back(PageStreaming::PageWant{address, distance, true});
                    if (level == m_FirstPinned)
                        m_Frontier.push_back(address);
                }

    while (!m_Frontier.empty())
    {
        const PageStreaming::PageAddress page = m_Frontier.back();
        m_Frontier.pop_back();
        if (page.Level == 0)
            continue;
        if (!NearestReach(page, distance, texel) ||
            GeometryPageLevel(distance, texel, view) >= static_cast<float32>(page.Level))
            continue; // under every camera's horizon, or content with this level or coarser
        const PageStreaming::PageStoreLevel& finer = m_Levels[page.Level - 1u];
        for (uint32 dz = 0; dz < 2u; ++dz)
            for (uint32 dx = 0; dx < 2u; ++dx)
            {
                const PageStreaming::PageAddress child{page.Face, static_cast<uint8>(page.Level - 1u),
                                                       2u * page.X + dx, 2u * page.Z + dz};
                if (child.X >= finer.PagesX || child.Z >= finer.PagesZ)
                    continue;
                if (!NearestReach(child, distance, texel) ||
                    GeometryPageLevel(distance, texel, view) >= static_cast<float32>(page.Level))
                    continue;
                out.push_back(PageStreaming::PageWant{child, distance, false});
                m_Frontier.push_back(child);
            }
    }

    if (m_Shapes->Pages.size() > kShapeCacheWorkingSets * m_Shapes->Visited)
    {
        const uint64 frame = m_Shapes->Frame;
        erase_if(m_Shapes->Pages, [frame](const auto& page) { return page.second.LastFrame != frame; });
    }
}

} // namespace GameEngine::TerrainECS
