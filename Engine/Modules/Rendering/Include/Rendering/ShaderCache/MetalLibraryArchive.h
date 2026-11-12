#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Rendering
{

/// The file an export ships in its bundled shader cache (.Cache/Shaders) with the Metal libraries it
/// compiled offline.
inline constexpr std::string_view kMetalLibraryArchiveFileName = "MetalLibraries.bin";

/// One stage compiled offline: a .metallib whose entry point is main0, found by the stage's
/// translation key (ComputeTranslationHash). Compute, mesh and task stages carry their workgroup size.
struct MetalLibraryArchiveEntry
{
    uint64_t Key = 0;
    uint32_t LocalSizeX = 0;
    uint32_t LocalSizeY = 0;
    uint32_t LocalSizeZ = 0;
    std::span<const uint8_t> Library;
};

/// The archive for @p entries, built under the translator and compile options @p optionsHash
/// (ComputeMetalLibraryOptionsHash). Keys must be unique.
std::vector<uint8_t> WriteMetalLibraryArchive(uint64_t optionsHash, std::span<const MetalLibraryArchiveEntry> entries);

/// The options hash and the entries of an archive; each entry's Library points into @p bytes.
/// False, with the reason, for bytes that are not a complete archive of this format version.
bool ReadMetalLibraryArchive(std::span<const uint8_t> bytes,
                             uint64_t& outOptionsHash,
                             std::vector<MetalLibraryArchiveEntry>& outEntries,
                             std::string& outError);

} // namespace GameEngine::Rendering
