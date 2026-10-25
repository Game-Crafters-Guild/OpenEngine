#pragma once

// Reading a reflected uniform name, private to the Rendering module: the material binding
// (Rendering/Materials/MaterialPropertyBinding.h) and the shader graph's property rows use them.

#include <string>
#include <string_view>

namespace GameEngine::Rendering::MaterialPropertyBinding
{

// A uniform name without its coding-style prefix, case kept: "uBaseColor" -> "BaseColor",
// "m_roughness" -> "roughness", "uUVScale" -> "UVScale".
std::string StripUniformPrefix(std::string_view uniformName);

// The material property key suggested for a uniform name: the name without its prefix, first
// letter lower case. "uBaseColor" -> "baseColor", "m_roughness" -> "roughness".
std::string SuggestPropertyKeyForUniform(std::string_view uniformName);

} // namespace GameEngine::Rendering::MaterialPropertyBinding
