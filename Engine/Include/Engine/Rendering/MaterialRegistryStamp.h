#pragma once

#include <cstdint>

namespace GameEngine::Engine::Renderer
{

/// Everything a table derived per material from the material registry depends
/// on: the index mapping (a material given or freed a GPU-scene row;
/// MaterialSystem::MaterialSSBOGeneration) and the process-wide content epoch
/// (any property, texture, awaited-slot, slot-map, UV-transform, sampler or
/// double-sidedness edit; Material::GetGlobalContentEpoch). Equal stamps mean a
/// sweep of the registry derives the same table, so a per-frame consumer
/// re-derives only when the stamp moves. Read it before the sweep: an edit
/// racing the sweep then moves the stamp past the recorded value.
struct MaterialRegistryStamp
{
    uint64_t IndexGeneration = 0;
    uint64_t ContentEpoch = 0;
    bool operator==(const MaterialRegistryStamp&) const = default;
};

}  // namespace GameEngine::Engine::Renderer
