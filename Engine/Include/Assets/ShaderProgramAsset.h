#pragma once

#include "AssetCore/Asset.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine {

namespace Rendering { struct ShaderPackage; }

// A reusable shader \"program\" description (.shader).
// This is optional; materials can bind stages directly without a program asset.
struct ShaderStageProgramEntry
{
    std::string source;              // source path (.hlsl/.glsl/.vert/.frag/.comp/...)
    std::string entryPoint = "main"; // meaningful for HLSL; GLSL usually stays \"main\"
    std::vector<std::string> defines;
};

struct ShaderProgramDocument
{
    std::string name;
    // stage key: \"vs\",\"fs\",\"cs\",\"ms\",\"gs\" -> entry
    std::unordered_map<std::string, ShaderStageProgramEntry> stages;
};

/// Where compiled shader programs are cached. A program is looked up in each read-only root in
/// order, then in the writable root; a miss compiles into the writable root, so nothing is ever
/// written to a read-only one.
struct ShaderCacheRoots
{
    /// Cooked caches that ship with the runtime (ApplicationPaths::GetBundledShaderCacheRoot), or the
    /// editor cache an offline cook reuses; empty unless that folder exists.
    std::vector<std::filesystem::path> ReadOnly;
    /// <workspace>/.Cache/Shaders; empty until the engine resolves the workspace.
    std::filesystem::path Writable;

    bool operator==(const ShaderCacheRoots&) const = default;
};

class ShaderProgramAsset : public Asset
{
public:
    ShaderProgramAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Shader, path)
    {
    }

    static void SetShaderCacheRoots(ShaderCacheRoots roots) noexcept;
    static const ShaderCacheRoots& GetShaderCacheRoots();

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const ShaderProgramDocument& GetDocument() const { return m_Doc; }
    const std::vector<std::string>& GetErrors() const { return m_Errors; }

    // Returns nullptr if compilation/reflection failed or hasn't run.
    const Rendering::ShaderPackage* GetPackage() const { return m_Package.get(); }
    // Absolute path to the generated .shaderpkg in <workspace>/.Cache/Shaders/..., or empty if not generated.
    const std::string& GetGeneratedShaderPkgPath() const { return m_GeneratedShaderPkgPath; }

private:
    struct ShaderPackageDeleter
    {
        void operator()(Rendering::ShaderPackage* p) const noexcept;
    };

    ShaderProgramDocument m_Doc{};
    std::vector<std::string> m_Errors;
    std::unique_ptr<Rendering::ShaderPackage, ShaderPackageDeleter> m_Package;
    std::string m_GeneratedShaderPkgPath;
};

} // namespace GameEngine

