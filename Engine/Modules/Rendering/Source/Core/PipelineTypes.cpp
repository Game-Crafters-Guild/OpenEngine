#include "Rendering/Core/PipelineTypes.h"

#include "Types/Fnv1a.h"

#include <cstring>
#include <span>

namespace GameEngine::Rendering
{

// FNV-1a 64-bit offset basis as the hash seed across this file.
static constexpr uint64_t kHashSeed = 0xcbf29ce484222325ULL;

uint64_t PipelineFormatKey::Hash() const noexcept
{
    using Hashing::HashCombine;
    uint64_t h = kHashSeed;
    h = HashCombine(h, static_cast<uint64_t>(ColorCount));
    for (uint8_t i = 0; i < ColorCount && i < kMaxColors; ++i)
    {
        h = HashCombine(h, static_cast<uint64_t>(ColorFormats[i]));
    }
    h = HashCombine(h, static_cast<uint64_t>(DepthFormat));
    h = HashCombine(h, static_cast<uint64_t>(StencilFormat));
    h = HashCombine(h, static_cast<uint64_t>(RasterizationSamples));
    return h;
}

// =====================================================================
// DescriptorSetLayoutDesc content hash
// =====================================================================

uint64_t HashDescriptorSetLayoutDesc(const DescriptorSetLayoutDesc& desc) noexcept
{
    using Hashing::HashCombine;
    using Hashing::Fnv1a64Value;
    uint64_t h = kHashSeed;
    h = Fnv1a64Value(h, desc.bindings.size());
    for (const auto& b : desc.bindings)
    {
        h = Fnv1a64Value(h, b.binding);
        h = Fnv1a64Value(h, b.type);
        h = Fnv1a64Value(h, b.count);
        h = Fnv1a64Value(h, b.shaderStages);
        h = Fnv1a64Value(h, b.flags);
        h = Fnv1a64Value(h, b.imageDim);
        h = Fnv1a64Value(h, b.imageArrayed);
        h = Fnv1a64Value(h, b.imageIsDepth);
        h = Fnv1a64Value(h, b.imageIsUnsignedInteger);
        h = Fnv1a64Value(h, b.storageTexelFormat);
        // WebGPU bakes these into the bind group layout, so two descs that
        // differ only here must intern to two layouts.
        h = Fnv1a64Value(h, b.storageReadOnly);
        h = Fnv1a64Value(h, b.imageFilterableFloat);
        h = Fnv1a64Value(h, b.imageUsageReflected);
        h = Fnv1a64Value(h, b.imageMultisample);
        // debugName intentionally NOT hashed
    }
    return h;
}

// =====================================================================
// Helpers for desc hashing (used by GraphicsPipelineDesc / ComputePipelineDesc)
// =====================================================================

namespace
{

// Hash a `shared_ptr<const vector<uint8_t>>` shader-byte ingredient.
// Null pointer hashes the same as an empty vector — semantically equivalent.
uint64_t HashShader(uint64_t h, const std::shared_ptr<const std::vector<uint8_t>>& bytes) noexcept
{
    using Hashing::Fnv1a64Values;
    if (!bytes)
        return Fnv1a64Values(h, std::span<const uint8_t>{});
    return Fnv1a64Values(h, *bytes);
}

uint64_t HashRasterizationState(uint64_t h, const RasterizationState& s) noexcept
{
    using Hashing::Fnv1a64Value;
    h = Fnv1a64Value(h, s.depthClampEnable);
    h = Fnv1a64Value(h, s.rasterizerDiscardEnable);
    h = Fnv1a64Value(h, s.polygonMode);
    h = Fnv1a64Value(h, s.cullMode);
    h = Fnv1a64Value(h, s.frontFace);
    h = Fnv1a64Value(h, s.depthBiasEnable);
    h = Fnv1a64Value(h, s.depthBiasConstantFactor);
    h = Fnv1a64Value(h, s.depthBiasClamp);
    h = Fnv1a64Value(h, s.depthBiasSlopeFactor);
    h = Fnv1a64Value(h, s.lineWidth);
    return h;
}

uint64_t HashDepthStencilState(uint64_t h, const DepthStencilState& s) noexcept
{
    using Hashing::Fnv1a64Value;
    h = Fnv1a64Value(h, s.depthTestEnable);
    h = Fnv1a64Value(h, s.depthWriteEnable);
    h = Fnv1a64Value(h, s.depthCompareOp);
    h = Fnv1a64Value(h, s.depthBoundsTestEnable);
    h = Fnv1a64Value(h, s.stencilTestEnable);
    h = Fnv1a64Value(h, s.minDepthBounds);
    h = Fnv1a64Value(h, s.maxDepthBounds);
    return h;
}

uint64_t HashColorBlendAttachment(uint64_t h, const ColorBlendAttachmentState& s) noexcept
{
    using Hashing::Fnv1a64Value;
    h = Fnv1a64Value(h, s.blendEnable);
    h = Fnv1a64Value(h, s.srcColorBlendFactor);
    h = Fnv1a64Value(h, s.dstColorBlendFactor);
    h = Fnv1a64Value(h, s.colorBlendOp);
    h = Fnv1a64Value(h, s.srcAlphaBlendFactor);
    h = Fnv1a64Value(h, s.dstAlphaBlendFactor);
    h = Fnv1a64Value(h, s.alphaBlendOp);
    h = Fnv1a64Value(h, s.colorWriteMask);
    return h;
}

uint64_t HashColorBlendState(uint64_t h, const ColorBlendState& s) noexcept
{
    using Hashing::Fnv1a64Value;
    h = Fnv1a64Value(h, s.logicOpEnable);
    h = Fnv1a64Value(h, s.alphaToCoverageEnable);
    h = Fnv1a64Value(h, s.attachments.size());
    for (const auto& a : s.attachments)
        h = HashColorBlendAttachment(h, a);
    h = Fnv1a64Value(h, s.blendConstants[0]);
    h = Fnv1a64Value(h, s.blendConstants[1]);
    h = Fnv1a64Value(h, s.blendConstants[2]);
    h = Fnv1a64Value(h, s.blendConstants[3]);
    return h;
}

uint64_t HashPushConstants(uint64_t h, const PushConstantRange& r) noexcept
{
    using Hashing::Fnv1a64Value;
    h = Fnv1a64Value(h, r.Offset);
    h = Fnv1a64Value(h, r.Size);
    h = Fnv1a64Value(h, r.StageMask);
    return h;
}

uint64_t HashNamedPushConstantRanges(uint64_t h, const std::vector<NamedPushConstantRange>& v) noexcept
{
    using Hashing::Fnv1a64Value;
    h = Fnv1a64Value(h, v.size());
    for (const auto& r : v)
    {
        // Name intentionally NOT hashed (diagnostic only).
        h = Fnv1a64Value(h, r.Offset);
        h = Fnv1a64Value(h, r.Size);
        h = Fnv1a64Value(h, r.StageMask);
    }
    return h;
}

uint64_t HashVertexBindings(uint64_t h, const std::vector<VertexInputBinding>& v) noexcept
{
    using Hashing::Fnv1a64Value;
    h = Fnv1a64Value(h, v.size());
    for (const auto& b : v)
    {
        h = Fnv1a64Value(h, b.binding);
        h = Fnv1a64Value(h, b.stride);
        h = Fnv1a64Value(h, b.inputRate);
    }
    return h;
}

uint64_t HashVertexAttributes(uint64_t h, const std::vector<VertexInputAttribute>& v) noexcept
{
    using Hashing::Fnv1a64Value;
    h = Fnv1a64Value(h, v.size());
    for (const auto& a : v)
    {
        h = Fnv1a64Value(h, a.location);
        h = Fnv1a64Value(h, a.binding);
        h = Fnv1a64Value(h, a.format);
        h = Fnv1a64Value(h, a.offset);
    }
    return h;
}

uint64_t HashDescriptorSetLayoutIds(uint64_t h, const std::vector<DescriptorSetLayoutId>& v) noexcept
{
    using Hashing::Fnv1a64Value;
    h = Fnv1a64Value(h, v.size());
    for (const auto& id : v)
        h = Fnv1a64Value(h, id.Value);
    return h;
}

uint64_t HashDynamicState(uint64_t h, const std::optional<DynamicStateInfo>& s) noexcept
{
    using Hashing::Fnv1a64Value;
    h = Fnv1a64Value(h, s.has_value());
    if (s.has_value())
    {
        h = Fnv1a64Value(h, s->states.size());
        for (auto state : s->states)
            h = Fnv1a64Value(h, state);
    }
    return h;
}

uint64_t HashSpecializationConstants(uint64_t h, const std::optional<SpecializationConstants>& s) noexcept
{
    using Hashing::HashCombine;
    using Hashing::Fnv1a64Value;
    h = Fnv1a64Value(h, s.has_value());
    if (s.has_value())
    {
        // Hash the actual content (constant ids + byte values, sorted by id)
        // so two callers that build the same specialization constants intern
        // to the same pipeline id regardless of insertion order. Previously
        // this used pointer identity, which fragmented the cache per call
        // site once specialization constants were adopted.
        h = HashCombine(h, s.value().GetHash());
    }
    return h;
}

} // anonymous namespace

// =====================================================================
// GraphicsPipelineDesc::ContentHash
// =====================================================================

uint64_t GraphicsPipelineDesc::ContentHash() const noexcept
{
    using Hashing::Fnv1a64Value;
    uint64_t h = kHashSeed;
    h = Fnv1a64Value(h, Kind);
    h = HashShader(h, VertexShader);
    h = HashShader(h, PixelShader);
    h = HashShader(h, MeshShader);
    h = HashShader(h, AmplificationShader);
    h = HashDescriptorSetLayoutIds(h, DescriptorSetLayouts);
    h = HashRasterizationState(h, Rasterization);
    h = HashDepthStencilState(h, DepthStencil);
    h = HashColorBlendState(h, ColorBlend);
    h = HashDynamicState(h, DynamicState);
    h = HashPushConstants(h, PushConstants);
    h = HashNamedPushConstantRanges(h, NamedPushConstantRanges);
    h = HashVertexBindings(h, VertexBindings);
    h = HashVertexAttributes(h, VertexAttributes);
    h = Fnv1a64Value(h, Topology);
    h = HashSpecializationConstants(h, Specialization);
    // DebugName intentionally NOT hashed.
    return h;
}

// =====================================================================
// ComputePipelineDesc::ContentHash
// =====================================================================

uint64_t ComputePipelineDesc::ContentHash() const noexcept
{
    using Hashing::Fnv1a64Value;
    uint64_t h = kHashSeed;
    h = HashShader(h, ComputeShader);
    h = HashDescriptorSetLayoutIds(h, DescriptorSetLayouts);
    h = HashPushConstants(h, PushConstants);
    h = HashNamedPushConstantRanges(h, NamedPushConstantRanges);
    h = HashSpecializationConstants(h, Specialization);
    // DebugName intentionally NOT hashed.
    return h;
}

bool PipelineFormatKey::operator==(const PipelineFormatKey& other) const noexcept
{
    if (ColorCount != other.ColorCount) return false;
    if (DepthFormat != other.DepthFormat) return false;
    if (StencilFormat != other.StencilFormat) return false;
    if (RasterizationSamples != other.RasterizationSamples) return false;
    // Only compare in-use slots — slots beyond ColorCount may be zero-padded
    // or stale and must not affect equality.
    for (uint8_t i = 0; i < ColorCount; ++i)
    {
        if (ColorFormats[i] != other.ColorFormats[i]) return false;
    }
    return true;
}

} // namespace GameEngine::Rendering
