#include "ApiScanner.h"

#include "ApiDraftReader.h"
#include "ApiResolver.h"

#include <algorithm>

namespace GameEngine::ComponentScanner::ApiScanner
{

ApiScanResult Scan(const std::filesystem::path& modulesDir, const std::vector<std::filesystem::path>& searchDirs)
{
    ApiDraftReader reader;
    reader.ReadModules(modulesDir, searchDirs);
    ApiResolver resolver(reader.Modules());
    ApiScanResult result;
    result.Description = resolver.Resolve();
    result.Refusals = std::move(reader.Refusals());
    result.Refusals.insert(result.Refusals.end(), resolver.Refusals().begin(), resolver.Refusals().end());
    std::sort(result.Refusals.begin(), result.Refusals.end(), [](const ApiRefusal& a, const ApiRefusal& b)
              { return a.File != b.File ? a.File < b.File : a.Line < b.Line; });
    return result;
}

} // namespace GameEngine::ComponentScanner::ApiScanner
