#pragma once

// The one parse of a program's declared properties: registration and composition resolve
// through it, so the CPU lanes and the composed GLSL agree. An entry lives until the next
// announced shader source edit (NotifyShaderSourcesChanged).

#include "Rendering/Materials/Shader/ShaderPropertyTable.h"
#include "Types/Types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace GameEngine::Rendering
{

struct ShaderPropertyTableInputs
{
    std::filesystem::path AdapterPath;        // adapter_forward.glsl (declares the adapter's own reads)
    std::filesystem::path SurfacePath;        // resolved surface file
    std::filesystem::path VertexModifierPath; // resolved vertex modifier, or empty
};

class ShaderPropertyTableCache
{
  public:
    static ShaderPropertyTableCache& Instance();

    // The table for these files: parsed once from the bytes the shader source
    // cache holds and shared until the next announced edit. A hit touches no
    // file. Thread-safe. An unreadable surface yields a table whose Errors name the file.
    std::shared_ptr<const ShaderPropertyTable> Resolve(const ShaderPropertyTableInputs& inputs);

    // True when Resolve(inputs) touches no file.
    bool Holds(const ShaderPropertyTableInputs& inputs);

  private:
    struct Entry
    {
        // The shader source cache generation the table was built under.
        uint64_t SourceGeneration = 0;
        std::shared_ptr<const ShaderPropertyTable> Table;
    };

    static std::shared_ptr<const ShaderPropertyTable> Build(const ShaderPropertyTableInputs& inputs, bool& readFailed);

    std::mutex m_Mutex;
    FastHashMap<std::string, Entry> m_Entries;
};

} // namespace GameEngine::Rendering
