#include "DebugServer/BuildStatusReport.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

// The distinct lines of `lines` in first-seen order.
std::vector<std::string> UniqueLines(const std::vector<std::string>& lines)
{
    std::vector<std::string> unique;
    std::unordered_set<std::string> seen;
    for (const std::string& line : lines)
        if (seen.insert(line).second)
            unique.push_back(line);
    return unique;
}

nlohmann::json FirstLines(const std::vector<std::string>& lines, std::size_t maxLines)
{
    nlohmann::json first = nlohmann::json::array();
    for (std::size_t i = 0; i < lines.size() && i < maxLines; ++i)
        first.push_back(lines[i]);
    return first;
}

} // namespace

nlohmann::json DescribeBuildStatus(const BuildPanel::StatusSnapshot& status, bool includeWarnings)
{
    const std::vector<std::string> errors = UniqueLines(status.progress.errors);
    const std::vector<std::string> warnings = UniqueLines(status.progress.warnings);
    nlohmann::json reply{{"running", status.running},
                         {"succeeded", status.succeeded},
                         {"generation", status.generation},
                         {"cancelled", status.progress.cancelled},
                         {"progress", status.progress.progress},
                         {"message", status.progress.statusMessage},
                         {"errors", errors.size()},
                         {"errorLines", FirstLines(errors, kBuildStatusMaxErrorLines)},
                         {"warnings", warnings.size()},
                         {"outputDirectory", status.outputDirectory.string()}};
    if (includeWarnings)
        reply["warningLines"] = FirstLines(warnings, kBuildStatusMaxWarningLines);
    return reply;
}

} // namespace GameEngine::Editor
