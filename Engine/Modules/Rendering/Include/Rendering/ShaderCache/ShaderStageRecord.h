#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Rendering
{

struct DescriptorSetLayoutDesc;
class SpecializationConstants;

/// The inputs one SPIR-V stage of a pipeline compiles with: the stage, the pipeline's descriptor set layouts,
/// the specialization constants and a hash of the bytes. Metal gives different code for each set of inputs,
/// so an export compiles each recorded set offline, with the bytes it finds in the shipped shaders.
struct ShaderStageRecord
{
    /// One descriptor binding, with the fields the translation reads.
    struct Binding
    {
        uint32_t Slot = 0;
        uint32_t Type = 0; // DescriptorType
        uint32_t Count = 1;
        uint32_t ShaderStages = 0;
        uint32_t Flags = 0;
    };

    /// One specialization constant: its id and its value bytes.
    struct Constant
    {
        uint32_t Id = 0;
        std::vector<uint8_t> Value;
    };

    /// The GLSL stage name: vert, frag, comp, mesh or task.
    std::string Stage;
    /// HashShaderStageBytes of the SPIR-V, and its size in bytes.
    uint64_t BytesHash = 0;
    uint32_t ByteCount = 0;
    /// One entry per set index; no value where the pipeline has no layout at that index.
    std::vector<std::optional<std::vector<Binding>>> SetLayouts;
    /// Sorted by id.
    std::vector<Constant> Constants;
};

/// The 64-bit FNV-1a hash of a stage's bytes, the key a record uses to find them again.
uint64_t HashShaderStageBytes(std::span<const uint8_t> bytes);

/// True when @p bytes start with the SPIR-V magic number. A backend's own source (MSL text) does not.
bool IsSpirvModule(std::span<const uint8_t> bytes);

/// The file a development run on @p workspaceRoot records its stage inputs to. An export of the
/// project at @p workspaceRoot reads the same file.
std::filesystem::path ShaderStageRecordFile(const std::filesystem::path& workspaceRoot);

/// The record of one SPIR-V stage: @p stage is the GLSL stage name, @p setLayouts the layouts of the
/// pipeline by set index and @p specialization null for none.
ShaderStageRecord MakeShaderStageRecord(std::span<const uint8_t> spirv, std::string_view stage,
                                        std::span<const DescriptorSetLayoutDesc> setLayouts,
                                        const SpecializationConstants* specialization);

/// One line of text for @p record, with no line break. The same inputs always give the same line.
std::string FormatShaderStageRecord(const ShaderStageRecord& record);

/// The record of a line that FormatShaderStageRecord wrote. False for any other text.
bool ParseShaderStageRecord(std::string_view line, ShaderStageRecord& out);

} // namespace GameEngine::Rendering
