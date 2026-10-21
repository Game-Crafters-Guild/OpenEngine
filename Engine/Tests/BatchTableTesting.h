#pragma once

// Batch tables as vectors for assertions. The builders write into caller storage (a ScratchScope
// in production); these give a test rows it can hold, compare and index past the call.

#include "Rendering/Core/BatchRegistry.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"

#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace GameEngine::Rendering::BatchTableTesting
{

inline std::vector<GPUDrawStreamBuilder::BatchTableEntry> Build(const BatchRegistry& registry,
                                                                std::span<const uint32_t> meshPoolGroup,
                                                                uint32_t* outTotalRecords = nullptr)
{
    std::vector<GPUDrawStreamBuilder::BatchTableEntry> table(GPUDrawStreamBuilder::BatchTableCapacity(registry));
    table.resize(GPUDrawStreamBuilder::BuildBatchTable(registry, meshPoolGroup, table,
                                                       *std::pmr::new_delete_resource(), outTotalRecords)
                     .size());
    return table;
}

inline std::vector<GPUDrawStreamBuilder::BatchTableEntry> BuildShadow(const BatchRegistry& registry,
                                                                      std::span<const uint8_t> materialDepthClass,
                                                                      std::span<const uint32_t> meshPoolGroup,
                                                                      uint32_t* outTotalRecords = nullptr)
{
    std::vector<GPUDrawStreamBuilder::BatchTableEntry> table(GPUDrawStreamBuilder::BatchTableCapacity(registry));
    table.resize(GPUDrawStreamBuilder::BuildShadowBatchTable(registry, materialDepthClass, meshPoolGroup, table,
                                                             *std::pmr::new_delete_resource(), outTotalRecords)
                     .size());
    return table;
}

inline std::vector<GPUDrawStreamBuilder::BatchTableEntry> BuildColor(const BatchRegistry& registry,
                                                                     std::span<const uint32_t> materialColorClass,
                                                                     std::span<const uint32_t> meshPoolGroup,
                                                                     uint32_t* outTotalRecords = nullptr)
{
    std::vector<GPUDrawStreamBuilder::BatchTableEntry> table(GPUDrawStreamBuilder::BatchTableCapacity(registry));
    table.resize(GPUDrawStreamBuilder::BuildColorBatchTable(registry, materialColorClass, meshPoolGroup, table,
                                                            *std::pmr::new_delete_resource(), outTotalRecords)
                     .size());
    return table;
}

} // namespace GameEngine::Rendering::BatchTableTesting
