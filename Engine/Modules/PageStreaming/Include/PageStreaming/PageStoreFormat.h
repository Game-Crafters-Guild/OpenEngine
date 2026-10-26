#pragma once

#include "PageStreaming/PageAddress.h"
#include "Types/Types.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine::Mathematics
{
struct Vector3;
}

namespace GameEngine::PageStreaming
{

// The page store (.gepage): one field of a terrain as a pyramid of fixed-size pages.
//
// The field is a lattice: level-0 sample i sits at field coordinate i / (samples - 1), and level N
// has sample j at the position of level-0 sample j * 2^N. A level's samples continue past the
// field's last sample when (samples - 1) is not a multiple of 2^N; such samples evaluate the
// field's filter over edge-clamped inputs, so every level covers the whole field. A page owns
// kPageOwnedSamples samples per axis (page p owns samples 128p to 128p + 127) and stores an apron
// of the neighbors' edge samples on each side, so bilinear filtering and central differences
// inside a page never read another page; past the field's edge the apron repeats the edge sample.
// The apron is one sample (a stride of 130), or one 4 x 4 block (a stride of 136) for a block
// format: 128 is a multiple of 4, so the block grid sits on the page's origin and an apron block
// encodes the same 16 samples as the neighbor's edge block, bit for bit.
//
// Heights are integers in their level's step: a sample is h = StepPerLevel[L] x k, relative to
// the reference radius (h = r - R; R is 0 on a flat store). A page stores k as 16-bit words above
// its base (k = MinWord + word) when its range fits, else as 32-bit integers at the same step. The
// apron sample a page stores and the same sample its neighbor owns are one integer k in both, so
// they decode to one float, bit for bit, at every level.
//
// On disk, little-endian: the fixed header, the level directory (each level's shape and step), the
// (face, level) directory of ranges into the index, the index of present pages only (sorted by
// face, level and Morton code), then the pages in the order the cook wrote them. Offsets are from
// the start of the store, so a store packed into a terrain container (TerrainPageContainer.h) is
// read at a base offset unchanged.

/// Samples a page owns per axis.
inline constexpr uint32 kPageOwnedSamples = 128u;
/// Apron samples a page of a lattice format stores beyond its owned samples on each side.
inline constexpr uint32 kPageApronSamples = 1u;
/// Samples a page of a lattice format stores per axis: its owned samples and the apron on both sides.
inline constexpr uint32 kPageStrideSamples = kPageOwnedSamples + 2u * kPageApronSamples;
/// Samples a page of a lattice format stores.
inline constexpr uint32 kPageSampleCount = kPageStrideSamples * kPageStrideSamples;
/// Texels per axis of a block format's block, and its apron on each side.
inline constexpr uint32 kPageBlockTexels = 4u;
/// Bytes of one BC5 block: two BC4 blocks (one a channel) of 8 bytes.
inline constexpr uint32 kBc5BlockBytes = 16u;
/// Version of the .gepage layout and of every rule that decides its bytes (the level rule, the
/// filters, the encoding and its step rule). It seeds every cook key, so a bump turns every store
/// into a miss.
inline constexpr uint32 kPageStoreFormatVersion = 2u;
/// The most faces a store holds (a cube's six).
inline constexpr uint32 kPageStoreMaxFaces = 6u;
/// The most levels a store holds.
inline constexpr uint32 kPageStoreMaxLevels = 32u;
/// The most level-0 samples along either axis of a store: 2^30 (a cube face of Earth at about
/// 1 cm). PageAddress's 32-bit X and Z hold its 2^23 pages per axis; page counts are 64-bit.
inline constexpr uint32 kPageStoreMaxSamplesPerAxis = 1u << 30u;

/// What a store's field is to the terrain. The terrain container's field directory names its
/// stores by this.
enum class PageFieldKind : uint8
{
    Height = 0,
    Splat = 1,        ///< four material weights summing to 255
    Normal = 2,       ///< the surface normal's x and z
    GrassControl = 3, ///< the grass height and density targets
    Color = 4,        ///< the premultiplied color map and its weight
    /// A Normal store's unencoded samples, from which its coarser pages are filtered. A bake cache
    /// beside the store: never in a terrain container.
    NormalSamples = 5,
};

/// How a page's samples are stored.
enum class PageFieldFormat : uint8
{
    /// Integers in the level's step: 16-bit words above the page's base, or 32-bit integers on a
    /// page whose entry has kPageWideWords.
    HeightSteps = 0,
    /// Four linear bytes a sample.
    Rgba8 = 1,
    /// Four bytes a sample: rgb sRGB-encoded, a linear.
    Rgba8Srgb = 2,
    /// Two linear bytes a sample.
    Rg8 = 3,
    /// Two linear bytes a sample (unsigned normalized), stored as BC5 blocks of 4 x 4 samples.
    Bc5 = 4,
};

/// How a coarser level is made from the level below it.
enum class PageFieldFilter : uint8
{
    /// The [1 2 1] / 4 tent per axis centered on the coarser sample's own position, so the
    /// lattice keeps its positions at every level.
    HeightTent = 0,
    /// The tent per channel on four weights, renormalized to sum to 255 with the remainder on the
    /// largest channel.
    WeightsTent = 1,
    /// The tent on unit normals stored as (x, z), y = sqrt(1 - x^2 - z^2), renormalized.
    NormalTent = 2,
    /// The tent per channel on continuous bytes, rounded to nearest.
    Tent = 3,
    /// The tent in linear light on premultiplied color: rgb decoded from sRGB, a linear, then rgb
    /// encoded back.
    PremultipliedTent = 4,
};

/// What a height value is in the world.
enum class PageHeightUnits : uint8
{
    Normalized = 0, ///< [0, 1], scaled by the terrain's Height Scale (.r16 and 16-bit PNG sources).
    Absolute = 1,   ///< The source's own values, scaled by the Height Scale (.r32: meters at Height Scale 1).
};

/// How a face's page grid maps to the surface.
enum class PageFaceMap : uint8
{
    Flat = 0, ///< one face on the plane
    /// Six square cube faces on the sphere, in the order of Mathematics::kCubeFaces, on the
    /// equal-angle lattice. A face's sample (0, 0) is its corner 0, and X and Z run to its corners 1
    /// and 3 (Mathematics::kCubeFaceAxes).
    EqualAngleCube = 1,
};

/// A side of a cube face's sample grid.
enum class PageFaceSide : uint8
{
    MinZ = 0, ///< the first row
    MaxZ = 1, ///< the last row
    MinX = 2, ///< the first column
    MaxX = 3, ///< the last column
};

/// Sides of a face.
inline constexpr uint32 kPageFaceSides = 4u;

/// Where a cube face's side meets the next face: the face and its side that share the edge, and
/// whether the sample index along the edge runs the other way on that face.
struct PageFaceEdge
{
    uint8 Face = 0;
    PageFaceSide Side = PageFaceSide::MinZ;
    bool Reversed = false;
};

/// The face of an EqualAngleCube store that shares `side` of `face`.
PageFaceEdge PageFaceNeighbor(uint8 face, PageFaceSide side);

/// The equal-angle coordinate s in [-1, 1] of the gnomonic (straight cube) coordinate a in [-1, 1]:
/// p(a) = a + a (a^2 - 1) r(a^2), r a cubic fit to (4 / pi) atan(a), odd and exact at -1, 0 and 1.
/// A format constant, in double precision with contraction off: a new coefficient changes every cooked planet byte.
float64 EqualAngleFaceCoordinate(float64 gnomonic);

/// The gnomonic coordinate whose equal-angle coordinate is `equalAngle` in [-1, 1]: Newton on
/// EqualAngleFaceCoordinate, a fixed count of steps from a = s, so every machine computes the same
/// value. Odd bit for bit, exact at -1, 0 and 1.
float64 GnomonicFaceCoordinate(float64 equalAngle);

/// The equal-angle coordinate of level-0 sample `index` of a face of `samples` samples a side:
/// (2 index - (samples - 1)) / (samples - 1), so index and samples - 1 - index give s and -s bit
/// for bit, and the face's first and last samples sit on its edges.
float64 EqualAngleSampleCoordinate(uint32 index, uint32 samples);

/// The fixed header of a store. Units, ReferenceRadius and StepPerLevel describe a HeightSteps
/// store and are zero on every other format.
struct PageStoreHeader
{
    PageFieldFormat Format = PageFieldFormat::HeightSteps;
    PageFieldFilter Filter = PageFieldFilter::HeightTent;
    PageHeightUnits Units = PageHeightUnits::Absolute;
    uint8 FaceCount = 1;
    PageFaceMap FaceMap = PageFaceMap::Flat;
    uint32 SamplesX = 0; ///< level-0 samples along X
    uint32 SamplesZ = 0; ///< level-0 samples along Z
    /// R: a decoded sample is h = r - R. 0 on a flat store.
    float64 ReferenceRadius = 0.0;
    /// Level L's step in the store's height units: positive and finite for every level the store
    /// has, 0 past them (MakePageStoreLayout clears those).
    std::array<float32, kPageStoreMaxLevels> StepPerLevel{};
    uint64 Key = 0; ///< the cook key the store was made under (HeightStoreKey.h)
};

/// One level of a store.
struct PageStoreLevel
{
    uint32 SamplesX = 0;
    uint32 SamplesZ = 0;
    uint32 PagesX = 0;
    uint32 PagesZ = 0;
    /// The level's first page in a dense row-major table of every page of one face's levels: the
    /// position the dense consumers (the page tables, the CPU directory) give the level. Not on disk.
    uint64 FirstEntry = 0;
};

/// PageIndexEntry::Flags: the page stores 32-bit integers rather than 16-bit words (HeightSteps only).
inline constexpr uint8 kPageWideWords = 1u;

/// One present page's entry in the index. Serialized field by field as kPageIndexEntryBytes.
struct PageIndexEntry
{
    uint64 Morton = 0; ///< PageMortonCode of the page in its level: the key the index is sorted by
    uint64 Offset = 0; ///< from the start of the store; 0 until the page is written
    uint64 Hash = 0;   ///< FNV-1a 64 of the page's stored bytes
    /// The lowest and highest value of the page as integers in its level's step: the floor and the
    /// ceiling over the level-0 values under its footprint (shared edge included) and every sample
    /// it stores (apron included). MinWord is the base of its 16-bit words. Zero on every format but
    /// HeightSteps.
    int32 MinWord = 0;
    int32 MaxWord = 0;
    uint8 Flags = 0; ///< kPageWideWords

