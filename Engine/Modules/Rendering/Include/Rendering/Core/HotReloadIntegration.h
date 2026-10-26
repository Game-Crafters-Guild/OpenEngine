#pragma once

#include <cstdint>
#include <vector>

namespace GameEngine {
    class AssetEventDispatcher; // fwd decl from AssetCore

namespace Rendering {

class IDevice;

// Call this when shader packages or modules are reloaded to invalidate dependent pipeline variants
void NotifyShadersHotReloaded(IDevice& device);

// Call this after any shader source file (.glsl, .hlsl, .vert, .frag, ...) is written,
// created, deleted or renamed. The compile path holds every source and every #include
// resolution it has read in memory and never re-reads a file until this runs; it empties
// that cache, so the next compile key and compile read the disk again. Thread-safe.
void NotifyShaderSourcesChanged();

// Tag-based selective invalidation helper
void InvalidatePipelineVariantsByTags(IDevice& device, const std::vector<uint64_t>& tags);

// Register an asset event callback that calls NotifyShadersHotReloaded on Shader/Material reloads
// Returns a dispatcher callback handle; store it and pass to UnregisterShaderHotReload on shutdown.
uint32_t RegisterShaderHotReload(IDevice& device, AssetEventDispatcher& dispatcher);
void UnregisterShaderHotReload(AssetEventDispatcher& dispatcher, uint32_t handle);

}} // namespace GameEngine::Rendering

