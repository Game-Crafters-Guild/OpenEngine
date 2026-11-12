#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::Rendering
{

/// One stage for the Metal compiler: the MSL file to compile and the .metallib to write.
struct MetalLibraryCompileJob
{
    std::filesystem::path Source;
    std::filesystem::path Library;
};

/// Runs the Metal compiler with @p flags (the `metal` flags that give the device's compile) for each
/// job. A job whose library file is missing afterwards failed.
using MetalLibraryCompiler =
    std::function<void(std::span<const MetalLibraryCompileJob> jobs, std::span<const std::string> flags)>;

/// What a bake did: the stages the record lists, the stages whose SPIR-V the bake found, and the
/// stages the archive holds.
struct MetalLibraryBake
{
    uint32_t RecordedStages = 0;
    uint32_t ResolvedStages = 0;
    uint32_t CompiledStages = 0;
    std::vector<uint8_t> Archive;
};

/// Compiles offline each stage of @p recordFile whose SPIR-V is in a package or .spv file under @p shaderRoots:
/// the runtime translator writes MSL under @p workDir, and @p compile makes each .metallib. The archive holds
/// every library that compiled, and is empty when none did. Defined on Apple platforms only.
MetalLibraryBake BakeMetalLibraries(const std::filesystem::path& recordFile,
                                    std::span<const std::filesystem::path> shaderRoots,
                                    const std::filesystem::path& workDir, const MetalLibraryCompiler& compile);

} // namespace GameEngine::Rendering