    bool HasWideWords() const { return (Flags & kPageWideWords) != 0; }
};

/// Morton, offset, hash, min word, max word, flags.
inline constexpr std::size_t kPageIndexEntryBytes = 8u * 3u + 4u * 2u + 1u;
static_assert(kPageIndexEntryBytes == 33u);

/// The index entries of one level of one face: Entries[First, First + Count).
struct PageStoreRange
{
    uint64 First = 0;
    uint64 Count = 0;
};

/// A store's shape: its header, levels and index.
struct PageStoreLayout
{
    PageStoreHeader Header;
    std::vector<PageStoreLevel> Levels;
    std::vector<PageStoreRange> Ranges;  ///< one per (face, level): Ranges[face * Levels.size() + level]
    std::vector<PageIndexEntry> Entries; ///< present pages only, sorted by (face, level, Morton)

    /// True when the address is inside the store, present or not.
    bool Contains(const PageAddress& address) const;
    /// The index of the address's entry (a binary search in its level's range), or none when the
    /// page is absent or outside the store.
    std::optional<std::size_t> PresentIndex(const PageAddress& address) const;
    /// The address's entry when the page is present, else null.
    const PageIndexEntry* FindPresent(const PageAddress& address) const;
    /// The address itself or its nearest present ancestor; none when the address is outside the
    /// store or no ancestor is present.
    std::optional<PageAddress> ResolvePresent(PageAddress address) const;
    /// The stored bytes of the page of `entry`.
    uint32 StoredBytes(const PageIndexEntry& entry) const;
    /// Bytes of everything before the first page.
    uint64 DataOffset() const;
    /// Bytes of the whole store.
    uint64 TotalBytes() const;
};

/// The levels of a field of `samplesX` x `samplesZ` level-0 samples: each coarser level has
/// samples / 2 + 1 per axis (the lattice covering the level below), down to the first level that
/// fits one page.
std::vector<PageStoreLevel> BuildPageStoreLevels(uint32 samplesX, uint32 samplesZ);

/// The pages of every level of one face of `levels`: the length of a face's dense table.
uint64 DensePageCount(std::span<const PageStoreLevel> levels);

/// True when `format` stores compressed blocks (Bc5): a loaded page holds the stored blocks, and
/// the GPU samples them as they are.
bool IsBlockFormat(PageFieldFormat format);
/// Samples a page of `format` stores per axis: kPageStrideSamples, or 136 for a block format (its
/// owned samples and a block on each side).
uint32 PageStrideTexels(PageFieldFormat format);
/// Bytes of one decoded sample of `format`: a float32 height, or the format's channels.
uint32 PageTexelBytes(PageFieldFormat format);
/// Stores the unit normal `normal` as a normal sample: (x, z) * 0.5 + 0.5 in two unsigned bytes.
void EncodeNormalSample(const Mathematics::Vector3& normal, uint8* outTexel);
/// The stored bytes of one page of `format`; a HeightSteps page holds 16-bit words, or 32-bit
/// integers when `wide`.
uint32 PageStoredBytes(PageFieldFormat format, bool wide);
/// The bytes one loaded page of `format` holds, on the CPU and in its cache slot: its decoded
/// samples (PageStrideTexels(format)^2 of PageTexelBytes(format) bytes), or for a block format its
/// stored blocks, which the GPU samples as they are.
uint32 PagePayloadBytes(PageFieldFormat format);
/// A loaded height page's bytes as its kPageSampleCount float32 heights (row-major, apron
/// included): the one view every height reader takes of a page's bytes.
std::span<const float32> HeightSamples(std::span<const uint8> bytes);
/// The same view, writable: the page's heights change in place.
std::span<float32> HeightSamples(std::vector<uint8>& bytes);

/// The layout of a store with `header`: every level, and an entry for each page that `present`
/// (one byte per page of the dense table, face-major, then level, then row-major) marks non-zero;
/// an empty `present` makes every page present. Offsets are assigned as the pages are written
/// (PageStoreWriter).
PageStoreLayout MakePageStoreLayout(const PageStoreHeader& header, std::span<const uint8> present = {});

/// The layout of `layout`'s store with more pages present: its own present pages keep their
/// entries, and each page `present` marks (as MakePageStoreLayout reads it) is added unwritten.
/// For a store that grows as its pages are baked; PageStoreWriter::OpenForPatch takes the result.
PageStoreLayout AddPresentPages(const PageStoreLayout& layout, std::span<const uint8> present);

} // namespace GameEngine::PageStreaming
