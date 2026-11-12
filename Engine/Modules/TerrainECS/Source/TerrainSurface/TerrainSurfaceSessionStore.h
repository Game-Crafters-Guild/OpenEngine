#pragma once

#include <filesystem>

namespace GameEngine
{
class GUID;
}

namespace GameEngine::TerrainECS
{

/// The folder of every driver session in the cache `cacheDirectory`: <cacheDirectory>/Session.
std::filesystem::path SessionStoreRoot(const std::filesystem::path& cacheDirectory);

/// The surface stores of session `session` (the terrains whose scene is not saved): a folder of
/// SessionStoreRoot, named for the session's GUID. A lock file beside it is held while the session lives.
std::filesystem::path SessionStoreDirectory(const std::filesystem::path& cacheDirectory, const GUID& session);
std::filesystem::path SessionStoreLock(const std::filesystem::path& cacheDirectory, const GUID& session);

/// Deletes the folder of every session in `cacheDirectory` whose lock no process holds, and its lock
/// file: the stores of an editor that stopped without deleting them.
void RemoveOrphanSessionStores(const std::filesystem::path& cacheDirectory);

} // namespace GameEngine::TerrainECS
