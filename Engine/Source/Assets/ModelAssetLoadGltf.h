#pragma once

// How the glTF loader opens a file and charges its animations' keys, shared by
// ModelAsset::LoadGLTF and the glTF clip path in AnimationClip: a document
// either passes every check below or is refused before any of its accessors is
// read.

#include "Types/Types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

struct cgltf_data;

namespace GameEngine {

using GltfExternalBufferStorage = std::unordered_map<std::filesystem::path, Vector<uint8>>;

/// Frees the parsed document, then the external file bytes its buffers share.
struct GltfDocumentRelease
{
    std::unique_ptr<GltfExternalBufferStorage> ExternalBuffers;
    void operator()(cgltf_data* document);
};

/// The deepest node hierarchy a glTF file may hold, in levels from a scene root (the root is level 1).
inline constexpr size_t kMaxGltfNodeDepth = 4096;

/// The most vertices MergeStaticPrimitivesByMaterial merges into one mesh: its indices are uint32. On a
/// 32-bit target (wasm32) the cap is size_t's maximum, which no mesh there can reach.
inline constexpr size_t kMaxMergedGltfVertices =
    static_cast<size_t>(std::min<uint64_t>(uint64_t{1} << 32, std::numeric_limits<size_t>::max()));

/// A parsed glTF document whose buffers are loaded; empty when the file was refused.
using GltfDocument = std::unique_ptr<cgltf_data, GltfDocumentRelease>;

/**
 * @brief Opens a .gltf or .glb file for reading.
 *
 * Parses `bytes`, refuses the file when its extensionsRequired lists an extension
 * the engine does not implement or a base64 data URI buffer declares a byteLength
 * past what its payload decodes to (before cgltf would allocate it), loads its
 * buffers (external ones resolve beside `path`, and one shorter than its declared
 * byteLength is refused), refuses a buffer view whose byteStride is outside glTF's
 * 4 to 252 or that runs past its buffer, an accessor whose extent is larger than
 * a size_t holds, and an accessor without a buffer view that declares more
 * elements than the HLOD cache allows a
 * submesh (2^26, or 2^27 for one read only as primitive indices), refuses a node
 * more than kMaxGltfNodeDepth levels below its scene root, runs cgltf_validate,
 * which bounds every buffer view, every accessor that has one and
 * every sparse block by the loaded bytes, every index a buffer view holds by its
 * primitive's vertex count, and the output of every animation channel that
 * targets a node by its input's key count, and refuses a skin with fewer inverse
 * bind matrices than joints. A refused file logs one error that names `path` and
 * the reason.
 *
 * @return The document, or an empty one when the file is refused.
 */
GltfDocument OpenGltfDocument(const Vector<uint8>& bytes, const std::filesystem::path& path);

struct Mesh;

/**
 * @brief Merges the static triangle meshes of a glTF model that share a material into one mesh per
 *        material, baking each source node's transform into the vertices, when the model has at
 *        least 32 such meshes and more than twice as many of them as materials.
 *
 * Skinned, morphed, non-triangle, multiply placed and LOD-chained meshes stay as they are. A
 * material's group whose merged vertex count would pass `maxMergedVertices` (the loader passes
 * kMaxMergedGltfVertices) stays unmerged, with a warning that names `modelName` and the material.
 */
void MergeStaticPrimitivesByMaterial(Vector<Mesh>& meshes, std::string_view modelName, size_t maxMergedVertices);

class GltfAllocationBudget;

/**
 * @brief Charges `budget` with the keys the glTF clip path sizes for animation `animationIndex` of
 *        `document`: one Animation::AnimKeyframe per element of the input of each channel with a
 *        sampler, an input, an output and a target node (a channel the clip path skips for its path
 *        is charged too).
 *
 * @return Empty when every channel fits; otherwise the first channel that does not, described for a
 *         refusal: "channel 3 of animation 0 ('Walk') has a key time accessor " and the budget's reason.
 */
std::string ChargeAnimationKeys(const cgltf_data& document, size_t animationIndex, GltfAllocationBudget& budget);

} // namespace GameEngine
