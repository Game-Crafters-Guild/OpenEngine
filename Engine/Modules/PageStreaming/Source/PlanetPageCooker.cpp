#include "PageStreaming/HeightPageCooker.h"

#include "HeightCookBands.h"
#include "HeightPageCodec.h"
#include "HeightSourceRows.h"
#include "HeightStepPlanner.h"
#include "NonInheritedFile.h"
#include "PageFaceSeams.h"
#include "PagePyramidBuilder.h"
#include "PageStreaming/PageStoreWriter.h"

#include "FileSystem/FileSystem.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/CubeFaces.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace GameEngine::PageStreaming
{
namespace
{

// A planet face's samples a side is valid when every level but the last halves evenly, so each
// level has a sample on both of the face's edges.
bool PlanetFaceSamplesHalve(uint32 samples)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(samples, samples);
    for (std::size_t level = 0; level + 1u < levels.size(); ++level)
        if ((levels[level].SamplesX - 1u) % 2u != 0)
            return false;
    return samples >= 5u;
}

// The level-0 lattice of a planet: the gnomonic coordinate of every sample index, and the height
// at each sample's direction.
class PlanetLattice
{
public:
    explicit PlanetLattice(const PlanetCookRequest& request)
        : m_Request(request)
    {
        m_Gnomonic.resize(request.FaceSamples);
        for (uint32 i = 0; i < request.FaceSamples; ++i)
            m_Gnomonic[i] = GnomonicFaceCoordinate(EqualAngleSampleCoordinate(i, request.FaceSamples));
    }

    // Rows [firstRow, firstRow + count) of `face`, row-major; false at a height that is not a number.
    bool Rows(uint8 face, uint32 firstRow, uint32 count, std::vector<float32>& out) const
    {
        const uint32 samples = m_Request.FaceSamples;
        out.resize(static_cast<std::size_t>(samples) * count);
        std::atomic<bool> finite{true};
        JobSystem::ParallelFor(m_Request.Pool, count, [&](size_t bandRow) {
            float32* row = out.data() + bandRow * samples;
            for (uint32 x = 0; x < samples; ++x)
            {
                row[x] = Height(face, x, firstRow + static_cast<uint32>(bandRow));
                if (!std::isfinite(row[x]))
                    finite.store(false);
            }
        });
        return finite.load();
    }

private:
    // The height at sample (x, z) of `face`. The direction is built per world axis from the face's
    // axes, so a sample on an edge has the same direction, bit for bit, on both faces.
    float32 Height(uint8 face, uint32 x, uint32 z) const
    {
        const Mathematics::CubeFaceAxes& axes = Mathematics::kCubeFaceAxes[face];
        const float64 cubeX = CubeComponent(axes, 0, m_Gnomonic[x], m_Gnomonic[z]);
        const float64 cubeY = CubeComponent(axes, 1, m_Gnomonic[x], m_Gnomonic[z]);
        const float64 cubeZ = CubeComponent(axes, 2, m_Gnomonic[x], m_Gnomonic[z]);
        const float64 inverseLength = 1.0 / std::sqrt(cubeX * cubeX + cubeY * cubeY + cubeZ * cubeZ);
        return m_Request.Height(face, x, z,
                                Mathematics::Vector3(static_cast<float32>(cubeX * inverseLength),
                                                     static_cast<float32>(cubeY * inverseLength),
                                                     static_cast<float32>(cubeZ * inverseLength)));
    }

    // The component along world axis `worldAxis` of the point (x, z) on the cube face `axes`.
    static float64 CubeComponent(const Mathematics::CubeFaceAxes& axes, uint8 worldAxis, float64 x, float64 z)
    {
        if (worldAxis == axes.NormalAxis)
            return axes.NormalSign;
        return worldAxis == axes.XAxis ? axes.XSign * x : axes.ZSign * z;
    }

    const PlanetCookRequest& m_Request;
    std::vector<float64> m_Gnomonic;
};

constexpr const char* kPlanetHeightNotNumber = "the planet's height is not a number at some direction: fix its height source";

constexpr const char* kPlanetLevel0NotReadBack =
    "the planet's level 0 could not be written beside the store and read back: check that the output folder exists and has free disk space";

using PlanetFaceSides = std::array<std::vector<float32>, kPageFaceSides>;

// A planet's level 0 beside the store, an .r32 of FaceSamples x 6 FaceSamples, so each height is
// evaluated once. The step pass writes it, the two passes after it read it. Removed at the cook's end.
class PlanetLevel0File
{
public:
    explicit PlanetLevel0File(const std::filesystem::path& output)
        : m_Path(FileSystem::MakeTemporarySiblingPath(output))
    {
    }

    ~PlanetLevel0File()
    {
        Close();
        std::error_code error;
        std::filesystem::remove(m_Path, error);
    }

    PlanetLevel0File(const PlanetLevel0File&) = delete;
    PlanetLevel0File& operator=(const PlanetLevel0File&) = delete;

    bool Create()
    {
        m_File = OpenNonInheritedFile(m_Path, NonInheritedFileMode::CreateWrite);
        return m_File != nullptr;
    }

    bool Append(std::span<const float32> samples)
    {
        return std::fwrite(samples.data(), sizeof(float32), samples.size(), m_File) == samples.size();
    }

    bool Close()
    {
        if (!m_File)
            return true;
        const bool closed = std::fclose(m_File) == 0;
        m_File = nullptr;
        return closed;
    }

    HeightCookSource Source(uint32 faceSamples) const
    {
        return HeightCookSource{m_Path, HeightSourceFormat::R32, faceSamples, faceSamples * kPageStoreMaxFaces};
    }

private:
    std::filesystem::path m_Path;
    std::FILE* m_File = nullptr;
};

// Evaluates every face's level 0, once: into the step plan and onto `level0`. Empty, else the reason.
std::string EvaluatePlanetLevel0(const PlanetLattice& lattice, const PlanetCookRequest& request,
                                 HeightStepPlanner& planner, PlanetLevel0File& level0)
{
    if (!level0.Create())
        return kPlanetLevel0NotReadBack;
    std::vector<float32> band;
    for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
        for (uint32 row = 0; row < request.FaceSamples; row += kCookBandRows)
        {
            if (Cancelled(request.Cancel))
                return "the cook was cancelled";
            const uint32 count = std::min(kCookBandRows, request.FaceSamples - row);
            if (!lattice.Rows(face, row, count, band))
                return kPlanetHeightNotNumber;
            planner.Push(band);
            if (!level0.Append(band))
                return kPlanetLevel0NotReadBack;
        }
    return level0.Close() ? std::string{} : kPlanetLevel0NotReadBack;
}

// Copies the samples of rows [firstRow, firstRow + rowCount) of a face that lie on its four sides.
void RecordPlanetSides(const float32* band, uint32 firstRow, uint32 rowCount, uint32 samples, PlanetFaceSides& sides)
{
    const uint32 last = samples - 1u;
    for (uint32 bandRow = 0; bandRow < rowCount; ++bandRow)
    {
        const float32* row = band + static_cast<std::size_t>(bandRow) * samples;
        const uint32 z = firstRow + bandRow;
        if (z == 0)
            std::copy(row, row + samples, sides[static_cast<std::size_t>(PageFaceSide::MinZ)].begin());
        if (z == last)
            std::copy(row, row + samples, sides[static_cast<std::size_t>(PageFaceSide::MaxZ)].begin());
        sides[static_cast<std::size_t>(PageFaceSide::MinX)][z] = row[0];
        sides[static_cast<std::size_t>(PageFaceSide::MaxX)][z] = row[last];
    }
}

// Pushes `face`'s level-0 rows from `rows` through `builder`, a band read ahead, and records the
// face's sides in `sides` when it is set. Empty, else the reason.
std::string PushPlanetFace(HeightSourceRows& rows, const PlanetCookRequest& request, uint8 face,
                           PagePyramidBuilder& builder, PlanetFaceSides* sides)
{
    const uint32 samples = request.FaceSamples;
    const CookBandResult bands = PushCookBands(rows, static_cast<uint32>(face) * samples, samples, samples, request.Cancel, builder,
                                               [samples, sides](const float32* band, uint32 row, uint32 count) {
                                                   if (sides)
                                                       RecordPlanetSides(band, row, count, samples, *sides);
                                               });
    if (bands == CookBandResult::ReadFailed)
        return kPlanetLevel0NotReadBack;
    if (bands == CookBandResult::Cancelled)
        return "the cook was cancelled";
    return {};
}

} // namespace

