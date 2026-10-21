#include "TerrainGrass/GrassComputePrograms.h"

#include <string>
#include <utility>
#include <vector>

namespace GameEngine::TerrainGrass
{

Rendering::ShaderProgramCompileRequest MakeGrassComputeCompileRequest(const std::filesystem::path& engineShaders,
                                                                      const GrassComputeProgram& program,
                                                                      bool compat)
{
    const std::filesystem::path grassDir = engineShaders / "TerrainGrass";
    Rendering::ShaderProgramCompileRequest req{};
    req.debugName = program.DebugName;
    req.baseDirectory = grassDir;
    // The CBT directory holds cbt_atlas.glsl, the resident-window atlas resolve grass shares.
    req.includeDirs = {engineShaders, engineShaders / "CBT", grassDir};
    // The compat profile selects the named-binding arm of the source, the same arm the
    // web cook builds, so a desktop run under GE_FORCE_COMPAT exercises it too.
    std::vector<std::string> defines;
    if (compat)
        defines.push_back("GE_COMPAT_PROFILE");
    req.stages = {{"cs", program.ShaderFile, "main", std::move(defines)}};
    return req;
}

} // namespace GameEngine::TerrainGrass
