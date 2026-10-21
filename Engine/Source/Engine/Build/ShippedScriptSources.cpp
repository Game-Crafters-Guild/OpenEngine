#include "ShippedScriptSources.h"

#include <string>
#include <system_error>

namespace GameEngine
{

namespace fs = std::filesystem;

bool HasAuthoredCSharpSources(const fs::path& scriptsRoot)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(scriptsRoot, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        std::error_code entryEc;
        if (it->is_directory(entryEc))
        {
            const std::string dir = it->path().filename().string();
            if (dir == "obj" || dir == "bin")
            {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (it->is_regular_file(entryEc) && it->path().extension() == ".cs")
            return true;
    }
    return false;
}

ShippedScriptSources CollectShippedScriptSources(const fs::path& scriptsRoot,
                                                 const std::vector<PackageCodeModule>& packageModules)
{
    ShippedScriptSources sources;
    if (HasAuthoredCSharpSources(scriptsRoot))
        sources.AuthoredRoot = scriptsRoot;
    for (const PackageCodeModule& module : packageModules)
    {
        if (module.Lang == PackageModuleRecord::ModuleLang::CSharp &&
            module.Kind == PackageModuleRecord::ModuleKind::Runtime)
            sources.PackageModules.push_back(module);
    }
    return sources;
}

ScriptCompileStep DecideScriptCompile(const ShippedScriptSources& sources, bool generatedProjectExists,
                                      bool nativeAot)
{
    if (sources.IsEmpty())
        return ScriptCompileStep::Skip;
    if (generatedProjectExists || nativeAot)
        return ScriptCompileStep::Compile;
    return ScriptCompileStep::MissingProject;
}

} // namespace GameEngine
