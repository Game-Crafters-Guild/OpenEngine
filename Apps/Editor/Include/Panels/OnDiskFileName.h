#pragma once

// Recovers a file's real on-disk name for display.
//
// Paths reach the editor's open-asset entry points from two provenances that disagree about
// case on macOS and Windows: the asset browser enumerates the filesystem and carries the real
// name, while anything resolved through the AssetRegistry carries a case-FOLDED path, because
// NormalizePathForMap folds keys on case-insensitive filesystems to match how fs::open itself
// resolves them. Showing either verbatim is how the same shader ends up titled "Rock.glsl"
// opened from the browser and "rock.glsl" opened from the material inspector.
//
// Only the file name is corrected — that is what the Script Editor titles its tab with.

#include "Types/StringUtils.h"

#include <filesystem>
#include <string>

namespace GameEngine::Editor
{

inline std::string OnDiskFileName(const std::filesystem::path& path)
{
    std::string name = path.filename().string();
    if (name.empty())
        return name;

    const std::filesystem::path parent = path.parent_path();
    if (parent.empty())
        return name;

    std::error_code ec;
    std::filesystem::directory_iterator it(parent, ec);
    if (ec)
        return name;

    for (const auto& entry : it)
    {
        const std::string candidate = entry.path().filename().string();
        if (candidate == name)
            return name; // exact match on disk; nothing to correct
        if (EqualsIgnoreCase(candidate, name))
            return candidate;
    }
    return name;
}

} // namespace GameEngine::Editor
