#include "Rendering/ShaderCache/MetalLibraryBake.h"

#include "MetalShaderTranslator.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/SpecializationConstants.h"
#include "Rendering/ShaderCache/MetalLibraryArchive.h"
#include "Rendering/ShaderCache/ShaderPackageContainer.h"
#include "Rendering/ShaderCache/ShaderStageRecord.h"

#include "Types/FlatSet.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>

namespace GameEngine::Rendering
{

namespace
{

// The bytes of @p file, text or binary. Empty when it cannot be read.
std::string ReadFileBytes(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::optional<MetalShaderStage> StageFromName(std::string_view name)
{
    if (name == "vert")
        return MetalShaderStage::Vertex;
    if (name == "frag")
        return MetalShaderStage::Fragment;
    if (name == "comp")
        return MetalShaderStage::Compute;
    if (name == "mesh")
        return MetalShaderStage::Mesh;
    if (name == "task")
        return MetalShaderStage::Object;
    return std::nullopt;
}

using StageBytes = std::pair<uint64_t, std::vector<uint8_t>>;

// Adds @p bytes to @p found when their size is in @p sizes and their hash is in @p hashes (both sorted).
void AddRecordedBytes(std::span<const uint8_t> bytes, std::span<const uint64_t> hashes,
                      std::span<const uint32_t> sizes, std::vector<StageBytes>& found)
{
    if (!std::binary_search(sizes.begin(), sizes.end(), static_cast<uint32_t>(bytes.size())))
        return;
    const uint64_t hash = HashShaderStageBytes(bytes);
    if (std::binary_search(hashes.begin(), hashes.end(), hash))
        found.emplace_back(hash, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

// The SPIR-V under @p roots whose hash is in @p hashes, sorted by hash: the SPIR-V chunks of the shader
// packages and the loose .spv files. @p hashes and @p sizes are sorted; bytes of another size are not hashed.
std::vector<StageBytes> FindStageBytes(std::span<const std::filesystem::path> roots, std::span<const uint64_t> hashes,
                                       std::span<const uint32_t> sizes)
{
    std::vector<StageBytes> found;
    std::vector<ShaderPackageChunkView> chunks;
    for (const std::filesystem::path& root : roots)
    {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            const std::filesystem::path extension = it->path().extension();
            const bool looseStage = extension == ".spv";
            if (!it->is_regular_file(ec) || (!looseStage && extension != ".shaderpkg"))
                continue;
            if (looseStage &&
                !std::binary_search(sizes.begin(), sizes.end(), static_cast<uint32_t>(it->file_size(ec))))
                continue;
            const std::string bytes = ReadFileBytes(it->path());
            const std::span<const uint8_t> view(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
            if (looseStage)
            {
                AddRecordedBytes(view, hashes, sizes, found);
                continue;
            }
            chunks.clear();
            if (ReadShaderPackageContainer(view, chunks) != ShaderPackageReadResult::Read)
                continue;
            for (const ShaderPackageChunkView& chunk : chunks)
            {
                if (chunk.Name.ends_with(kShaderPackageSpirvSuffix))
                    AddRecordedBytes(chunk.Bytes, hashes, sizes, found);
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const StageBytes& a, const StageBytes& b) { return a.first < b.first; });
    return found;
}

template <typename T>
T ValueAs(const std::vector<uint8_t>& bytes)
{
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

// The constants of @p record as the pipeline gave them. False for a value size no constant type has.
bool MakeSpecialization(const ShaderStageRecord& record, SpecializationConstants& out)
{
    for (const ShaderStageRecord::Constant& constant : record.Constants)
    {
        switch (constant.Value.size())
        {
        case 1: out.AddConstant(constant.Id, ValueAs<uint8_t>(constant.Value)); break;
        case 2: out.AddConstant(constant.Id, ValueAs<uint16_t>(constant.Value)); break;
        case 4: out.AddConstant(constant.Id, ValueAs<uint32_t>(constant.Value)); break;
        case 8: out.AddConstant(constant.Id, ValueAs<uint64_t>(constant.Value)); break;
        default: return false;
        }
    }
    return true;
}

std::vector<DescriptorSetLayoutDesc> MakeLayouts(const ShaderStageRecord& record)
{
    std::vector<DescriptorSetLayoutDesc> layouts(record.SetLayouts.size());
    for (size_t set = 0; set < layouts.size(); ++set)
    {
        if (!record.SetLayouts[set].has_value())
            continue;
        for (const ShaderStageRecord::Binding& recorded : *record.SetLayouts[set])
        {
            DescriptorBinding binding{};
            binding.binding = recorded.Slot;
            binding.type = static_cast<DescriptorType>(recorded.Type);
            binding.count = recorded.Count;
            binding.shaderStages = static_cast<decltype(binding.shaderStages)>(recorded.ShaderStages);
            binding.flags = static_cast<decltype(binding.flags)>(recorded.Flags);
            layouts[set].bindings.push_back(binding);
        }
    }
    return layouts;
}

} // namespace

MetalLibraryBake BakeMetalLibraries(const std::filesystem::path& recordFile,
                                    std::span<const std::filesystem::path> shaderRoots,
                                    const std::filesystem::path& workDir, const MetalLibraryCompiler& compile)
{
    MetalLibraryBake bake;
    const std::string text = ReadFileBytes(recordFile);
    std::vector<ShaderStageRecord> records;
    ShaderStageRecord parsed;
    for (std::string_view line : Split(text, '\n'))
    {
        if (ParseShaderStageRecord(line, parsed))
            records.push_back(parsed);
    }
    bake.RecordedStages = static_cast<uint32_t>(records.size());

    std::vector<uint64_t> hashes;
    std::vector<uint32_t> sizes;
    for (const ShaderStageRecord& record : records)
    {
        hashes.push_back(record.BytesHash);
        sizes.push_back(record.ByteCount);
    }
    std::sort(hashes.begin(), hashes.end());
    std::sort(sizes.begin(), sizes.end());
    const std::vector<StageBytes> stageBytes = FindStageBytes(shaderRoots, hashes, sizes);

    std::error_code ec;
    std::filesystem::create_directories(workDir, ec);
    std::vector<MetalLibraryCompileJob> jobs;
    std::vector<MetalLibraryArchiveEntry> entries;
    FlatSet<uint64_t> keys;
    for (const ShaderStageRecord& record : records)
    {
        const std::optional<MetalShaderStage> stage = StageFromName(record.Stage);
        const auto bytes = std::lower_bound(stageBytes.begin(), stageBytes.end(), record.BytesHash,
                                            [](const StageBytes& a, uint64_t hash) { return a.first < hash; });
        SpecializationConstants constants;
        if (!stage || bytes == stageBytes.end() || bytes->first != record.BytesHash ||
            bytes->second.size() != record.ByteCount || !MakeSpecialization(record, constants))
            continue;
        ++bake.ResolvedStages;
        const std::vector<DescriptorSetLayoutDesc> layouts = MakeLayouts(record);
        std::vector<const DescriptorSetLayoutDesc*> setLayouts;
        for (size_t set = 0; set < layouts.size(); ++set)
            setLayouts.push_back(record.SetLayouts[set].has_value() ? &layouts[set] : nullptr);
        const SpecializationConstants* specialization = record.Constants.empty() ? nullptr : &constants;
        const uint64_t key = ComputeTranslationHash(bytes->second, *stage, setLayouts, specialization);
        if (!keys.Insert(key))
            continue;
        const MetalShaderTranslation translation = TranslateSpirvToMsl(bytes->second, *stage, setLayouts, specialization);
        if (!translation.Success)
            continue;
        const std::string name = std::format("{:016x}", key);
        MetalLibraryCompileJob& job = jobs.emplace_back(workDir / (name + ".metal"), workDir / (name + ".metallib"));
        std::ofstream(job.Source, std::ios::binary) << translation.Msl;
        std::filesystem::remove(job.Library, ec);
        entries.push_back({key, translation.LocalSizeX, translation.LocalSizeY, translation.LocalSizeZ, {}});
    }
    if (jobs.empty())
        return bake;
    compile(jobs, MetalLibraryCompileFlags(kShippedFastMath));

    std::vector<std::string> libraries(jobs.size());
    std::vector<MetalLibraryArchiveEntry> compiled;
    for (size_t i = 0; i < jobs.size(); ++i)
    {
        libraries[i] = ReadFileBytes(jobs[i].Library);
        if (libraries[i].empty())
            continue;
        entries[i].Library = std::span(reinterpret_cast<const uint8_t*>(libraries[i].data()), libraries[i].size());
        compiled.push_back(entries[i]);
    }
    bake.CompiledStages = static_cast<uint32_t>(compiled.size());
    if (!compiled.empty())
        bake.Archive = WriteMetalLibraryArchive(ComputeMetalLibraryOptionsHash(kShippedFastMath), compiled);
    return bake;
}

} // namespace GameEngine::Rendering
