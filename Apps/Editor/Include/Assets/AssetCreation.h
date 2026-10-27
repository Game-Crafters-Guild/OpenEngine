#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"

namespace GameEngine {
class AssetManager;
namespace ECS { class World; }
namespace Editor {
class UndoRedoService;
} // namespace Editor
} // namespace GameEngine

namespace GameEngine::Editor
{

// Generate a unique file path in `dir` by appending numeric suffixes to `baseStem`
// until no conflict is found. Returns empty path on failure (4096 attempts exhausted).
std::filesystem::path MakeUniqueFilePath(const std::filesystem::path& dir,
                                         const std::string& baseStem,
                                         const std::string& extensionWithDot);

// The path of `fileName` in `dir`, or of "Stem (1).ext", "Stem (2).ext" and on while that is taken, as a
// paste names a copy. A path in `reserved` counts as taken, so a batch names each file once. Empty when
// 9999 numbers are taken.
std::filesystem::path MakeNumberedFilePath(const std::filesystem::path& dir, const std::filesystem::path& fileName,
                                           std::span<const std::filesystem::path> reserved = {});

struct AssetFileResult
{
    std::filesystem::path path; // Empty on failure.
};

// Create a new asset file with undo support and asset registration.
// Creates parent directories if needed. Does NOT refresh any UI.
AssetFileResult CreateAssetFile(const std::filesystem::path& dir,
                                const std::string& baseName,
                                const std::string& extension,
                                const std::function<std::string(const std::string& resolvedStem)>& contentFn,
                                const std::string& undoLabel,
                                UndoRedoService* undo,
                                AssetManager* assets);

// Create a default PBR material file. Encapsulates MaterialDocument + JSON serialization
// so callers don't need <nlohmann/json.hpp> or MaterialAsset.h.
AssetFileResult CreateDefaultMaterialFile(const std::filesystem::path& dir,
                                          const std::string& baseName,
                                          UndoRedoService* undo,
                                          AssetManager* assets);

// Create a default navigation grid asset (.navgrid) with default GridSettings and empty cost/blocked data.
AssetFileResult CreateDefaultNavGridFile(const std::filesystem::path& dir,
                                         const std::string& baseName,
                                         UndoRedoService* undo,
                                         AssetManager* assets);

// Export .material files for a model's derived materials.
// Creates files in modelFilePath.parent_path()/Materials/, registers with the asset system,
// and updates MeshRenderer components on the provided entities to use file-backed GUIDs.
void ExportModelMaterials(const std::filesystem::path& modelFilePath,
                          const GUID& modelGuid,
                          const std::vector<ECS::EntityHandle>& entities,
                          ECS::World& world,
                          AssetManager& assets);

/// Permute on-disk .material files so physical slot i receives the content that was at slot
/// `displayOrderTopToBottom[i]` before the operation. `displayOrderTopToBottom` is the inspector
/// list order (top = index 0); it must be a permutation of 0..matCount-1. All slots must have
/// file-backed materials. Reloads affected materials and clears the material compiler cache.
bool ApplyModelMaterialSlotDisplayOrderToFiles(const std::filesystem::path& modelFilePath,
                                               const GUID& modelGuid,
                                               const std::vector<uint32_t>& displayOrderTopToBottom,
                                               AssetManager& assets);

} // namespace GameEngine::Editor
