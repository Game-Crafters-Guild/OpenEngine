#pragma once

#include "Rendering/Core/Device.h"

#include <cstdint>
#include <string>
#include <vector>

namespace MTL
{
class CompileOptions;
}

namespace GameEngine::Rendering
{

class SpecializationConstants;
struct MetalWrittenBinding;

// SPIR-V -> MSL translation via SPIRV-Cross. Descriptor sets become Metal 3
// argument buffers at [[buffer(set)]]; per-binding [[id(n)]] assignment is
// forced to match ComputeMetalArgumentBufferLayout (see
// MetalArgumentBufferLayout.h). Push constants land at
// [[buffer(kMetalPushConstantBufferIndex)]].
struct MetalShaderTranslation
{
    bool Success = false;
    std::string Msl;
    std::string EntryPoint; // SPIRV-Cross renames the entry to "main0"
    std::string Error;
    // Compute and Mesh stages: SPIR-V workgroup size, needed at dispatch /
    // drawMeshThreadgroups time.
    uint32_t LocalSizeX = 0;
    uint32_t LocalSizeY = 0;
    uint32_t LocalSizeZ = 0;
};

enum class MetalShaderStage
{
    Vertex,
    Fragment,
    Compute,
    // GL_EXT_mesh_shader stages. Object is the optional amplification/task
    // stage that dispatches mesh threadgroups.
    Mesh,
    Object
};

MetalShaderTranslation TranslateSpirvToMsl(const std::vector<uint8_t>& spirv,
                                           MetalShaderStage stage,
                                           const std::vector<const DescriptorSetLayoutDesc*>& setLayouts,
                                           const SpecializationConstants* specialization);

// The translation key: a hash of every input TranslateSpirvToMsl reads and of the translator
// version (kMslCacheVersion). It keys the MSL disk cache and the shipped Metal libraries.
uint64_t ComputeTranslationHash(const std::vector<uint8_t>& spirv, MetalShaderStage stage,
                                const std::vector<const DescriptorSetLayoutDesc*>& setLayouts,
                                const SpecializationConstants* specialization);

// The MSL language version of the device's compile and of an export's offline compile. One version at
// both ends gives one result for a stage that ships in one pipeline and compiles in another.
constexpr uint32_t kMetalLanguageMajor = 3;
constexpr uint32_t kMetalLanguageMinor = 2;
// A shipped game compiles MSL with fast math; GE_METAL_PRECISE_MATH=1 turns it off for a debugging run.
constexpr bool kShippedFastMath = true;

// A hash of the translator version, the SPIR-V to MSL options and the MSL compile flags. A shipped
// library built under another value is stale and is not used.
uint64_t ComputeMetalLibraryOptionsHash(bool fastMath);

// The `metal` flags of a shipped stage library: the options of MakeMetalCompileOptions as flags.
std::vector<std::string> MetalLibraryCompileFlags(bool fastMath);

// The options the device compiles MSL with. The caller releases them.
MTL::CompileOptions* MakeMetalCompileOptions();

// The fast-math setting of the device: kShippedFastMath, unless GE_METAL_PRECISE_MATH=1.
bool MetalFastMathEnabled();

#ifndef NDEBUG
// Adds each storage buffer and storage image that the SPIR-V module uses and does not decorate NonWritable.
// The list stays unchanged when the module does not parse.
void AppendWrittenStorageBindings(const std::vector<uint8_t>& spirv, std::vector<MetalWrittenBinding>& written);
#endif

} // namespace GameEngine::Rendering