HeightCookResult CookPlanetPageStore(const PlanetCookRequest& request)
{
    HeightCookResult result;
    // Level 0 is read back as one source of six faces' rows, counted in 32 bits.
    constexpr uint32 kPlanetMaxFaceSamples = std::numeric_limits<uint32>::max() / kPageStoreMaxFaces;
    if (!request.Height)
    {
        result.Error = "the planet cook has no height function: set PlanetCookRequest::Height";
        return result;
    }
    if (!PlanetFaceSamplesHalve(request.FaceSamples) ||
        request.FaceSamples > std::min(kPageStoreMaxSamplesPerAxis, kPlanetMaxFaceSamples))
    {
        result.Error = "a planet face of " + std::to_string(request.FaceSamples) +
                       " samples a side does not halve to one page with a sample on its edges at every level: "
                       "use m x 2^k + 1 samples with m at most 128";
        return result;
    }
    if (!std::isfinite(request.ReferenceRadius) || !(request.ReferenceRadius > 0.0))
    {
        result.Error = "the planet's reference radius must be a positive number of meters";
        return result;
    }
    const PlanetLattice lattice(request);

    // The steps, from every face's level 0, evaluated here once and read back by the passes below.
    HeightStepPlanner planner(request.FaceSamples, request.FaceSamples, kPageStoreMaxFaces);
    PlanetLevel0File level0(request.Output);
    if (result.Error = EvaluatePlanetLevel0(lattice, request, planner, level0); !result.Error.empty())
        return result;
    const std::array<int8, kPageStoreMaxLevels> exponents = planner.StepExponents();
    HeightSourceRows rows;
    if (!rows.Open(level0.Source(request.FaceSamples), nullptr).empty())
    {
        result.Error = kPlanetLevel0NotReadBack;
        return result;
    }

    PageStoreHeader header;
    header.Format = PageFieldFormat::HeightSteps;
    header.Filter = PageFieldFilter::HeightTent;
    header.Units = PageHeightUnits::Absolute;
    header.FaceCount = static_cast<uint8>(kPageStoreMaxFaces);
    header.FaceMap = PageFaceMap::EqualAngleCube;
    header.SamplesX = request.FaceSamples;
    header.SamplesZ = request.FaceSamples;
    header.ReferenceRadius = request.ReferenceRadius;
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
    for (std::size_t level = 0; level < levels.size(); ++level)
        header.StepPerLevel[level] = std::ldexp(1.0f, exponents[level]);
    header.Key = request.Key;

    PageStoreWriter writer;
    if (result.Error = writer.Create(request.Output, MakePageStoreLayout(header)); !result.Error.empty())
        return result;

    // Each face's lines one in from its sides, then the seams of every level from them.
    PageFaceSeams seams(levels, header.StepPerLevel);
    for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
    {
        PageFaceLines inner;
        const PageFaceBuild capture{face, nullptr, &inner};
        PagePyramidBuilder builder(writer, false, request.Pool, &capture);
        PlanetFaceSides sides;
        for (std::vector<float32>& side : sides)
            side.resize(request.FaceSamples);
        if (result.Error = PushPlanetFace(rows, request, face, builder, &sides); !result.Error.empty())
        {
            writer.Abandon();
            return result;
        }
        for (std::vector<float32>& side : sides)
            for (float32& value : side)
                value = QuantizeHeight(value, header.StepPerLevel[0]);
        seams.SetFace(face, std::move(inner), std::move(sides));
    }
    seams.Build();

    // The pages, each face reading its neighbors' lines past its sides.
    for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
    {
        const PageFaceLines ghosts = seams.Ghosts(face);
        const PageFaceBuild build{face, &ghosts, nullptr};
        PagePyramidBuilder builder(writer, false, request.Pool, &build);
        if (result.Error = PushPlanetFace(rows, request, face, builder, nullptr); !result.Error.empty())
        {
            writer.Abandon();
            return result;
        }
        if (!builder.Finish())
        {
            writer.Abandon();
            result.Error = "the page store could not be written (is the disk full?)";
            return result;
        }
        result.PagesWritten += builder.PagesWritten();
    }
    result.StoreBytes = writer.Layout().TotalBytes();
    result.Error = writer.Finish();
    return result;
}

} // namespace GameEngine::PageStreaming
