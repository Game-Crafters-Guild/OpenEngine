#pragma once

#include "Types/Types.h"

#include <filesystem>
#include <memory>
#include <string>

namespace GameEngine::PageStreaming
{

class HeightPageLayer;

/// The step, in meters, of an authored height layer's samples and of its store at every level: 2^-14
/// (about 61 micrometers). A sculpt writes on this lattice (QuantizeHeight), so the store's integers
/// keep every sample bit for bit.
inline constexpr float32 kHeightLayerStep = 1.0f / 16384.0f;
/// The version of the authored height layer's rules (the step, the saved pages, the composition on
/// load), stored in the store header's Key. A layer store is in PageHeightUnits::LayerMeters, and the
/// store reader does not refuse it for another kPageStoreFormatVersion: a change of the caches'
/// version never refuses a saved layer. LoadHeightLayer refuses every other layer version.
inline constexpr uint64 kHeightLayerFormatVersion = 1u;

/// Writes the authored pages of `layer` (HeightPageLayer::ExportAuthoredPages) as a height store
/// (.gepage) at `target`, published by rename. Level 0 holds the layer's level-0 pages; a coarser
/// level holds the writes made at that level, not a filtered pyramid. Each sample is an integer in
/// kHeightLayerStep. A layer with no pages writes no file. Returns an empty string, else the reason.
std::string SaveHeightLayer(const HeightPageLayer& layer, const std::filesystem::path& target);

/// Reads the store at `path` (SaveHeightLayer) into `out`: a new layer of the store's faces and
/// pyramid with the coarser levels composed again. Returns an empty string, else the reason (`out`
/// is unchanged).
std::string LoadHeightLayer(const std::filesystem::path& path, std::shared_ptr<HeightPageLayer>& out);

} // namespace GameEngine::PageStreaming
