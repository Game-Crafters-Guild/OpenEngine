#include "TerrainECS/PageLevelRule.h"

#include "Mathematics/Interpolation.h"


#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace GameEngine::TerrainECS
{
namespace
{

// True when `a` and `b` ask a requester for the same pages: every member equal.
bool SameCamera(const PageRequestCamera& a, const PageRequestCamera& b)
{
    const Mathematics::Vector3& p = a.Position;
    const Mathematics::Vector3& f = a.Forward;
    return p.x == b.Position.x && p.y == b.Position.y && p.z == b.Position.z && f.x == b.Forward.x &&
           f.y == b.Forward.y && f.z == b.Forward.z && a.HorizontalHalfFov == b.HorizontalHalfFov &&
           a.HasDirection == b.HasDirection;
}

constexpr float32 kReferenceRows = static_cast<float32>(kPageLevelReferenceRows);
constexpr float32 kBlendStart = 0.75f;
// A forward shorter than this in the horizontal plane has no bearing: the wedge is the disc.
constexpr float32 kWedgeMinForward = 1e-3f;

// Positive when the bearing of (bx, bz) is greater than the bearing of (ax, az), the two less than
// pi apart. The bearing is atan2(x, z): 0 along +Z, pi / 2 along +X.
float32 BearingTurn(float32 ax, float32 az, float32 bx, float32 bz)
{
    return az * bx - ax * bz;
}

} // namespace

PageLevelView MakePageLevelView(uint32 renderHeight, float32 verticalFovRadians, float32 targetPixelError)
{
    const float32 rows = static_cast<float32>(renderHeight);
    PageLevelView view;
    view.FocalPixels = 0.5f * rows / std::tan(0.5f * verticalFovRadians);
    view.TargetPixels = targetPixelError * rows / kReferenceRows;
    return view;
}

float32 GeometryPageLevel(float32 distance, float32 level0Texel, const PageLevelView& view)
{
    const float32 edge = std::max(distance, std::numeric_limits<float32>::min()) * view.TargetPixels;
    return std::log2(edge / (view.FocalPixels * level0Texel));
}

float32 PageParentBlend(float32 level)
{
    const float32 inRing = level - std::floor(level);
    return Math::Clamp01((inRing - kBlendStart) / (1.0f - kBlendStart));
}

bool SamePagePlacement(const PageRequestTerrain& a, const PageRequestTerrain& b)
{
    return a.OriginX == b.OriginX && a.OriginZ == b.OriginZ && a.Level0TexelX == b.Level0TexelX &&
           a.Level0TexelZ == b.Level0TexelZ && a.SamplesX == b.SamplesX && a.SamplesZ == b.SamplesZ &&
           a.MinHeight == b.MinHeight && a.MaxHeight == b.MaxHeight;
}

uint32 FirstPinnedPageLevel(std::span<const PageStreaming::PageStoreLevel> levels, uint32 faces, float32 level0Texel,
                            uint32 cacheSlots)
{
    if (levels.empty())
        return 0;
    const uint64 cap = cacheSlots / kPinWholePyramidCacheFraction;
    const uint64 pages = faces * PageStreaming::DensePageCount(levels);
    if (pages <= cap)
        return 0;
    // Coarsest first while the pinned pages stay within the cap and the level's texel is at least
    // kPinnedPageTexelMeters; the top level always, so every fallback walk ends on a page.
    uint32 first = static_cast<uint32>(levels.size()) - 1u;
    uint64 pinned = faces * static_cast<uint64>(levels.back().PagesX) * levels.back().PagesZ;
    while (first > 0 && level0Texel * static_cast<float32>(1u << (first - 1u)) >= kPinnedPageTexelMeters)
    {
        const uint64 finer = faces * static_cast<uint64>(levels[first - 1u].PagesX) * levels[first - 1u].PagesZ;
        if (pinned + finer > cap)
            break;
        pinned += finer;
        --first;
    }
    return first;
}

void PageRequester::Configure(const PageRequestTerrain& terrain, PageRequestArea area)
{
    m_Terrain = terrain;
    m_Area = area;
    m_Levels = PageStreaming::BuildPageStoreLevels(terrain.SamplesX, terrain.SamplesZ);
    m_FirstPinned = FirstPinnedPageLevel(m_Levels, 1u, std::max(terrain.Level0TexelX, terrain.Level0TexelZ),
                                         terrain.CacheSlots);
    m_Built = false;
}

PageRequester::Footprint PageRequester::FootprintOf(const PageRequestTerrain& terrain,
                                                    const PageStreaming::PageAddress& address)
{
    const float32 span = static_cast<float32>(PageStreaming::kPageOwnedSamples << address.Level);
    Footprint box;
    box.MinX = terrain.OriginX + static_cast<float32>(address.X) * span * terrain.Level0TexelX;
    box.MinZ = terrain.OriginZ + static_cast<float32>(address.Z) * span * terrain.Level0TexelZ;
    box.MaxX = box.MinX + span * terrain.Level0TexelX;
    box.MaxZ = box.MinZ + span * terrain.Level0TexelZ;
    return box;
}

float32 PageRequester::DistanceToFootprint(const PageRequestTerrain& terrain, const Footprint& box,
                                           const Mathematics::Vector3& camera)
{
    const float32 dx = std::max({box.MinX - camera.x, 0.0f, camera.x - box.MaxX});
    const float32 dz = std::max({box.MinZ - camera.z, 0.0f, camera.z - box.MaxZ});
    const float32 dy = std::max({terrain.MinHeight - camera.y, 0.0f, camera.y - terrain.MaxHeight});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool PageRequester::InWedge(const Footprint& box, const WedgeView& view)
{
    if (view.EveryBearing)
        return true;
    const float32 dx = std::max({box.MinX - view.Position.x, 0.0f, view.Position.x - box.MaxX});
    const float32 dz = std::max({box.MinZ - view.Position.z, 0.0f, view.Position.z - box.MaxZ});
    const float32 pageWidth = std::max(box.MaxX - box.MinX, box.MaxZ - box.MinZ);
    if (std::sqrt(dx * dx + dz * dz) <= kPageWedgeFootPages * pageWidth)
        return true;
    // The camera is outside the rectangle, so its corners span less than pi of bearing: the turn
    // orders them, and the lowest and highest corners bound the rectangle's bearings.
    float32 lowX = box.MinX - view.Position.x;
    float32 lowZ = box.MinZ - view.Position.z;
    float32 highX = lowX;
    float32 highZ = lowZ;
    for (const auto& [x, z] :
         {std::pair{box.MaxX, box.MinZ}, std::pair{box.MinX, box.MaxZ}, std::pair{box.MaxX, box.MaxZ}})
    {
        const float32 cornerX = x - view.Position.x;
        const float32 cornerZ = z - view.Position.z;
        if (BearingTurn(lowX, lowZ, cornerX, cornerZ) < 0.0f)
        {
            lowX = cornerX;
            lowZ = cornerZ;
        }
        if (BearingTurn(highX, highZ, cornerX, cornerZ) > 0.0f)
        {
            highX = cornerX;
            highZ = cornerZ;
        }
    }
    const float32 lowDot = view.ForwardX * lowX + view.ForwardZ * lowZ;
    const float32 highDot = view.ForwardX * highX + view.ForwardZ * highZ;
    if (BearingTurn(lowX, lowZ, view.ForwardX, view.ForwardZ) >= 0.0f &&
        BearingTurn(view.ForwardX, view.ForwardZ, highX, highZ) >= 0.0f && (lowDot > 0.0f || highDot > 0.0f))
        return true; // forward lies between the bounding corners
    // Else the gap to forward is the angle to the nearer bounding corner.
    return lowDot >= view.CosReach * std::sqrt(lowX * lowX + lowZ * lowZ) ||
           highDot >= view.CosReach * std::sqrt(highX * highX + highZ * highZ);
}

float32 PageRequester::NearestDistance(const PageStreaming::PageAddress& address, PageRequestArea area) const
{
    const Footprint box = FootprintOf(m_Terrain, address);
    float32 nearest = std::numeric_limits<float32>::max();
    for (const WedgeView& view : m_Views)
        if (area == PageRequestArea::Disc || InWedge(box, view))
            nearest = std::min(nearest, DistanceToFootprint(m_Terrain, box, view.Position));
    return nearest;
}

void PageRequester::PrepareViews(std::span<const PageRequestCamera> cameras)
{
    m_Views.clear();
    for (const PageRequestCamera& camera : cameras)
    {
        WedgeView view;
        view.Position = camera.Position;
        const float32 forwardLength =
            std::sqrt(camera.Forward.x * camera.Forward.x + camera.Forward.z * camera.Forward.z);
        const float32 reach = camera.HorizontalHalfFov + kPageWedgeMarginRadians;
        // No direction, a view straight up or down, or a reach of pi or more: every bearing.
        view.EveryBearing = !camera.HasDirection || forwardLength < kWedgeMinForward ||
                            reach >= std::numbers::pi_v<float32>;
        if (!view.EveryBearing)
        {
            view.ForwardX = camera.Forward.x / forwardLength;
            view.ForwardZ = camera.Forward.z / forwardLength;
            view.CosReach = std::cos(reach);
        }
        m_Views.push_back(view);
    }
}

bool PageRequester::Build(std::span<const PageRequestCamera> cameras, const PageLevelView& view,
                          std::vector<PageStreaming::PageWant>& out)
{
    // A parked camera asks for the same set: the walk below depends only on these and Configure.
    if (m_Built && view == m_BuiltView && std::ranges::equal(cameras, m_BuiltCameras, SameCamera))
        return false;
    m_Built = true;
    m_BuiltView = view;
    m_BuiltCameras.assign(cameras.begin(), cameras.end());
    out.clear();
    if (m_Levels.empty() || cameras.empty())
        return true;
    PrepareViews(cameras);
    const float32 texel = std::max(m_Terrain.Level0TexelX, m_Terrain.Level0TexelZ);

    // Every page of the pinned levels, coarsest first; the walk below descends from the finest
    // pinned level, and a page enters the list only after its parent (closed under parents).
    m_Frontier.clear();
    for (uint32 level = static_cast<uint32>(m_Levels.size()); level-- > m_FirstPinned;)
    {
        for (uint32 z = 0; z < m_Levels[level].PagesZ; ++z)
        {
            for (uint32 x = 0; x < m_Levels[level].PagesX; ++x)
            {
                const PageStreaming::PageAddress address{0, static_cast<uint8>(level), x, z};
                const float32 distance = NearestDistance(address, PageRequestArea::Disc);
                out.push_back(PageStreaming::PageWant{address, distance, true});
                if (level == m_FirstPinned)
                    m_Frontier.push_back(FrontierPage{
                        address, m_Area == PageRequestArea::Disc ? distance : NearestDistance(address, m_Area)});
            }
        }
    }

    // A frontier page carries its distance over the area, so the walk measures each page once.
    while (!m_Frontier.empty())
    {
        const FrontierPage page = m_Frontier.back();
        m_Frontier.pop_back();
        const uint32 level = page.Address.Level;
        if (level == 0)
            continue;
        if (GeometryPageLevel(page.Distance, texel, view) >= static_cast<float32>(level))
            continue; // the whole footprint is content with this level or coarser
        const PageStreaming::PageStoreLevel& finer = m_Levels[level - 1u];
        for (uint32 dz = 0; dz < 2u; ++dz)
        {
            for (uint32 dx = 0; dx < 2u; ++dx)
            {
                const PageStreaming::PageAddress child{0, static_cast<uint8>(level - 1u), 2u * page.Address.X + dx,
                                                       2u * page.Address.Z + dz};
                if (child.X >= finer.PagesX || child.Z >= finer.PagesZ)
                    continue;
                // Wanted only where its own nearest point needs its level: elsewhere every point of
                // its footprint resolves its parent's level or a coarser one.
                const float32 childDistance = NearestDistance(child, m_Area);
                if (GeometryPageLevel(childDistance, texel, view) >= static_cast<float32>(level))
                    continue;
                out.push_back(PageStreaming::PageWant{child, childDistance, false});
                m_Frontier.push_back(FrontierPage{child, childDistance});
            }
        }
    }
    return true;
}

} // namespace GameEngine::TerrainECS
