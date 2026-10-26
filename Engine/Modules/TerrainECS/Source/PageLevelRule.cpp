#include "TerrainECS/PageLevelRule.h"

#include "Mathematics/Interpolation.h"


#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::TerrainECS
{
namespace
{

constexpr float32 kReferenceRows = static_cast<float32>(kPageLevelReferenceRows);
constexpr float32 kBlendStart = 0.75f;

// Distance from `camera` to the box of a page's footprint: its world rectangle and the field's
// height range.
float32 DistanceToFootprint(const PageRequestTerrain& terrain, const PageStreaming::PageAddress& address,
                            const Mathematics::Vector3& camera)
{
    const float32 span = static_cast<float32>(PageStreaming::kPageOwnedSamples << address.Level);
    const float32 minX = terrain.OriginX + static_cast<float32>(address.X) * span * terrain.Level0TexelX;
    const float32 minZ = terrain.OriginZ + static_cast<float32>(address.Z) * span * terrain.Level0TexelZ;
    const float32 maxX = minX + span * terrain.Level0TexelX;
    const float32 maxZ = minZ + span * terrain.Level0TexelZ;
    const float32 dx = std::max({minX - camera.x, 0.0f, camera.x - maxX});
    const float32 dz = std::max({minZ - camera.z, 0.0f, camera.z - maxZ});
    const float32 dy = std::max({terrain.MinHeight - camera.y, 0.0f, camera.y - terrain.MaxHeight});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float32 NearestDistance(const PageRequestTerrain& terrain, const PageStreaming::PageAddress& address,
                        std::span<const Mathematics::Vector3> cameras)
{
    float32 nearest = std::numeric_limits<float32>::max();
    for (const Mathematics::Vector3& camera : cameras)
        nearest = std::min(nearest, DistanceToFootprint(terrain, address, camera));
    return nearest;
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

uint32 FirstPinnedPageLevel(const PageRequestTerrain& terrain, std::span<const PageStreaming::PageStoreLevel> levels)
{
    if (levels.empty())
        return 0;
    const uint64 cap = terrain.CacheSlots / kPinWholePyramidCacheFraction;
    const uint64 pages = static_cast<uint64>(levels.back().FirstEntry) + levels.back().PagesX * levels.back().PagesZ;
    if (pages <= cap)
        return 0;
    // Coarsest first while the pinned pages stay within the cap and the level's texel is at least
    // kPinnedPageTexelMeters; the top level always, so every fallback walk ends on a page.
    const float32 texel = std::max(terrain.Level0TexelX, terrain.Level0TexelZ);
    uint32 first = static_cast<uint32>(levels.size()) - 1u;
    uint64 pinned = static_cast<uint64>(levels.back().PagesX) * levels.back().PagesZ;
    while (first > 0 && texel * static_cast<float32>(1u << (first - 1u)) >= kPinnedPageTexelMeters)
    {
        const uint64 finer = static_cast<uint64>(levels[first - 1u].PagesX) * levels[first - 1u].PagesZ;
        if (pinned + finer > cap)
            break;
        pinned += finer;
        --first;
    }
    return first;
}

void HeightPageRequester::Configure(const PageRequestTerrain& terrain)
{
    m_Terrain = terrain;
    m_Levels = PageStreaming::BuildPageStoreLevels(terrain.SamplesX, terrain.SamplesZ);
    m_FirstPinned = FirstPinnedPageLevel(terrain, m_Levels);
}

void HeightPageRequester::Build(std::span<const Mathematics::Vector3> cameras, const PageLevelView& view,
                                std::vector<PageStreaming::PageWant>& out)
{
    out.clear();
    if (m_Levels.empty() || cameras.empty())
        return;
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
                out.push_back(PageStreaming::PageWant{address, NearestDistance(m_Terrain, address, cameras), true});
                if (level == m_FirstPinned)
                    m_Frontier.push_back(address);
            }
        }
    }

    while (!m_Frontier.empty())
    {
        const PageStreaming::PageAddress page = m_Frontier.back();
        m_Frontier.pop_back();
        if (page.Level == 0)
            continue;
        const float32 distance = NearestDistance(m_Terrain, page, cameras);
        if (GeometryPageLevel(distance, texel, view) >= static_cast<float32>(page.Level))
            continue; // the whole footprint is content with this level or coarser
        const PageStreaming::PageStoreLevel& finer = m_Levels[page.Level - 1u];
        for (uint32 dz = 0; dz < 2u; ++dz)
        {
            for (uint32 dx = 0; dx < 2u; ++dx)
            {
                const PageStreaming::PageAddress child{0, static_cast<uint8>(page.Level - 1u), 2u * page.X + dx,
                                                       2u * page.Z + dz};
                if (child.X >= finer.PagesX || child.Z >= finer.PagesZ)
                    continue;
                // Wanted only where its own nearest point needs its level: elsewhere every point of
                // its footprint resolves its parent's level or a coarser one.
                const float32 childDistance = NearestDistance(m_Terrain, child, cameras);
                if (GeometryPageLevel(childDistance, texel, view) >= static_cast<float32>(page.Level))
                    continue;
                out.push_back(PageStreaming::PageWant{child, childDistance, false});
                m_Frontier.push_back(child);
            }
        }
    }
}

} // namespace GameEngine::TerrainECS
