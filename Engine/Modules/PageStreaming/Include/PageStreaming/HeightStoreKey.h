#pragma once

#include "AssetCore/GUID.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Types/Types.h"

#include <array>
#include <filesystem>
#include <optional>

namespace GameEngine::PageStreaming
{

/// The file a height store is cooked from.
enum class HeightSourceFormat : uint8
{
    R16 = 0,   ///< raw 16-bit unsigned samples, row-major
    R32 = 1,   ///< raw 32-bit float samples, row-major
    Png16 = 2, ///< a 16-bit grayscale PNG
};

/// The source file's content, as one full read of it gives: the identity of a store's source.
/// The asset registry's own fingerprint is not an identity for a heightmap: past 256 KB it hashes
/// a sparse sample of the file, so an edit in the middle of a DEM keeps it.
struct HeightSourceIdentity
{
    uint64 ContentHash = 0; ///< FNV-1a 64 of every byte of the file
    /// The grid the steps were chosen on: the import grid the read used.
    uint32 SamplesX = 0;
    uint32 SamplesZ = 0;
    /// Each level's step as a power of two, 2^StepExponents[L] in the source's units, chosen from
    /// the samples (R32 sources; a 16-bit source's step is kNormalizedHeightStep at every level).
    std::array<int8, kPageStoreMaxLevels> StepExponents{};
};

/// Everything that decides a height store's bytes. Deliberately nothing else: no entity, scene,
/// runtime handle, streaming budget, material or setting that leaves the bytes unchanged, so two
/// terrains in two scenes on the same heightmap with the same import settings share one store.
struct HeightStoreCookInputs
{
    uint64 SourceContentHash = 0;
    HeightSourceFormat Format = HeightSourceFormat::R32;
    uint32 SamplesX = 0; ///< the import grid
    uint32 SamplesZ = 0;
};

/// The cook key: FNV-1a 64 seeded with kPageStoreFormatVersion over the inputs and the height
/// filter. The steps follow from the content and the grid, and the version names their rule.
uint64 ComputeHeightStoreKey(const HeightStoreCookInputs& inputs);

/// The store of `asset` cooked under `key`: <directory>/<asset GUID>-<key, 16 hex digits>.gepage.
std::filesystem::path HeightStoreFile(const std::filesystem::path& directory, const GUID& asset, uint64 key);

/// The identity remembered for `source`'s file at its current modification time, size and file
/// identity, or none when there is no memo for `asset` in `directory` or the file has changed since
/// (then a full read gives the identity: ScanHeightSource). The caller compares the memo's grid
/// with its own: a memo of another grid chose its steps for that grid.
std::optional<HeightSourceIdentity> ReadHeightSourceMemo(const std::filesystem::path& directory, const GUID& asset,
                                                         const std::filesystem::path& source);

/// Remembers `identity` for `source`'s file as it is now, in <directory>/<asset GUID>.source.
/// False on an I/O failure.
bool WriteHeightSourceMemo(const std::filesystem::path& directory, const GUID& asset,
                           const std::filesystem::path& source, const HeightSourceIdentity& identity);

} // namespace GameEngine::PageStreaming
