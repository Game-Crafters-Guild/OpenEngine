#pragma once

#include "Types/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class GUID;
} // namespace GameEngine

namespace GameEngine::Scene
{
struct SceneReadContext;
struct SceneValue;
struct SceneWriteContext;
} // namespace GameEngine::Scene

namespace GameEngine::AIECS
{
class AiRegistry;

/// The newest program whose AiProgram::StableId is `stableId`, or AiAgent::kNoProgram. A null id
/// finds no program.
uint16 FindProgramByStableId(const AiRegistry& registry, const GUID& stableId);

/// The scene text of a cell or a record subject: an entity reference when `isEntity`, else the number.
std::string FormatAiValue(const Scene::SceneWriteContext& ctx, uint32 value, bool isEntity);

/// The scene text "[a, b, ...]" of `values`; the values at `entityIndices` are entity references.
std::string FormatAiValues(const Scene::SceneWriteContext& ctx, std::span<const uint32> values,
                           std::span<const uint16> entityIndices);

/// Reads one item that FormatAiValue wrote. An entity reference gives the id of the loaded entity,
/// or the id of an invalid handle when the entity is not in the load.
bool ReadAiValue(const Scene::SceneReadContext& ctx, const Scene::SceneValue& item, uint32& out);

/// Reads the list that FormatAiValues wrote, with at most `maxCount` values.
bool ReadAiValues(const Scene::SceneReadContext& ctx, std::string_view text, size_t maxCount, std::vector<uint32>& out,
                  std::string* outError);

} // namespace GameEngine::AIECS
