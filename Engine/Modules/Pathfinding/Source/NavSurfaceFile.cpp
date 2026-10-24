#include "Pathfinding/NavSurfaceFile.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace GameEngine::Pathfinding
{

namespace
{

constexpr char kMagic[4] = {'G', 'E', 'N', 'V'};
// Packed record sizes, in bytes, as the layout in NavSurfaceFile.h lists their fields.
constexpr size_t kHeaderBytes = 4 + 4 + 8 + 2 + 1 + 2 + 8 + 1 + 8 + 8;
constexpr size_t kProfileBytes = 4 + 4 * 4 + 1 + 8 + 4 + 8;
constexpr size_t kAreaBytes = 1 + 8 + 4 + 2;
constexpr size_t kChartBytes = 4 + 3 * 4 + 3 * 4 + 4 * 4 + 4 + 4 + 8 + 8;
constexpr size_t kIndexEntryBytes = 4 + 4 + 8 + 4 + 4 + 8;

class Writer
{
  public:
    explicit Writer(std::vector<uint8>& out) : m_Out(out) {}

    template <typename T>
    void Put(T value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* bytes = reinterpret_cast<const uint8*>(&value);
        m_Out.insert(m_Out.end(), bytes, bytes + sizeof(T));
    }

    // Writes `value` over the bytes at `position`, written earlier as a placeholder.
    template <typename T>
    void PutAt(size_t position, T value)
    {
        std::memcpy(m_Out.data() + position, &value, sizeof(T));
    }

    void PutBytes(const std::vector<uint8>& bytes) { m_Out.insert(m_Out.end(), bytes.begin(), bytes.end()); }
    size_t Position() const { return m_Out.size(); }

  private:
    std::vector<uint8>& m_Out;
};

class Reader
{
  public:
    explicit Reader(std::span<const uint8> bytes) : m_Bytes(bytes) {}

    bool Seek(uint64 position)
    {
        if (position > m_Bytes.size())
            return false;
        m_Position = static_cast<size_t>(position);
        return true;
    }

    template <typename T>
    bool Get(T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (m_Bytes.size() - m_Position < sizeof(T))
            return false;
        std::memcpy(&value, m_Bytes.data() + m_Position, sizeof(T));
        m_Position += sizeof(T);
        return true;
    }

    // True when `count` records of `size` bytes fit between `offset` and the end of the file.
    bool Fits(uint64 offset, uint64 count, uint64 size) const
    {
        return offset <= m_Bytes.size() && count <= (m_Bytes.size() - offset) / std::max<uint64>(size, 1);
    }

    std::span<const uint8> Bytes() const { return m_Bytes; }

  private:
    std::span<const uint8> m_Bytes;
    size_t m_Position = 0;
};

// Spreads the 32 bits of `value` over the even bits of the result.
uint64 SpreadBits(uint32 value)
{
    uint64 bits = value;
    bits = (bits | (bits << 16)) & 0x0000FFFF0000FFFFull;
    bits = (bits | (bits << 8)) & 0x00FF00FF00FF00FFull;
    bits = (bits | (bits << 4)) & 0x0F0F0F0F0F0F0F0Full;
    bits = (bits | (bits << 2)) & 0x3333333333333333ull;
    bits = (bits | (bits << 1)) & 0x5555555555555555ull;
    return bits;
}

// The Morton code of a signed tile coordinate, with the coordinates biased so negative tiles sort
// before positive ones along each axis.
uint64 MortonOf(const NavSurfaceTile& tile)
{
    constexpr uint32 kBias = 1u << 31;
    return SpreadBits(static_cast<uint32>(tile.X) + kBias) | (SpreadBits(static_cast<uint32>(tile.Z) + kBias) << 1);
}

void WriteChartRecord(Writer& writer, const NavMeshChart& chart, uint32 tileCount)
{
    writer.Put(chart.Id);
    writer.Put(chart.Origin.SectorX);
    writer.Put(chart.Origin.SectorY);
    writer.Put(chart.Origin.SectorZ);
    writer.Put(chart.Origin.LocalX);
    writer.Put(chart.Origin.LocalY);
    writer.Put(chart.Origin.LocalZ);
    const auto& rotation = chart.Rotation.GetGLM();
    for (const float32 component : {rotation.x, rotation.y, rotation.z, rotation.w})
        writer.Put(component);
    writer.Put(chart.Span);
    writer.Put(tileCount);
}

bool Fail(std::string& outError, NavSurfaceData& out, std::string message)
{
    outError = std::move(message);
    out = {};
    return false;
}

bool ReadChart(Reader& reader, NavSurfaceChart& chart, uint64& outIndexOffset, uint64& outDataOffset,
               uint32& outTileCount)
{
    NavMeshChart& record = chart.Chart;
    bool ok = reader.Get(record.Id) && reader.Get(record.Origin.SectorX) && reader.Get(record.Origin.SectorY) &&
              reader.Get(record.Origin.SectorZ) && reader.Get(record.Origin.LocalX) &&
              reader.Get(record.Origin.LocalY) && reader.Get(record.Origin.LocalZ);
    float32 rotation[4] = {};
    for (float32& component : rotation)
        ok = ok && reader.Get(component);
    record.Rotation = Mathematics::Quaternion(rotation[3], rotation[0], rotation[1], rotation[2]);
    return ok && reader.Get(record.Span) && reader.Get(outTileCount) && reader.Get(outIndexOffset) &&
           reader.Get(outDataOffset);
}

// A byte range of the file: a chart's index or its tile block.
struct Region
{
    uint64 Begin = 0;
    uint64 End = 0;

    bool Overlaps(const Region& other) const { return Begin < other.End && other.Begin < End; }
};

// True when a non-empty `region` shares a byte with one of `regions`.
bool OverlapsAny(const std::vector<Region>& regions, const Region& region)
{
    return region.End > region.Begin &&
           std::any_of(regions.begin(), regions.end(), [&](const Region& r) { return r.Overlaps(region); });
}

// Adds `region` to `regions` unless it shares a byte with one of them.
bool Claim(std::vector<Region>& regions, const Region& region)
{
    if (OverlapsAny(regions, region))
        return false;
    regions.push_back(region);
    return true;
}

// Reads a chart's index and its tiles. The tiles lie one after another in the chart's data block,
// in index order from its start, as the writer lays them out. Neither the index nor the block may
// share a byte with any other region the file names (`regions`, which gains both), so the tiles read
// never add up to more than the file.
bool ReadTiles(Reader& reader, uint64 indexOffset, uint64 dataOffset, uint32 tileCount, NavSurfaceChart& chart,
               std::vector<Region>& regions)
{
    const std::span<const uint8> bytes = reader.Bytes();
    if (tileCount > kNavChartTilePool || !reader.Fits(indexOffset, tileCount, kIndexEntryBytes) ||
        dataOffset > bytes.size() || !reader.Seek(indexOffset))
        return false;
    const Region index{indexOffset, indexOffset + static_cast<uint64>(tileCount) * kIndexEntryBytes};
    if (OverlapsAny(regions, index))
        return false;
    chart.Tiles.resize(tileCount);
    std::vector<uint32> sizes(tileCount);
    uint64 blockSize = 0;
    for (uint32 i = 0; i < tileCount; ++i)
    {
        NavSurfaceTile& tile = chart.Tiles[i];
        uint64 offset = 0;
        if (!reader.Get(tile.X) || !reader.Get(tile.Z) || !reader.Get(offset) || !reader.Get(sizes[i]) ||
            !reader.Get(tile.PolygonCount) || !reader.Get(tile.ContentHash))
            return false;
        if (offset != blockSize || sizes[i] > bytes.size() - dataOffset - blockSize)
            return false;
        blockSize += sizes[i];
    }
    const Region block{dataOffset, dataOffset + blockSize};
    if (OverlapsAny(regions, block) || index.Overlaps(block))
        return false;
    uint64 at = dataOffset;
    for (uint32 i = 0; i < tileCount; ++i)
    {
        const auto first = bytes.begin() + static_cast<std::ptrdiff_t>(at);
        chart.Tiles[i].Data.assign(first, first + sizes[i]);
        at += sizes[i];
    }
    regions.push_back(index);
    regions.push_back(block);
    return true;
}

} // namespace

std::vector<NavSurfaceArea> DefaultNavSurfaceAreas()
{
    return {{kNavAreaNull, HashStringId("Null"), 1.0f, 0u}, {kNavAreaWalkable, HashStringId("Walkable"), 1.0f, 1u}};
}

std::array<StringId, kNavSurfaceFlagBits> DefaultNavSurfaceFlagNames()
{
    std::array<StringId, kNavSurfaceFlagBits> names{};
    names[0] = HashStringId("Walk");
    return names;
}

bool WriteNavSurface(const NavSurfaceData& data, std::vector<uint8>& out)
{
    out.clear();
    if (data.Profiles.size() > std::numeric_limits<uint16>::max() ||
        data.Areas.size() > std::numeric_limits<uint8>::max())
        return false;
    for (const NavSurfaceProfile& profile : data.Profiles)
        for (const NavSurfaceChart& chart : profile.Charts)
            if (chart.Tiles.size() > kNavChartTilePool)
                return false;

    Writer writer(out);
    for (const char c : kMagic)
        writer.Put(c);
    writer.Put(kNavSurfaceFormatVersion);
    writer.Put(data.BakeSettingsHash);
    writer.Put(data.TileCells);
    writer.Put(static_cast<uint8>(data.Lifecycle));
    writer.Put(static_cast<uint16>(data.Profiles.size()));
    const size_t profileTableAt = writer.Position();
    writer.Put(uint64{0});
    writer.Put(static_cast<uint8>(data.Areas.size()));
    const size_t areaTableAt = writer.Position();
    writer.Put(uint64{0});
    const size_t flagNamesAt = writer.Position();
    writer.Put(uint64{0});

    writer.PutAt(profileTableAt, static_cast<uint64>(writer.Position()));
    std::vector<size_t> chartTableAt;
    for (const NavSurfaceProfile& profile : data.Profiles)
    {
        writer.Put(profile.Id);
        writer.Put(profile.Radius);
        writer.Put(profile.Height);
        writer.Put(profile.MaxSlope);
        writer.Put(profile.StepHeight);
        writer.Put(static_cast<uint8>(profile.AlwaysBake ? 1 : 0));
        writer.Put(profile.SettingsHash);
        writer.Put(static_cast<uint32>(profile.Charts.size()));
        chartTableAt.push_back(writer.Position());
        writer.Put(uint64{0});
    }

    writer.PutAt(areaTableAt, static_cast<uint64>(writer.Position()));
    for (const NavSurfaceArea& area : data.Areas)
    {
        writer.Put(area.Id);
        writer.Put(area.Name);
        writer.Put(area.DefaultCost);
        writer.Put(area.DefaultFlags);
    }
    writer.PutAt(flagNamesAt, static_cast<uint64>(writer.Position()));
    for (const StringId name : data.FlagNames)
        writer.Put(name);

    // Chart tables, each chart with placeholders for its index and data offsets.
    struct ChartPlacement
    {
        const NavSurfaceChart* Chart = nullptr;
        size_t IndexOffsetAt = 0;
        size_t DataOffsetAt = 0;
        std::vector<const NavSurfaceTile*> Order;
    };
    std::vector<ChartPlacement> placements;
    for (size_t p = 0; p < data.Profiles.size(); ++p)
    {
        writer.PutAt(chartTableAt[p], static_cast<uint64>(writer.Position()));
        for (const NavSurfaceChart& chart : data.Profiles[p].Charts)
        {
            ChartPlacement placement;
            placement.Chart = &chart;
            WriteChartRecord(writer, chart.Chart, static_cast<uint32>(chart.Tiles.size()));
            placement.IndexOffsetAt = writer.Position();
            writer.Put(uint64{0});
            placement.DataOffsetAt = writer.Position();
            writer.Put(uint64{0});
            for (const NavSurfaceTile& tile : chart.Tiles)
                placement.Order.push_back(&tile);
            std::sort(placement.Order.begin(), placement.Order.end(),
                      [](const NavSurfaceTile* a, const NavSurfaceTile* b) { return MortonOf(*a) < MortonOf(*b); });
            placements.push_back(std::move(placement));
        }
    }

    // Each chart's index, then its tiles' bytes in the same order.
    for (const ChartPlacement& placement : placements)
    {
        writer.PutAt(placement.IndexOffsetAt, static_cast<uint64>(writer.Position()));
        uint64 offset = 0;
        for (const NavSurfaceTile* tile : placement.Order)
        {
            writer.Put(tile->X);
            writer.Put(tile->Z);
            writer.Put(offset);
            writer.Put(static_cast<uint32>(tile->Data.size()));
            writer.Put(tile->PolygonCount);
            writer.Put(tile->ContentHash);
            offset += tile->Data.size();
        }
    }
    for (const ChartPlacement& placement : placements)
    {
        writer.PutAt(placement.DataOffsetAt, static_cast<uint64>(writer.Position()));
        for (const NavSurfaceTile* tile : placement.Order)
            writer.PutBytes(tile->Data);
    }
    return true;
}

bool ReadNavSurface(std::span<const uint8> bytes, NavSurfaceData& out, std::string& outError)
{
    out = {};
    Reader reader(bytes);
    char magic[4] = {};
    uint32 version = 0;
    if (bytes.size() < kHeaderBytes || !reader.Get(magic) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        return Fail(outError, out, "not a .navsurface file");
    reader.Get(version);
    if (version != kNavSurfaceFormatVersion)
        return Fail(outError, out,
                    "the file is format version " + std::to_string(version) + " and this engine reads version " +
                        std::to_string(kNavSurfaceFormatVersion) + ": bake the surface again");

    uint8 lifecycle = 0;
    uint16 profileCount = 0;
    uint8 areaCount = 0;
    uint64 profileTable = 0, areaTable = 0, flagNames = 0;
    reader.Get(out.BakeSettingsHash);
    reader.Get(out.TileCells);
    reader.Get(lifecycle);
    reader.Get(profileCount);
    reader.Get(profileTable);
    reader.Get(areaCount);
    reader.Get(areaTable);
    reader.Get(flagNames);
    if (out.TileCells != kNavMeshTileCells || lifecycle > static_cast<uint8>(NavSurfaceLifecycle::Cache))
        return Fail(outError, out, "the header is not one this engine writes");
    out.Lifecycle = static_cast<NavSurfaceLifecycle>(lifecycle);

    if (!reader.Fits(areaTable, areaCount, kAreaBytes) || !reader.Seek(areaTable))
        return Fail(outError, out, "the area table lies outside the file");
    out.Areas.resize(areaCount);
    for (NavSurfaceArea& area : out.Areas)
        if (!reader.Get(area.Id) || !reader.Get(area.Name) || !reader.Get(area.DefaultCost) ||
            !reader.Get(area.DefaultFlags))
            return Fail(outError, out, "the area table is truncated");
    if (!reader.Fits(flagNames, kNavSurfaceFlagBits, sizeof(StringId)) || !reader.Seek(flagNames))
        return Fail(outError, out, "the flag names lie outside the file");
    for (StringId& name : out.FlagNames)
        reader.Get(name);

    if (!reader.Fits(profileTable, profileCount, kProfileBytes))
        return Fail(outError, out, "the profile table lies outside the file");
    // Every region the file names (the header, the tables, each chart table, each index and tile
    // block) lies inside the file and shares no byte with another, so nothing is read twice and what
    // the reader allocates is bounded by the file's size.
    std::vector<Region> regions;
    if (!Claim(regions, {0, kHeaderBytes}) ||
        !Claim(regions, {profileTable, profileTable + static_cast<uint64>(profileCount) * kProfileBytes}) ||
        !Claim(regions, {areaTable, areaTable + static_cast<uint64>(areaCount) * kAreaBytes}) ||
        !Claim(regions, {flagNames, flagNames + kNavSurfaceFlagBits * sizeof(StringId)}))
        return Fail(outError, out, "two of the file's tables share bytes");
    out.Profiles.resize(profileCount);
    for (size_t p = 0; p < out.Profiles.size(); ++p)
    {
        NavSurfaceProfile& profile = out.Profiles[p];
        uint8 alwaysBake = 0;
        uint32 chartCount = 0;
        uint64 chartTable = 0;
        if (!reader.Seek(profileTable + p * kProfileBytes) || !reader.Get(profile.Id) || !reader.Get(profile.Radius) ||
            !reader.Get(profile.Height) || !reader.Get(profile.MaxSlope) || !reader.Get(profile.StepHeight) ||
            !reader.Get(alwaysBake) || !reader.Get(profile.SettingsHash) || !reader.Get(chartCount) ||
            !reader.Get(chartTable))
            return Fail(outError, out, "the profile table is truncated");
        profile.AlwaysBake = alwaysBake != 0;
        if (!reader.Fits(chartTable, chartCount, kChartBytes) ||
            !Claim(regions, {chartTable, chartTable + static_cast<uint64>(chartCount) * kChartBytes}))
            return Fail(outError, out, "a chart table lies outside the file or under another table");
        profile.Charts.resize(chartCount);
        for (size_t c = 0; c < profile.Charts.size(); ++c)
        {
            uint64 indexOffset = 0, dataOffset = 0;
            uint32 tileCount = 0;
            if (!reader.Seek(chartTable + c * kChartBytes) ||
                !ReadChart(reader, profile.Charts[c], indexOffset, dataOffset, tileCount))
                return Fail(outError, out, "a chart table is truncated");
            if (!ReadTiles(reader, indexOffset, dataOffset, tileCount, profile.Charts[c], regions))
                return Fail(outError, out, "a chart's tiles lie outside the file, out of order or under another chart's");
        }
    }
    return true;
}

} // namespace GameEngine::Pathfinding
