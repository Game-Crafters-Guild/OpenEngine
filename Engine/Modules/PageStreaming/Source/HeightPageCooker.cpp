#include "PageStreaming/HeightPageCooker.h"

#include "HeightCookBands.h"
#include "PagePyramidBuilder.h"
#include "HeightSourceRows.h"
#include "HeightStepPlanner.h"
#include "SourceRangeReader.h"
#include "HeightPageCodec.h"
#include "PageStreaming/PageStore/PageStoreReader.h"
#include "PageStreaming/PageStore/PageStoreWriter.h"

#include "AssetCore/SharedFileRead.h"
#include "Terrain/Heightfield.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>
#include <vector>


namespace GameEngine::PageStreaming
{
namespace
{

// Bytes read per step of the whole-file scan.
constexpr std::size_t kScanChunkBytes = 64u * 1024u * 1024u;

std::string LowerExtension(const std::filesystem::path& file)
{
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string ResolveRawSource(const std::filesystem::path& file, HeightSourceFormat format, uint32 samplesX,
                             uint32 samplesZ, HeightCookSource& out)
{
    std::error_code ec;
    const uint64 fileBytes = std::filesystem::file_size(file, ec);
    if (ec)
        return "the heightmap " + file.string() + " could not be read";
    const uint64 sampleBytes = format == HeightSourceFormat::R16 ? 2u : 4u;
    if (samplesX < 2 || samplesZ < 2)
        return "the heightmap's grid is not set: set its Samples X and Samples Z";
    if (samplesX > kPageStoreMaxSamplesPerAxis || samplesZ > kPageStoreMaxSamplesPerAxis)
        return "the heightmap is " + std::to_string(samplesX) + " x " + std::to_string(samplesZ) +
               " samples, more than " + std::to_string(kPageStoreMaxSamplesPerAxis) +
               " along an axis: split it into terrains of at most that many samples a side";
    if (static_cast<uint64>(samplesX) * samplesZ * sampleBytes != fileBytes)
        return "the heightmap's Samples X and Samples Z (" + std::to_string(samplesX) + " x " +
               std::to_string(samplesZ) + ") do not account for its " + std::to_string(fileBytes) +
               " bytes: set the grid it was exported with";
    out = HeightCookSource{file, format, samplesX, samplesZ};
    return {};
}

std::string ResolvePngSource(const std::filesystem::path& file, HeightCookSource& out)
{
    Terrain::PngHeightmapSize size;
    if (std::string reason = Terrain::ResolvePngHeightmapSize(file, size); !reason.empty())
        return reason;
    if (size.Width < 2 || size.Height < 2)
        return "the PNG " + file.string() + " is smaller than 2 x 2 samples";
    if (size.Width > kPageStoreMaxSamplesPerAxis || size.Height > kPageStoreMaxSamplesPerAxis)
        return "the PNG " + file.string() + " is wider than " + std::to_string(kPageStoreMaxSamplesPerAxis) +
               " samples along an axis";
    out = HeightCookSource{file, HeightSourceFormat::Png16, size.Width, size.Height};
    return {};
}

// A scan chunk's float samples into the step planner; false at a sample that is not a finite number.
bool PlanSteps(const uint8* bytes, std::size_t count, HeightStepPlanner& planner)
{
    const auto* samples = reinterpret_cast<const float32*>(bytes);
    for (std::size_t i = 0; i < count; ++i)
        if (!std::isfinite(samples[i]))
            return false;
    planner.Push({samples, count});
    return true;
}

PageStoreHeader MakeHeader(const HeightCookRequest& request)
{
    const bool absolute = request.Source.Format == HeightSourceFormat::R32;
    PageStoreHeader header;
    header.Format = PageFieldFormat::HeightSteps;
    header.Filter = PageFieldFilter::HeightTent;
    header.Units = absolute ? PageHeightUnits::Absolute : PageHeightUnits::Normalized;
    header.FaceCount = 1;
    header.FaceMap = PageFaceMap::Flat;
    header.SamplesX = request.Source.SamplesX;
    header.SamplesZ = request.Source.SamplesZ;
    header.ReferenceRadius = 0.0;
    const std::size_t levels = BuildPageStoreLevels(header.SamplesX, header.SamplesZ).size();
    for (std::size_t level = 0; level < levels; ++level)
        header.StepPerLevel[level] =
            absolute ? std::ldexp(1.0f, request.Identity.StepExponents[level]) : kNormalizedHeightStep;
    header.Key = request.Key;
    return header;
}

// A previous store whose pages a cook can patch: same grid, steps and units.
bool CanPatch(const PageStoreHeader& previous, const PageStoreHeader& next)
{
    return previous.Format == next.Format && previous.Filter == next.Filter && previous.Units == next.Units &&
           previous.FaceCount == next.FaceCount && previous.FaceMap == next.FaceMap &&
           previous.SamplesX == next.SamplesX && previous.SamplesZ == next.SamplesZ &&
           previous.ReferenceRadius == next.ReferenceRadius && previous.StepPerLevel == next.StepPerLevel;
}

std::string StartStore(const HeightCookRequest& request, const PageStoreHeader& header, PageStoreWriter& writer,
                       bool& outPatching)
{
    outPatching = false;
    std::error_code ec;
    if (!request.Previous.empty() && request.Previous != request.Output && std::filesystem::exists(request.Previous, ec))
    {
        PageStoreLayout previousLayout;
        {
            PageStoreReader previous;
            if (previous.Open(request.Previous, PageFieldKind::Height).empty())
                previousLayout = previous.Layout();
        }
        if (!previousLayout.Levels.empty() && CanPatch(previousLayout.Header, header))
        {
            previousLayout.Header.Key = header.Key;
            outPatching = true;
            return writer.OpenForPatch(request.Previous, request.Output, std::move(previousLayout));
        }
    }
    return writer.Create(request.Output, MakePageStoreLayout(header));
}

} // namespace

std::string ResolveHeightCookSource(const std::filesystem::path& file, uint32 samplesX, uint32 samplesZ,
                                    HeightCookSource& out)
{
    const std::string ext = LowerExtension(file);
    if (ext == ".r32")
        return ResolveRawSource(file, HeightSourceFormat::R32, samplesX, samplesZ, out);
    if (ext == ".r16")
        return ResolveRawSource(file, HeightSourceFormat::R16, samplesX, samplesZ, out);
    if (ext == ".png")
        return ResolvePngSource(file, out);
    if (ext == ".tif" || ext == ".tiff")
        return "GeoTIFF heightmaps are not read: export the DEM as .r32 (32-bit float heights in meters, rows "
               "from the smallest Z, nodata filled) and set its Samples X and Samples Z";
    return "'" + ext + "' is not a heightmap format: use .r32, .r16 or a 16-bit PNG";
}

std::string ScanHeightSource(const HeightCookSource& source, AssetIOService* io, HeightSourceIdentity& out,
                             const std::atomic<bool>* cancel, HeightCookProgress* progress)
{
    std::error_code ec;
    const uint64 size = std::filesystem::file_size(source.File, ec);
    if (ec)
        return "the heightmap " + source.File.string() + " could not be read";

    HeightSourceIdentity identity;
    identity.ContentHash = Hashing::kFnv1a64OffsetBasis;
    identity.SamplesX = source.SamplesX;
    identity.SamplesZ = source.SamplesZ;
    const bool ranged = source.Format == HeightSourceFormat::R32;
    HeightStepPlanner planner(ranged ? source.SamplesX : 0u, ranged ? source.SamplesZ : 0u, 1u);

    // One chunk is read ahead while the one before it is hashed.
    SourceRangeReader reader(source.File, io);
    std::vector<uint8> chunk;
    uint64 offset = 0;
    if (size > 0)
        reader.Request(0, std::min<uint64>(kScanChunkBytes, size));
    while (offset < size)
    {
        if (!reader.Take(chunk))
            return "the heightmap " + source.File.string() + " could not be read to its end";
        offset += chunk.size();
        if (offset < size)
            reader.Request(offset, std::min<uint64>(kScanChunkBytes, size - offset));
        if (Cancelled(cancel))
        {
            if (offset < size)
                reader.Take(chunk); // never leave a read outstanding past the reader
            return "the cook was cancelled";
        }
        identity.ContentHash = Hashing::Fnv1a64(chunk.data(), chunk.size(), identity.ContentHash);
        if (progress)
            progress->RowsDone.store(offset * source.SamplesZ / size);
        if (ranged && !PlanSteps(chunk.data(), chunk.size() / 4u, planner))
        {
            if (offset < size)
                reader.Take(chunk);
            return "the heightmap has samples that are not numbers (nodata): fill them before export";
        }
    }
    if (ranged)
        identity.StepExponents = planner.StepExponents();
    out = identity;
    return {};
}

HeightCookResult CookHeightPageStore(const HeightCookRequest& request)
{
    HeightCookResult result;
    HeightSourceRows rows;
    if (result.Error = rows.Open(request.Source, request.Io); !result.Error.empty())
        return result;

    const PageStoreHeader header = MakeHeader(request);
    PageStoreWriter writer;
    if (result.Error = StartStore(request, header, writer, result.Patched); !result.Error.empty())
        return result;

    // Each band is read while the band before it builds the pyramid.
    PagePyramidBuilder builder(writer, result.Patched, request.Pool, nullptr);
    const uint32 height = request.Source.SamplesZ;
    const CookBandResult bands = PushCookBands(
        rows, 0, height, request.Source.SamplesX, request.Cancel, builder,
        [&request, height](const float32*, uint32 row, uint32 count) {
            if (request.Progress)
                request.Progress->RowsDone.store(static_cast<uint64>(height) + row + count);
        });
    if (bands == CookBandResult::Cancelled)
    {
        writer.Abandon();
        result.Error = "the cook was cancelled";
        return result;
    }
    const bool readOk = bands == CookBandResult::Done;
    if (!readOk || !builder.Finish())
    {
        writer.Abandon();
        result.Error = readOk ? "the page store could not be written (is the disk full?)"
                              : "the heightmap " + request.Source.File.string() + " could not be read to its end";
        return result;
    }
    result.StoreBytes = writer.Layout().TotalBytes();
    result.PagesWritten = builder.PagesWritten();
    result.PagesUnchanged = builder.PagesUnchanged();
    result.Error = writer.Finish();
    return result;
}

} // namespace GameEngine::PageStreaming
