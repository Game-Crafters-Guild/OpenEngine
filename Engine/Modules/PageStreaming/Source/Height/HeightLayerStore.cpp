#include "PageStreaming/Height/HeightLayerStore.h"

#include "HeightLayerAuthoredPages.h"
#include "HeightPageCodec.h"
#include "PageStore/PageStoreSerialization.h"

#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageStore/PageStoreReader.h"
#include "PageStreaming/PageStore/PageStoreWriter.h"

#include <algorithm>
#include <cassert>
#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

std::string SaveHeightLayer(const HeightPageLayer& layer, const std::filesystem::path& target)
{
    const std::vector<HeightPageLayerPage> pages = HeightLayerAuthoredPages::Export(layer);
    if (pages.empty())
        return {};
    const std::span<const PageStoreLevel> levels = layer.Levels();
    PageStoreHeader header;
    header.Format = PageFieldFormat::HeightSteps;
    header.Filter = PageFieldFilter::HeightTent;
    header.Units = PageHeightUnits::LayerMeters;
    header.FaceCount = static_cast<uint8>(layer.FaceCount());
    header.FaceMap = layer.FaceCount() == kPageStoreMaxFaces ? PageFaceMap::EqualAngleCube : PageFaceMap::Flat;
    header.SamplesX = levels.front().SamplesX;
    header.SamplesZ = levels.front().SamplesZ;
    std::fill_n(header.StepPerLevel.begin(), levels.size(), kHeightLayerStep);
    header.Key = kHeightLayerFormatVersion;

    std::vector<PageAddress> present;
    present.reserve(pages.size());
    for (const HeightPageLayerPage& page : pages)
        present.push_back(page.Address);
    std::optional<PageStoreLayout> layout = MakeSparsePageStoreLayout(header, present);
    if (!layout)
        return "the height layer's pages do not fit a store of its pyramid";

    PageStoreWriter writer;
    if (std::string reason = writer.Create(target, std::move(*layout)); !reason.empty())
        return reason;
    EncodedPage encoded;
    for (const HeightPageLayerPage& page : pages)
    {
        assert(std::ranges::all_of(page.Deltas, [](float32 v) { return QuantizeHeight(v, kHeightLayerStep) == v; }) &&
               "a saved layer's samples are on kHeightLayerStep's lattice");
        const auto [lowest, highest] = std::ranges::minmax(page.Deltas);
        EncodeHeightPage(writer.Layout().Header, page.Address.Level, page.Deltas, lowest, highest, encoded);
        if (!writer.WritePage(page.Address, encoded))
        {
            writer.Abandon();
            return "writing a page of the height layer to " + target.string() + " failed";
        }
    }
    return writer.Finish();
}

std::string LoadHeightLayer(const std::filesystem::path& path, std::shared_ptr<HeightPageLayer>& out)
{
    PageStoreReader reader;
    if (std::string reason = reader.Open(path, PageFieldKind::Height); !reason.empty())
        return reason;
    const PageStoreLayout& layout = reader.Layout();
    const PageStoreHeader& header = layout.Header;
    if (header.Units != PageHeightUnits::LayerMeters)
        return path.string() +
               " is a height store but not a height layer: the Terrain's Height Layer names a file that is not a "
               "planet's sculpt";
    if (header.Key != kHeightLayerFormatVersion)
        return path.string() + " is height layer format " + std::to_string(header.Key) + " and this engine reads format " +
               std::to_string(kHeightLayerFormatVersion);
    const bool layerSteps = std::all_of(header.StepPerLevel.begin(),
                                        header.StepPerLevel.begin() + static_cast<std::ptrdiff_t>(layout.Levels.size()),
                                        [](float32 step) { return step == kHeightLayerStep; });
    if ((header.FaceCount != 1u && header.FaceCount != kPageStoreMaxFaces) || header.ReferenceRadius != 0.0 ||
        !layerSteps)
        return path.string() + " is a height layer of a shape its format does not allow: the file is damaged";

    std::vector<HeightPageLayerPage> pages;
    pages.reserve(layout.Entries.size());
    for (uint32 face = 0; face < header.FaceCount; ++face)
        for (std::size_t level = 0; level < layout.Levels.size(); ++level)
        {
            const PageStoreRange& range = layout.Ranges[face * layout.Levels.size() + level];
            for (uint64 entry = range.First; entry < range.First + range.Count; ++entry)
            {
                HeightPageLayerPage page{
                    PageOfMortonCode(static_cast<uint8>(face), static_cast<uint8>(level), layout.Entries[entry].Morton),
                    level > 0, std::vector<float32>(kPageSampleCount)};
                const std::span<uint8> bytes(reinterpret_cast<uint8*>(page.Deltas.data()),
                                             page.Deltas.size() * sizeof(float32));
                if (!reader.ReadPageSamples(page.Address, bytes))
                    return "reading a page of the height layer " + path.string() + " failed";
                pages.push_back(std::move(page));
            }
        }
    auto layer = std::make_shared<HeightPageLayer>(header.FaceCount, layout.Levels);
    if (!HeightLayerAuthoredPages::Import(*layer, pages))
        return path.string() + " holds a page outside its height layer";
    out = std::move(layer);
    return {};
}

} // namespace GameEngine::PageStreaming
