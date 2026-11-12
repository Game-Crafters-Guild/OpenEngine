#include "Scene/AiSceneText.h"

#include "AIECS/AiRegistry.h"
#include "AIECS/Components/AiAgent.h"
#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneValue.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace GameEngine::AIECS
{

uint16 FindProgramByStableId(const AiRegistry& registry, const GUID& stableId)
{
    uint16 found = Components::AiAgent::kNoProgram;
    if (stableId.IsNull())
        return found;
    for (uint16 id = 0; const AiProgram* program = registry.FindProgram(id); ++id)
    {
        if (program->StableId == stableId)
            found = id;
    }
    return found;
}

std::string FormatAiValue(const Scene::SceneWriteContext& ctx, uint32 value, bool isEntity)
{
    if (!isEntity)
        return std::to_string(value);
    std::string reference = Scene::FormatEntityReferenceForSave(ctx, ECS::EntityHandle(value));
    return reference.empty() ? Scene::FormatQuoted({}) : reference;
}

std::string FormatAiValues(const Scene::SceneWriteContext& ctx, std::span<const uint32> values,
                           std::span<const uint16> entityIndices)
{
    std::vector<std::string> items;
    items.reserve(values.size());
    for (size_t index = 0; index < values.size(); ++index)
    {
        const bool isEntity = std::find(entityIndices.begin(), entityIndices.end(), index) != entityIndices.end();
        items.push_back(FormatAiValue(ctx, values[index], isEntity));
    }
    return "[" + Join(items, ", ") + "]";
}

bool ReadAiValue(const Scene::SceneReadContext& ctx, const Scene::SceneValue& item, uint32& out)
{
    if (item.Kind == Scene::SceneValueKind::Int)
    {
        if (item.IntValue < 0 || item.IntValue > std::numeric_limits<uint32>::max())
            return false;
        out = static_cast<uint32>(item.IntValue);
        return true;
    }
    if (item.Kind != Scene::SceneValueKind::String)
        return false;
    ECS::EntityHandle entity;
    if (!Scene::TryResolveEntityReference(ctx, Scene::FormatQuoted(item.StringValue), entity))
        return false;
    out = entity.id;
    return true;
}

bool ReadAiValues(const Scene::SceneReadContext& ctx, std::string_view text, size_t maxCount, std::vector<uint32>& out,
                  std::string* outError)
{
    Scene::SceneValue list;
    if (!Scene::ParseValue(text, list, outError))
        return false;
    if (list.Kind != Scene::SceneValueKind::Array || list.Items.size() > maxCount)
    {
        if (outError)
            *outError = "expected a list of at most " + std::to_string(maxCount) +
                        " values, each a whole number or a quoted entity id";
        return false;
    }
    out.resize(list.Items.size());
    for (size_t index = 0; index < list.Items.size(); ++index)
    {
        if (!ReadAiValue(ctx, list.Items[index], out[index]))
        {
            if (outError)
                *outError = "item " + std::to_string(index) + " is not a whole number from 0 to 4294967295 or a quoted entity id";
            return false;
        }
    }
    return true;
}

} // namespace GameEngine::AIECS
