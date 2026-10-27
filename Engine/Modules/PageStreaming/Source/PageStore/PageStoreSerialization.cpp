#include "PageStoreSerialization.h"

#include "AssetCore/SharedFileRead.h"

#include <bit>
#include <cmath>
#include <cstring>

namespace GameEngine::PageStreaming
{
namespace
{

static_assert(std::endian::native == std::endian::little,
              "the .gepage layout is little-endian and written with raw copies");

// The magic and the version, read before the rest of the header so a store of any version is
// named by its version.
constexpr std::size_t kPageStorePrefixBytes = 8u;

template <typename T>
void Put(std::vector<uint8>& bytes, const T& value)
{
    const auto* raw = reinterpret_cast<const uint8*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

template <typename T>
T Take(const uint8* data, std::size_t& cursor)
{
    T value{};
    std::memcpy(&value, data + cursor, sizeof(T));
    cursor += sizeof(T);
    return value;
}

bool ReadExact(SharedFileReader& file, uint64 offset, void* destination, uint64 bytes)
{
    return file.SeekTo(offset) && file.Read(destination, bytes) == static_cast<int64>(bytes);
}

std::string VersionRefusal(uint32 version)
{
    return "the page store is format " + std::to_string(version) + " and this engine reads format " +
           std::to_string(kPageStoreFormatVersion) +
           ": export the game again (in the editor, opening the scene cooks the terrain again)";
}

bool IsHeight(const PageStoreHeader& header)
{
    return header.Format == PageFieldFormat::HeightSteps;
}

// A format or filter value past the ones this engine knows was written by a newer engine; a known
// pair that is not `kind`'s is another kind's store or a damaged one.
std::string ValidateEncoding(const PageStoreHeader& header, PageFieldKind kind)
{
    if (static_cast<uint8>(header.Format) > static_cast<uint8>(PageFieldFormat::Bc5) ||
        static_cast<uint8>(header.Filter) > static_cast<uint8>(PageFieldFilter::PremultipliedTent))
        return std::string(kNewerPageStoreRefusal);
    if (PageEncodingKind(header.Format, header.Filter) != kind)
        return std::string("the page store is not a ") + PageFieldKindName(kind) +
               " store: its page format or pyramid filter is another kind's, or the file is damaged: export the game "
               "again (in the editor, opening the scene cooks the terrain again)";
    return {};
}

std::string ValidateHeader(const PageStoreHeader& header, uint32 levelCount)
{
    if (IsHeight(header) && header.Units != PageHeightUnits::Normalized && header.Units != PageHeightUnits::Absolute)
        return "unknown height units";
    if (!IsHeight(header) && (header.Units != PageHeightUnits{} || header.ReferenceRadius != 0.0))
        return "a store that holds no heights has height units or a reference radius";
    if (header.FaceCount == 0 || header.FaceCount > kPageStoreMaxFaces)
        return "face count out of range";
    if (header.FaceMap != PageFaceMap::Flat && header.FaceMap != PageFaceMap::EqualAngleCube)
        return "unknown face map";
    if (header.FaceMap == PageFaceMap::EqualAngleCube &&
        (header.FaceCount != kPageStoreMaxFaces || header.SamplesX != header.SamplesZ))
        return "a cube store has six square faces";
    if (header.SamplesX == 0 || header.SamplesZ == 0 || header.SamplesX > kPageStoreMaxSamplesPerAxis ||
        header.SamplesZ > kPageStoreMaxSamplesPerAxis)
        return "level-0 size out of range";
    if (!std::isfinite(header.ReferenceRadius) || header.ReferenceRadius < 0.0)
        return "reference radius out of range";
    if (levelCount != BuildPageStoreLevels(header.SamplesX, header.SamplesZ).size())
        return "level count disagrees with the level-0 size";
    return {};
}

// Each (face, level) range follows the one before it, holds at most its level's pages, and lists
// them in strictly increasing Morton order, so the binary search finds every entry.
bool RangesAreValid(const PageStoreLayout& layout)
{
    uint64 next = 0;
    for (std::size_t r = 0; r < layout.Ranges.size(); ++r)
    {
        const PageStoreRange& range = layout.Ranges[r];
        const PageStoreLevel& level = layout.Levels[r % layout.Levels.size()];
        if (range.First != next || range.Count > static_cast<uint64>(level.PagesX) * level.PagesZ)
            return false;
        for (uint64 i = 1; i < range.Count; ++i)
            if (layout.Entries[range.First + i - 1u].Morton >= layout.Entries[range.First + i].Morton)
                return false;
        next += range.Count;
    }
    return next == layout.Entries.size();
}

} // namespace

std::optional<PageFieldKind> PageEncodingKind(PageFieldFormat format, PageFieldFilter filter)
{
    struct KindEncoding
    {
        PageFieldKind Kind;
        PageFieldFormat Format;
        PageFieldFilter Filter;
    };
    static constexpr KindEncoding kEncodings[] = {
        {PageFieldKind::Height, PageFieldFormat::HeightSteps, PageFieldFilter::HeightTent},
        {PageFieldKind::Splat, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent},
        {PageFieldKind::Normal, PageFieldFormat::Bc5, PageFieldFilter::NormalTent},
        {PageFieldKind::GrassControl, PageFieldFormat::Rg8, PageFieldFilter::Tent},
        {PageFieldKind::Color, PageFieldFormat::Rgba8Srgb, PageFieldFilter::PremultipliedTent},
        {PageFieldKind::NormalSamples, PageFieldFormat::Rg8, PageFieldFilter::NormalTent},
    };
    for (const KindEncoding& encoding : kEncodings)
        if (encoding.Format == format && encoding.Filter == filter)
            return encoding.Kind;
    return std::nullopt;
}

const char* PageFieldKindName(PageFieldKind kind)
{
    switch (kind)
    {
    case PageFieldKind::Height:
        return "height";
    case PageFieldKind::Splat:
        return "splat";
    case PageFieldKind::Normal:
        return "normal";
    case PageFieldKind::GrassControl:
        return "grass control";
    case PageFieldKind::Color:
        return "color";
    case PageFieldKind::NormalSamples:
        return "normal samples";
    }
    return "unknown";
}

std::vector<uint8> SerializePageStoreHead(const PageStoreLayout& layout)
{
    std::vector<uint8> bytes;
    bytes.reserve(static_cast<std::size_t>(layout.DataOffset()));
    const PageStoreHeader& header = layout.Header;
    Put(bytes, kPageStoreMagic);
    Put(bytes, kPageStoreFormatVersion);
    Put(bytes, static_cast<uint8>(header.Format));
    Put(bytes, static_cast<uint8>(header.Filter));
    Put(bytes, static_cast<uint8>(header.Units));
    Put(bytes, header.FaceCount);
    Put(bytes, static_cast<uint8>(header.FaceMap));
    Put(bytes, header.SamplesX);
    Put(bytes, header.SamplesZ);
    Put(bytes, static_cast<uint32>(layout.Levels.size()));
    Put(bytes, header.ReferenceRadius);
    Put(bytes, header.Key);
    for (std::size_t level = 0; level < layout.Levels.size(); ++level)
    {
        const PageStoreLevel& shape = layout.Levels[level];
        Put(bytes, shape.SamplesX);
        Put(bytes, shape.SamplesZ);
        Put(bytes, shape.PagesX);
        Put(bytes, shape.PagesZ);
        Put(bytes, header.StepPerLevel[level]);
    }
    for (const PageStoreRange& range : layout.Ranges)
    {
        Put(bytes, range.First);
        Put(bytes, range.Count);
    }
    for (const PageIndexEntry& entry : layout.Entries)
    {
        Put(bytes, entry.Morton);
        Put(bytes, entry.Offset);
        Put(bytes, entry.Hash);
        Put(bytes, entry.MinWord);
        Put(bytes, entry.MaxWord);
        Put(bytes, entry.Flags);
    }
    return bytes;
}

std::string ReadPageStoreHead(SharedFileReader& file, PageFieldKind kind, uint64 baseOffset, uint64 availableBytes,
                              PageStoreLayout& out)
{
    uint8 fixed[kPageStoreHeaderBytes];
    if (availableBytes < kPageStorePrefixBytes || !ReadExact(file, baseOffset, fixed, kPageStorePrefixBytes))
        return "the file is shorter than a page store header";
    std::size_t cursor = 0;
    if (Take<uint32>(fixed, cursor) != kPageStoreMagic)
        return "the file is not a page store";
    if (const uint32 version = Take<uint32>(fixed, cursor); version != kPageStoreFormatVersion)
        return VersionRefusal(version);
    if (availableBytes < kPageStoreHeaderBytes ||
        !ReadExact(file, baseOffset + kPageStorePrefixBytes, fixed + kPageStorePrefixBytes,
                   kPageStoreHeaderBytes - kPageStorePrefixBytes))
        return "the file is shorter than a page store header";

    PageStoreLayout layout;
    PageStoreHeader& header = layout.Header;
    header.Format = static_cast<PageFieldFormat>(Take<uint8>(fixed, cursor));
    header.Filter = static_cast<PageFieldFilter>(Take<uint8>(fixed, cursor));
    header.Units = static_cast<PageHeightUnits>(Take<uint8>(fixed, cursor));
    header.FaceCount = Take<uint8>(fixed, cursor);
    header.FaceMap = static_cast<PageFaceMap>(Take<uint8>(fixed, cursor));
    header.SamplesX = Take<uint32>(fixed, cursor);
    header.SamplesZ = Take<uint32>(fixed, cursor);
    const uint32 levelCount = Take<uint32>(fixed, cursor);
    header.ReferenceRadius = Take<float64>(fixed, cursor);
    header.Key = Take<uint64>(fixed, cursor);
    if (std::string reason = ValidateEncoding(header, kind); !reason.empty())
        return reason;
    if (std::string reason = ValidateHeader(header, levelCount); !reason.empty())
        return "the page store header is corrupt: " + reason;

    // The expected levels follow from the validated header; the stored directory must equal them.
    layout.Levels = BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
    layout.Ranges.resize(layout.Levels.size() * header.FaceCount);
    const uint64 directoryBytes = levelCount * kPageStoreLevelBytes + layout.Ranges.size() * kPageStoreRangeBytes;
    if (kPageStoreHeaderBytes + directoryBytes > availableBytes)
        return "the page store is truncated in its directory";
    std::vector<uint8> directory(static_cast<std::size_t>(directoryBytes));
    if (!ReadExact(file, baseOffset + kPageStoreHeaderBytes, directory.data(), directory.size()))
        return "the page store's directory could not be read";
    cursor = 0;
    for (std::size_t level = 0; level < layout.Levels.size(); ++level)
    {
        const PageStoreLevel& shape = layout.Levels[level];
        const uint32 samplesX = Take<uint32>(directory.data(), cursor);
        const uint32 samplesZ = Take<uint32>(directory.data(), cursor);
        const uint32 pagesX = Take<uint32>(directory.data(), cursor);
        const uint32 pagesZ = Take<uint32>(directory.data(), cursor);
        const float32 step = Take<float32>(directory.data(), cursor);
        if (samplesX != shape.SamplesX || samplesZ != shape.SamplesZ || pagesX != shape.PagesX || pagesZ != shape.PagesZ)
            return "the page store's level directory is corrupt";
        if (!IsHeight(header))
        {
            if (step != 0.0f)
                return "the page store's level directory is corrupt: a store that holds no heights has a height step";
            continue;
        }
        if (!std::isfinite(step) || !(step > 0.0f))
            return "the page store's level directory is corrupt: a level has no positive height step";
        if (step > std::ldexp(1.0f, kHeightStepExponentMax))
            return "the page store's level directory is corrupt: a level's height step is past 2^" +
                   std::to_string(kHeightStepExponentMax);
        header.StepPerLevel[level] = step;
    }
    uint64 entryCount = 0;
    for (PageStoreRange& range : layout.Ranges)
    {
        range.First = Take<uint64>(directory.data(), cursor);
        range.Count = Take<uint64>(directory.data(), cursor);
        entryCount += range.Count;
    }

    const uint64 indexStart = kPageStoreHeaderBytes + directoryBytes;
    if (entryCount > (availableBytes - indexStart) / kPageIndexEntryBytes)
        return "the page store is truncated in its index";
    const uint64 headBytes = indexStart + entryCount * kPageIndexEntryBytes;
    std::vector<uint8> index(static_cast<std::size_t>(entryCount * kPageIndexEntryBytes));
    if (!ReadExact(file, baseOffset + indexStart, index.data(), index.size()))
        return "the page store's index could not be read";
    cursor = 0;
    layout.Entries.resize(static_cast<std::size_t>(entryCount));
    for (PageIndexEntry& entry : layout.Entries)
    {
        entry.Morton = Take<uint64>(index.data(), cursor);
        entry.Offset = Take<uint64>(index.data(), cursor);
        entry.Hash = Take<uint64>(index.data(), cursor);
        entry.MinWord = Take<int32>(index.data(), cursor);
        entry.MaxWord = Take<int32>(index.data(), cursor);
        entry.Flags = Take<uint8>(index.data(), cursor);
        if ((entry.Flags & ~kPageWideWords) != 0 || entry.MinWord > entry.MaxWord)
            return "the page store's index is corrupt";
        if (!IsHeight(header) && (entry.Flags != 0 || entry.MinWord != 0 || entry.MaxWord != 0))
            return "the page store's index is corrupt: a page that holds no heights has height bounds";
        if (entry.Offset < headBytes || entry.Offset > availableBytes ||
            layout.StoredBytes(entry) > availableBytes - entry.Offset)
            return "the page store's index points outside the store";
    }
    if (!RangesAreValid(layout))
        return "the page store's index is corrupt: its ranges are out of order";
    // The pages end where the store does: a format byte changed to a format of other page sizes
    // moves the end.
    if (layout.TotalBytes() != availableBytes)
        return "the page store is corrupt: its size does not match its pages";
    out = std::move(layout);
    return {};
}

} // namespace GameEngine::PageStreaming
