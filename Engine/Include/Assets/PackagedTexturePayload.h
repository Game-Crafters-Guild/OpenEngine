#pragma once

#include "AssetCore/GUID.h"

#include <filesystem>
#include <optional>

namespace GameEngine
{
class AssetRegistry;

// Generated export metadata: the filename, within the owning mount's Tex directory, of the
// one payload the export cooked for the texture: BCn per the texture's settings (BC6H for an
// HDR source, classified or not), uncompressed pixels for Compression None and an
// unclassified LDR texture.
inline constexpr const char* kTexturePackagedMetaKey = "assets.texture.packaged";

/// Resolve a finished package payload without reading or hashing the source image.
/// Nullopt means an authored source; an empty path means malformed package metadata.
std::optional<std::filesystem::path> ResolvePackagedTexturePayload(const AssetRegistry& registry, const GUID& guid);
} // namespace GameEngine
