#include "TerrainECS/TerrainSurfacePageBaker.h"

#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainSurfaceBakeInputs.h"
#include "TerrainECS/TerrainSurfaceStore.h"

#include "PageStreaming/PageStore/PageStoreAccess.h"
#include "PageStreaming/PageStore/PageStorePatcher.h"
#include "PageStreaming/PageStore/PageStoreReader.h"
#include "PageStreaming/PageStore/PageStoreWriter.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <optional>

namespace GameEngine::TerrainECS
{

namespace
{

using PageStreaming::kPageApronSamples;
using PageStreaming::kPageOwnedSamples;
using PageStreaming::PageAddress;
using PageStreaming::PageSampleRect;
using PageStreaming::PageStoreLayout;
using PageStreaming::PageStoreLevel;

// The height pages of one level that a set of surface pages reads: a dense rect of pages, each empty
// while it is not on hand.
struct FetchedHeightPages
{
    uint32 Level = 0;
    uint32 FirstX = 0;
    uint32 FirstZ = 0;
    uint32 CountX = 0;
    uint32 CountZ = 0;
    std::vector<std::vector<float32>> Pages;

    const float32* Find(uint32 x, uint32 z) const
    {
        if (x < FirstX || z < FirstZ || x - FirstX >= CountX || z - FirstZ >= CountZ)
            return nullptr;
        const std::vector<float32>& page = Pages[static_cast<std::size_t>(z - FirstZ) * CountX + (x - FirstX)];
        return page.empty() ? nullptr : page.data();
    }
};

// The dense rect of height pages of `level` next to or under one of `pages` (not empty).
FetchedHeightPages HeightFetchRect(const PageStoreLevel& shape, uint32 level, std::span<const PageAddress> pages)
{
    FetchedHeightPages rect;
    rect.Level = level;
    uint32 minX = pages.front().X, maxX = pages.front().X, minZ = pages.front().Z, maxZ = pages.front().Z;
    for (const PageAddress& page : pages)
    {
        minX = std::min(minX, page.X);
        maxX = std::max(maxX, page.X);
        minZ = std::min(minZ, page.Z);
        maxZ = std::max(maxZ, page.Z);
    }
    rect.FirstX = minX == 0u ? 0u : minX - 1u;
    rect.FirstZ = minZ == 0u ? 0u : minZ - 1u;
    rect.CountX = std::min(maxX + 1u, shape.PagesX - 1u) - rect.FirstX + 1u;
    rect.CountZ = std::min(maxZ + 1u, shape.PagesZ - 1u) - rect.FirstZ + 1u;
    return rect;
}

// Adds the height pages of HeightFetchRect to `out`.
void AddHeightFetchRect(const PageStoreLevel& shape, uint32 level, std::span<const PageAddress> pages,
                        std::vector<PageAddress>& out)
{
    if (pages.empty())
        return;
    const FetchedHeightPages rect = HeightFetchRect(shape, level, pages);
    for (uint32 z = 0; z < rect.CountZ; ++z)
        for (uint32 x = 0; x < rect.CountX; ++x)
            out.push_back(PageAddress{0, static_cast<uint8>(level), rect.FirstX + x, rect.FirstZ + z});
}

// Every height page next to or under one of `pages`, fetched.
FetchedHeightPages FetchHeightPages(const SurfaceHeightPageFetch& fetch, const PageStoreLevel& shape, uint32 level,
                                    std::span<const PageAddress> pages, JobSystem::WorkStealingThreadPool* pool)
{
    if (pages.empty())
        return FetchedHeightPages{level};
    FetchedHeightPages fetched = HeightFetchRect(shape, level, pages);
    fetched.Pages.resize(static_cast<std::size_t>(fetched.CountX) * fetched.CountZ);
    JobSystem::ParallelFor(pool, fetched.Pages.size(), [&](std::size_t i) {
        const PageAddress address{0, static_cast<uint8>(level), fetched.FirstX + static_cast<uint32>(i % fetched.CountX),
                                  fetched.FirstZ + static_cast<uint32>(i / fetched.CountX)};
        std::vector<float32> samples(PageStreaming::kPageSampleCount);
        if (fetch(address, samples))
            fetched.Pages[i] = std::move(samples);
    });
    return fetched;
}

// The first and last sample a page stores along one axis (its owned samples and its apron).
int64 StoredFirst(uint32 page)
{
    return static_cast<int64>(page) * kPageOwnedSamples - kPageApronSamples;
}

int64 StoredLast(uint32 page)
{
    return static_cast<int64>(page) * kPageOwnedSamples + kPageOwnedSamples - 1 + kPageApronSamples;
}

float32 PageSample(const float32* page, uint32 pageX, uint32 pageZ, int64 x, int64 z)
{
    const int64 column = x - StoredFirst(pageX);
    const int64 row = z - StoredFirst(pageZ);
    return page[static_cast<std::size_t>(row) * PageStreaming::kPageStrideSamples + static_cast<std::size_t>(column)];
}

// The heights over SurfaceHeightRect of `address`, each from the height page that owns the sample.
// A sample whose owner is not on hand takes the nearest sample of the page's own height page.
// False when the page's own height page is not on hand.
bool ComposeHeightWindow(const PageStoreLevel& shape, PageStreaming::PageFieldFormat format, const PageAddress& address,
                         const FetchedHeightPages& fetched, SurfaceHeightWindow& out)
{
    const float32* own = fetched.Find(address.X, address.Z);
    if (!own)
        return false;
    out.Rect = SurfaceHeightRect(shape, address.X, address.Z, format);
    out.Heights.resize(static_cast<std::size_t>(out.Rect.Width()) * out.Rect.Height());
    std::size_t at = 0;
    for (uint32 z = out.Rect.MinZ; z <= out.Rect.MaxZ; ++z)
        for (uint32 x = out.Rect.MinX; x <= out.Rect.MaxX; ++x)
        {
            const bool insideX = x >= StoredFirst(address.X) && x <= StoredLast(address.X);
            const bool insideZ = z >= StoredFirst(address.Z) && z <= StoredLast(address.Z);
            const uint32 ownerX = insideX ? address.X : x / kPageOwnedSamples;
            const uint32 ownerZ = insideZ ? address.Z : z / kPageOwnedSamples;
            const float32* owner = fetched.Find(ownerX, ownerZ);
            if (owner)
            {
                out.Heights[at++] = PageSample(owner, ownerX, ownerZ, x, z);
                continue;
            }
            const int64 nearestX = std::clamp<int64>(x, StoredFirst(address.X), StoredLast(address.X));
            const int64 nearestZ = std::clamp<int64>(z, StoredFirst(address.Z), StoredLast(address.Z));
            out.Heights[at++] = PageSample(own, address.X, address.Z, nearestX, nearestZ);
        }
    return true;
}

// The level-0 samples `address` stores, clipped to the level.
PageSampleRect StoredSampleRect(const PageAddress& address, const PageStoreLevel& shape)
{
    const auto clip = [](int64 sample, uint32 samples) {
        return static_cast<uint32>(std::clamp<int64>(sample, 0, static_cast<int64>(samples) - 1));
    };
    return PageSampleRect{clip(StoredFirst(address.X), shape.SamplesX), clip(StoredFirst(address.Z), shape.SamplesZ),
                          clip(StoredLast(address.X), shape.SamplesX), clip(StoredLast(address.Z), shape.SamplesZ)};
}

std::size_t DenseIndex(std::span<const PageStoreLevel> levels, const PageAddress& address)
{
    const PageStoreLevel& shape = levels[address.Level];
    return static_cast<std::size_t>(shape.FirstEntry + static_cast<uint64>(address.Z) * shape.PagesX + address.X);
}

std::vector<PageAddress> EveryPage(const PageStoreLevel& shape, uint32 level)
{
    std::vector<PageAddress> pages;
    pages.reserve(static_cast<std::size_t>(shape.PagesX) * shape.PagesZ);
    for (uint32 z = 0; z < shape.PagesZ; ++z)
        for (uint32 x = 0; x < shape.PagesX; ++x)
            pages.push_back(PageAddress{0, static_cast<uint8>(level), x, z});
    return pages;
}

// The writer of a Normal store's samples cache. The store itself is written through its plane's
// PageStoreAccess.
struct SurfaceStoreWriters
{
    PageStreaming::PageStoreWriter Samples;
    bool HasSamples = false;
};

void AbandonStores(PageStreaming::PageStoreAccess& store, SurfaceStoreWriters& writers)
{
    store.Close();
    writers.Samples.Abandon();
}

// Publishes the samples cache, then the store. When the store fails, its new cache is deleted.
std::string FinishStores(PageStreaming::PageStoreAccess& store, SurfaceStoreWriters& writers,
                         const std::filesystem::path& file)
{
    if (writers.HasSamples)
        if (std::string error = writers.Samples.Finish(); !error.empty())
        {
            store.Close();
            return error;
        }
    std::string error = store.Finish();
    if (!error.empty() && writers.HasSamples)
    {
        std::error_code ec;
        std::filesystem::remove(SurfaceSamplesCachePath(file), ec);
    }
    return error;
}

void MakePatcher(PageStreaming::PageStoreAccess& store, SurfaceStoreWriters& writers,
                 JobSystem::WorkStealingThreadPool* pool, std::optional<PageStreaming::PageStorePatcher>& out)
{
    if (writers.HasSamples)
        out.emplace(store.Writer(), writers.Samples, pool);
    else
        out.emplace(store.Writer(), pool);
}

} // namespace

struct TerrainSurfacePageBaker::BakedPage
{
    PageAddress Address;
    std::vector<uint8> Texels; // empty: not baked
};

struct TerrainSurfacePageBaker::OpenStore
{
    std::filesystem::path File;
    SurfaceStoreWriters Writers;
};

TerrainSurfacePageBaker::TerrainSurfacePageBaker(std::shared_ptr<const TerrainSurfaceBakeInputs> inputs,
                                                 PageStreaming::PageFieldKind plane,
                                                 std::shared_ptr<PageStreaming::PageStoreAccess> store,
                                                 SurfaceHeightPageFetch fetchHeights,
                                                 JobSystem::WorkStealingThreadPool* pool)
    : m_Inputs(std::move(inputs))
    , m_Plane(plane)
    , m_Store(std::move(store))
    , m_FetchHeights(std::move(fetchHeights))
    , m_Pool(pool)
{
    const PageStreaming::PageStoreHeader header = MakeSurfaceStoreHeader(m_Inputs->Config, plane, 0u);
    m_Levels = PageStreaming::BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
}

TerrainSurfacePageBaker::~TerrainSurfacePageBaker()
{
    if (std::string error = Finish(); !error.empty())
        Logger::Log::Warning("Surface pages: a store held open was not published: {}", error);
}

void TerrainSurfacePageBaker::SetInputs(std::shared_ptr<const TerrainSurfaceBakeInputs> inputs,
                                        SurfaceHeightPageFetch fetchHeights)
{
    m_Inputs = std::move(inputs);
    m_FetchHeights = std::move(fetchHeights);
}

uint32 TerrainSurfacePageBaker::FirstCoarseLevel(uint32 firstCoarseLevel) const
{
    const uint32 levels = static_cast<uint32>(m_Levels.size());
    return std::clamp(firstCoarseLevel, 1u, std::max(levels, 2u) - 1u);
}

std::vector<PageAddress> TerrainSurfacePageBaker::HeightPagesReadByCreate(uint32 firstCoarseLevel) const
{
    std::vector<PageAddress> read;
    if (m_Plane == PageStreaming::PageFieldKind::GrassControl)
        return read;
    for (uint32 level = FirstCoarseLevel(firstCoarseLevel); level < m_Levels.size(); ++level)
        for (const PageAddress& page : EveryPage(m_Levels[level], level))
            read.push_back(page);
    return read;
}

std::vector<PageAddress> TerrainSurfacePageBaker::HeightPagesReadByBake(std::span<const PageAddress> level0Pages) const
{
    std::vector<PageAddress> read;
    if (m_Plane == PageStreaming::PageFieldKind::GrassControl || level0Pages.empty())
        return read;
    AddHeightFetchRect(m_Levels.front(), 0u, level0Pages, read);
    std::vector<PageAddress> ancestors;
    for (uint32 level = 1; level < m_Levels.size(); ++level)
    {
        ancestors.clear();
        for (const PageAddress& page : level0Pages)
        {
            const PageAddress ancestor{0, static_cast<uint8>(level), page.X >> level, page.Z >> level};
            if (std::find(ancestors.begin(), ancestors.end(), ancestor) == ancestors.end())
                ancestors.push_back(ancestor);
        }
        AddHeightFetchRect(m_Levels[level], level, ancestors, read);
    }
    return read;
}

std::vector<PageAddress> TerrainSurfacePageBaker::Level0PagesReading(const PageStreaming::PageSampleRect& changed) const
{
    // A page stores its stride from StoredFirst minus the format's extra apron, and its bake reads
    // the heights one sample past that.
    const PageStreaming::PageStoreHeader header = MakeSurfaceStoreHeader(m_Inputs->Config, m_Plane, 0u);
    const int64 reach =
        (static_cast<int64>(PageStreaming::PageStrideTexels(header.Format)) - kPageOwnedSamples) / 2 + 1;
    const PageStoreLevel& shape = m_Levels.front();
    // Page p reads the samples from p * kPageOwnedSamples - reach to (p + 1) * kPageOwnedSamples - 1 + reach.
    const auto firstPage = [reach](uint32 sample) {
        return static_cast<uint32>(std::max<int64>(0, (static_cast<int64>(sample) - reach) / kPageOwnedSamples));
    };
    const auto lastPage = [reach](uint32 sample, uint32 pages) {
        const int64 page = (static_cast<int64>(sample) + reach) / kPageOwnedSamples;
        return static_cast<uint32>(std::min<int64>(pages - 1, page));
    };
    std::vector<PageAddress> pages;
    for (uint32 pz = firstPage(changed.MinZ); pz <= lastPage(changed.MaxZ, shape.PagesZ); ++pz)
        for (uint32 px = firstPage(changed.MinX); px <= lastPage(changed.MaxX, shape.PagesX); ++px)
            pages.push_back(PageAddress{0, 0, px, pz});
    return pages;
}

std::vector<TerrainSurfacePageBaker::BakedPage> TerrainSurfacePageBaker::BakePages(uint32 level, std::span<const PageAddress> pages)
{
    const PageStoreLevel& shape = m_Levels[level];
    std::vector<BakedPage> baked(pages.size());
    // Grass control reads no heights.
    if (m_Plane == PageStreaming::PageFieldKind::GrassControl)
    {
        JobSystem::ParallelFor(m_Pool, pages.size(), [&](std::size_t i) {
            baked[i].Address = pages[i];
            if (!TerrainModifierSystem::BakeSurfaceGrassPage(*m_Inputs, pages[i], shape, baked[i].Texels))
                baked[i].Texels.clear();
        });
        return baked;
    }
    const FetchedHeightPages fetched = FetchHeightPages(m_FetchHeights, shape, level, pages, m_Pool);
    const bool normal = m_Plane == PageStreaming::PageFieldKind::Normal;
    const PageStreaming::PageFieldFormat format =
        normal ? PageStreaming::PageFieldFormat::Bc5 : PageStreaming::PageFieldFormat::Rgba8;
    JobSystem::ParallelFor(m_Pool, pages.size(), [&](std::size_t i) {
        baked[i].Address = pages[i];
        SurfaceHeightWindow window;
        if (!ComposeHeightWindow(shape, format, pages[i], fetched, window) ||
            !BakePageTexels(pages[i], shape, window, baked[i].Texels))
            baked[i].Texels.clear();
    });
    return baked;
}

bool TerrainSurfacePageBaker::BakePageTexels(const PageAddress& address, const PageStoreLevel& shape,
                                             const SurfaceHeightWindow& window, std::vector<uint8>& outTexels) const
{
    if (m_Plane == PageStreaming::PageFieldKind::Normal)
        return BakeSurfaceNormalPage(m_Inputs->Config, address, shape, window, outTexels);
    if (!TerrainModifierSystem::BakeSurfaceSplatPage(*m_Inputs, address, shape, window, outTexels))
        return false;
    if (m_Plane != PageStreaming::PageFieldKind::Color)
        return true;
    // A color page composes over the splat page of its address.
    const std::vector<uint8> splat = std::move(outTexels);
    return TerrainModifierSystem::BakeSurfaceColorPage(*m_Inputs, address, shape, splat, outTexels);
}

std::string TerrainSurfacePageBaker::CreateStore(const std::filesystem::path& file, uint64 key, uint32 firstCoarseLevel)
{
    if (m_Open)
        return "the baker holds the store at " + m_Open->File.string() + " open: finish it first";
    const PageStreaming::PageStoreHeader header = MakeSurfaceStoreHeader(m_Inputs->Config, m_Plane, key);
    const uint32 levels = static_cast<uint32>(m_Levels.size());
    const uint32 first = FirstCoarseLevel(firstCoarseLevel);
    std::vector<uint8> present(static_cast<std::size_t>(PageStreaming::DensePageCount(m_Levels)), 0u);
    for (uint32 level = first; level < levels; ++level)
        for (const PageAddress& page : EveryPage(m_Levels[level], level))
            present[DenseIndex(m_Levels, page)] = 1u;

    SurfaceStoreWriters writers;
    if (std::string error = m_Store->Create(file, PageStreaming::MakePageStoreLayout(header, present)); !error.empty())
        return error;
    writers.HasSamples = m_Plane == PageStreaming::PageFieldKind::Normal;
    if (writers.HasSamples)
    {
        PageStreaming::PageStoreHeader samplesHeader = header;
        samplesHeader.Format = PageStreaming::PageFieldFormat::Rg8;
        if (std::string error = writers.Samples.Create(SurfaceSamplesCachePath(file),
                                                       PageStreaming::MakePageStoreLayout(samplesHeader, present));
            !error.empty())
        {
            m_Store->Close();
            return error;
        }
    }
    std::optional<PageStreaming::PageStorePatcher> patcher;
    MakePatcher(*m_Store, writers, m_Pool, patcher);
    // Coarsest first: the far field is whole before any finer level is.
    for (uint32 level = levels; level-- > first;)
    {
        const std::vector<PageAddress> pages = EveryPage(m_Levels[level], level);
        for (const BakedPage& page : BakePages(level, pages))
        {
            if (page.Texels.empty())
            {
                AbandonStores(*m_Store, writers);
                return "the height page of level " + std::to_string(level) + " page (" +
                       std::to_string(page.Address.X) + ", " + std::to_string(page.Address.Z) +
                       ") is not on hand: a coarse level is baked from heights the CPU holds";
            }
            if (!patcher->WritePage(page.Address, page.Texels))
            {
                AbandonStores(*m_Store, writers);
                return "the surface store could not be written at " + file.string();
            }
        }
    }
    m_PagesWritten += patcher->PagesWritten();
    patcher.reset();
    return FinishStores(*m_Store, writers, file);
}

std::string TerrainSurfacePageBaker::OpenForBake(const std::filesystem::path& file)
{
    if (m_Open)
        return m_Open->File == file ? std::string{}
                                    : "the baker holds the store at " + m_Open->File.string() + " open: finish it first";
    const bool hasSamples = m_Plane == PageStreaming::PageFieldKind::Normal;
    const std::filesystem::path samplesFile = SurfaceSamplesCachePath(file);
    PageStoreLayout layout;
    PageStoreLayout samplesLayout;
    {
        PageStreaming::PageStoreReader reader;
        if (std::string error = reader.Open(file, m_Plane); !error.empty())
            return error;
        layout = reader.Layout();
        if (hasSamples)
        {
            if (std::string error = reader.Open(samplesFile, PageStreaming::PageFieldKind::NormalSamples);
                !error.empty())
                return "the samples cache at " + samplesFile.string() + " is not readable: bake the store again (" +
                       error + ")";
            samplesLayout = reader.Layout();
        }
    }
    if (layout.Levels.size() != m_Levels.size() || layout.Header.SamplesX != m_Levels.front().SamplesX ||
        layout.Header.SamplesZ != m_Levels.front().SamplesZ)
        return "the surface store at " + file.string() + " is for another terrain lattice: bake it again";
    if (hasSamples &&
        (samplesLayout.Entries.size() != layout.Entries.size() || samplesLayout.Header.Key != layout.Header.Key))
        return "the samples cache at " + samplesFile.string() + " does not match its store: bake the store again";

    auto open = std::make_unique<OpenStore>();
    open->File = file;
    open->Writers.HasSamples = hasSamples;
    if (std::string error = m_Store->OpenForPatch(file, std::move(layout)); !error.empty())
        return error;
    if (hasSamples)
        if (std::string error = open->Writers.Samples.OpenForPatch(samplesFile, samplesFile, std::move(samplesLayout));
            !error.empty())
        {
            m_Store->Close();
            return error;
        }
    m_Open = std::move(open);
    return {};
}

std::string TerrainSurfacePageBaker::BakeLevel0Pages(const std::filesystem::path& file, std::span<const PageAddress> pages)
{
    if (std::string error = OpenForBake(file); !error.empty())
        return error;
    SurfaceStoreWriters& writers = m_Open->Writers;
    const PageStoreLayout layout = m_Store->Writer().Layout();

    std::vector<BakedPage> level0 = BakePages(0u, pages);
    // The ancestors the store does not hold, finest first, each baked at its own spacing.
    std::vector<std::vector<BakedPage>> ancestors(layout.Levels.size());
    std::vector<uint8> present(static_cast<std::size_t>(PageStreaming::DensePageCount(layout.Levels)), 0u);
    std::vector<PageAddress> missing;
    for (const BakedPage& page : level0)
        if (!page.Texels.empty())
            present[DenseIndex(m_Levels, page.Address)] = 1u;
    for (uint32 level = 1; level < layout.Levels.size(); ++level)
    {
        missing.clear();
        for (const BakedPage& page : level0)
        {
            const PageAddress ancestor{0, static_cast<uint8>(level), page.Address.X >> level, page.Address.Z >> level};
            if (!page.Texels.empty() && !layout.FindPresent(ancestor) &&
                std::find(missing.begin(), missing.end(), ancestor) == missing.end())
                missing.push_back(ancestor);
        }
        ancestors[level] = BakePages(level, missing);
        for (const BakedPage& page : ancestors[level])
            if (!page.Texels.empty())
                present[DenseIndex(m_Levels, page.Address)] = 1u;
    }

    std::string grown = m_Store->AddPresentPages(present);
    if (grown.empty() && writers.HasSamples)
        grown = writers.Samples.AddPresentPages(present);
    if (!grown.empty())
    {
        AbandonStores(*m_Store, writers);
        m_Open.reset();
        return grown;
    }
    std::optional<PageStreaming::PageStorePatcher> patcher;
    MakePatcher(*m_Store, writers, m_Pool, patcher);
    std::optional<PageSampleRect> written;
    std::optional<PageAddress> notBaked;
    for (uint32 level = static_cast<uint32>(layout.Levels.size()); level-- > 0u;)
        for (const BakedPage& page : level == 0u ? level0 : ancestors[level])
        {
            if (page.Texels.empty())
            {
                if (!notBaked)
                    notBaked = page.Address;
                continue;
            }
            if (!patcher->WritePage(page.Address, page.Texels))
            {
                patcher.reset();
                AbandonStores(*m_Store, writers);
                m_Open.reset();
                return "the surface store could not be written at " + file.string();
            }
            if (level == 0u)
            {
                const PageSampleRect stored = StoredSampleRect(page.Address, layout.Levels.front());
                written = written ? Union(*written, stored) : stored;
            }
        }
    if (written && !patcher->RefilterAncestors(*written))
    {
        patcher.reset();
        AbandonStores(*m_Store, writers);
        m_Open.reset();
        return "the surface store's coarser pages could not be written at " + file.string();
    }
    m_PagesWritten += patcher->PagesWritten();
    m_Store->Commit();
    if (notBaked)
        return "the height page of level " + std::to_string(notBaked->Level) + " page (" +
               std::to_string(notBaked->X) + ", " + std::to_string(notBaked->Z) +
               ") is not on hand: the page is not baked";
    return {};
}

std::string TerrainSurfacePageBaker::Finish()
{
    if (!m_Open)
        return {};
    const std::unique_ptr<OpenStore> open = std::move(m_Open);
    return FinishStores(*m_Store, open->Writers, open->File);
}

} // namespace GameEngine::TerrainECS
