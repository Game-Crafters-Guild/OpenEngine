#pragma once

#if RENDERING_HAS_SHADERC

#include <gtest/gtest.h>
#include <shaderc/shaderc.hpp>

#include <cstdint>
#include <vector>

namespace GameEngine::Rendering::Tests
{

/// Compiles test GLSL to SPIR-V for Vulkan 1.3. A compile error fails the current test and
/// returns an empty vector.
inline std::vector<uint8_t> CompileGlsl(const char* source, shaderc_shader_kind kind, const char* name)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    const auto result = compiler.CompileGlslToSpv(source, kind, name, options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
    {
        ADD_FAILURE() << "GLSL compile failed: " << result.GetErrorMessage();
        return {};
    }
    const auto* begin = reinterpret_cast<const uint8_t*>(result.cbegin());
    const auto* end = reinterpret_cast<const uint8_t*>(result.cend());
    return std::vector<uint8_t>(begin, end);
}

} // namespace GameEngine::Rendering::Tests

#endif
