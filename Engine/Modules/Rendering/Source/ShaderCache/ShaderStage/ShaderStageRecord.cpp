#include "Rendering/ShaderCache/ShaderStageRecord.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/SpecializationConstants.h"

#include "Types/Fnv1a.h"
#include "Types/ParseNumber.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace GameEngine::Rendering
{

namespace
{

constexpr std::string_view kStages[] = {"vert", "frag", "comp", "mesh", "task"};

bool IsKnownStage(std::string_view stage)
{
    for (std::string_view known : kStages)
    {
        if (stage == known)
            return true;
    }
    return false;
}

template <typename T>
bool ParseField(std::string_view text, T& out)
{
    const std::optional<uint64_t> value = ParseUnsigned(text);
    if (!value.has_value() || *value > std::numeric_limits<T>::max())
        return false;
    out = static_cast<T>(*value);
    return true;
}

// "set<N>=none", or "set<N>=<slot>.<type>.<count>.<stages>.<flags>,..." ("set<N>=" for an empty layout).
bool ParseSetLayout(std::string_view value, std::optional<std::vector<ShaderStageRecord::Binding>>& out)
{
    if (value == "none")
    {
        out.reset();
        return true;
    }
    out.emplace();
    if (value.empty())
        return true;
    for (std::string_view text : Split(value, ','))
    {
        const std::vector<std::string_view> fields = Split(text, '.');
        ShaderStageRecord::Binding binding;
        if (fields.size() != 5 || !ParseField(fields[0], binding.Slot) || !ParseField(fields[1], binding.Type) ||
            !ParseField(fields[2], binding.Count) || !ParseField(fields[3], binding.ShaderStages) ||
            !ParseField(fields[4], binding.Flags))
            return false;
        out->push_back(binding);
    }
    return true;
}

// "<id>:<byte>.<byte>...,..." in increasing id order.
bool ParseConstants(std::string_view value, std::vector<ShaderStageRecord::Constant>& out)
{
    if (value.empty())
        return true;
    for (std::string_view text : Split(value, ','))
    {
        const size_t colon = text.find(':');
        ShaderStageRecord::Constant constant;
        if (colon == std::string_view::npos || !ParseField(text.substr(0, colon), constant.Id))
            return false;
        for (std::string_view byteText : Split(text.substr(colon + 1), '.'))
        {
            if (!ParseField(byteText, constant.Value.emplace_back()))
                return false;
        }
        if (!out.empty() && out.back().Id >= constant.Id)
            return false;
        out.push_back(std::move(constant));
    }
    return true;
}

} // namespace

uint64_t HashShaderStageBytes(std::span<const uint8_t> bytes)
{
    return Hashing::Fnv1a64(bytes.data(), bytes.size());
}

bool IsSpirvModule(std::span<const uint8_t> bytes)
{
    constexpr uint32_t kSpirvMagic = 0x07230203u;
    uint32_t magic = 0;
    if (bytes.size() < sizeof(magic))
        return false;
    std::memcpy(&magic, bytes.data(), sizeof(magic));
    return magic == kSpirvMagic;
}

std::filesystem::path ShaderStageRecordFile(const std::filesystem::path& workspaceRoot)
{
    return workspaceRoot / ".Cache" / "Pipelines" / "ShaderStages.txt";
}

std::string FormatShaderStageRecord(const ShaderStageRecord& record)
{
    std::string line =
        "stage=" + record.Stage + " spirv=" + std::to_string(record.BytesHash) + ":" + std::to_string(record.ByteCount);
    for (size_t set = 0; set < record.SetLayouts.size(); ++set)
    {
        line += " set" + std::to_string(set) + "=";
        const auto& layout = record.SetLayouts[set];
        if (!layout.has_value())
        {
            line += "none";
            continue;
        }
        for (size_t i = 0; i < layout->size(); ++i)
        {
            const ShaderStageRecord::Binding& binding = (*layout)[i];
            line += (i == 0 ? "" : ",") + std::to_string(binding.Slot) + "." + std::to_string(binding.Type) + "." +
                    std::to_string(binding.Count) + "." + std::to_string(binding.ShaderStages) + "." +
                    std::to_string(binding.Flags);
        }
    }
    line += " spec=";
    for (size_t i = 0; i < record.Constants.size(); ++i)
    {
        line += (i == 0 ? "" : ",") + std::to_string(record.Constants[i].Id) + ":";
        for (size_t b = 0; b < record.Constants[i].Value.size(); ++b)
            line += (b == 0 ? "" : ".") + std::to_string(record.Constants[i].Value[b]);
    }
    return line;
}

bool ParseShaderStageRecord(std::string_view line, ShaderStageRecord& out)
{
    out = {};
    const std::vector<std::string_view> tokens = Split(line, ' ');
    if (tokens.size() < 3 || !tokens[0].starts_with("stage=") || !tokens[1].starts_with("spirv=") ||
        !tokens.back().starts_with("spec="))
        return false;
    out.Stage = std::string(tokens[0].substr(6));
    if (!IsKnownStage(out.Stage))
        return false;
    const std::string_view spirv = tokens[1].substr(6);
    const size_t colon = spirv.find(':');
    if (colon == std::string_view::npos || !ParseField(spirv.substr(0, colon), out.BytesHash) ||
        !ParseField(spirv.substr(colon + 1), out.ByteCount))
        return false;
    for (size_t i = 2; i + 1 < tokens.size(); ++i)
    {
        const std::string prefix = "set" + std::to_string(i - 2) + "=";
        if (!tokens[i].starts_with(prefix) ||
            !ParseSetLayout(tokens[i].substr(prefix.size()), out.SetLayouts.emplace_back()))
            return false;
    }
    return ParseConstants(tokens.back().substr(5), out.Constants);
}

ShaderStageRecord MakeShaderStageRecord(std::span<const uint8_t> spirv, std::string_view stage,
                                        std::span<const DescriptorSetLayoutDesc> setLayouts,
                                        const SpecializationConstants* specialization)
{
    ShaderStageRecord record;
    record.Stage = stage;
    record.BytesHash = HashShaderStageBytes(spirv);
    record.ByteCount = static_cast<uint32_t>(spirv.size());
    for (const DescriptorSetLayoutDesc& layout : setLayouts)
    {
        auto& bindings = record.SetLayouts.emplace_back().emplace();
        for (const DescriptorBinding& binding : layout.bindings)
            bindings.push_back({binding.binding, static_cast<uint32_t>(binding.type), binding.count,
                                binding.shaderStages, binding.flags});
    }
    if (specialization != nullptr)
    {
        for (const auto& [id, entry] : specialization->GetEntries())
        {
            const uint8_t* value = specialization->GetDataPtr() + entry.Offset;
            record.Constants.push_back({id, std::vector<uint8_t>(value, value + entry.Size)});
        }
        std::sort(record.Constants.begin(), record.Constants.end(),
                  [](const ShaderStageRecord::Constant& a, const ShaderStageRecord::Constant& b) { return a.Id < b.Id; });
    }
    return record;
}

} // namespace GameEngine::Rendering
