#pragma once

#include "Panels/BuildPanel.h"

#include <nlohmann/json.hpp>

#include <cstddef>

namespace GameEngine::Editor
{

// Unique error lines the game build summary lists; `errors` counts all of them.
inline constexpr std::size_t kBuildStatusMaxErrorLines = 10;
// Unique warning lines it lists when the caller asks for them.
inline constexpr std::size_t kBuildStatusMaxWarningLines = 20;

// The `get_build_status` debug response, the summary of the editor's game build:
// {running, succeeded, generation, cancelled, progress, message, errors, errorLines,
// warnings, outputDirectory}. `errors` and `warnings` count the unique lines;
// `errorLines` holds the first kBuildStatusMaxErrorLines of them and `warningLines`
// (present only with `includeWarnings`) the first kBuildStatusMaxWarningLines, so the
// summary stays short however much the build reported.
nlohmann::json DescribeBuildStatus(const BuildPanel::StatusSnapshot& status, bool includeWarnings);

} // namespace GameEngine::Editor
