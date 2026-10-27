#pragma once

#include "ECS/ECS.h"
#include "Editor/DragDropPayloads.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/Interaction/Payload.h"

#include <cstdint>
#include <vector>

namespace GameEngine::Editor
{

// Decode one tree id of a HierarchyEntityDragPayload. Mirrors HierarchyDataProvider::Encode:
// raw = index | (version << kEntityIndexBits). The single decode shared by every drop target that
// accepts hierarchy drags.
inline ECS::EntityHandle DecodeHierarchyTreeId(std::uint64_t treeId)
{
    const std::uint32_t raw = static_cast<std::uint32_t>(treeId);
    const ECS::EntityIndex idx = static_cast<ECS::EntityIndex>(raw & ECS::kEntityIndexMask);
    const ECS::EntityVersion ver =
        static_cast<ECS::EntityVersion>((raw >> ECS::kEntityIndexBits) & ECS::kEntityVersionMask);
    return ECS::EntityHandle(idx, ver);
}

// Decode the first entity of a HierarchyEntityDragPayload.
inline ECS::EntityHandle DecodeFirstHierarchyEntity(const UI::Interaction::DropRequest& request)
{
    const auto* payload = request.payload.TryGet<HierarchyEntityDragPayload>();
    if (!payload || payload->treeIds.empty())
        return ECS::EntityHandle();
    return DecodeHierarchyTreeId(payload->treeIds[0]);
}

// Decode every entity of a HierarchyEntityDragPayload, in drag order; empty for another payload.
inline std::vector<ECS::EntityHandle> DecodeHierarchyEntities(const UI::Interaction::DropRequest& request)
{
    std::vector<ECS::EntityHandle> entities;
    if (const auto* payload = request.payload.TryGet<HierarchyEntityDragPayload>())
    {
        entities.reserve(payload->treeIds.size());
        for (const std::uint64_t treeId : payload->treeIds)
            entities.push_back(DecodeHierarchyTreeId(treeId));
    }
    return entities;
}

} // namespace GameEngine::Editor
