#include "SceneDegradedReport.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace GameEngine::Editor
{

nlohmann::json BuildSceneDegradedReport(const std::optional<SceneLoadDegraded>& atLoad,
                                        const std::optional<SceneLoadDegraded>& outstanding,
                                        std::size_t skipsOffset)
{
    if (!atLoad.has_value())
        return nlohmann::json();

    nlohmann::json skips = nlohmann::json::array();
    if (outstanding.has_value())
    {
        const auto& rows = outstanding->Census.skips;
        const std::size_t first = std::min(skipsOffset, rows.size());
        const std::size_t last = first + std::min(kSceneDegradedSkipsPerPage, rows.size() - first);
        for (std::size_t index = first; index < last; ++index)
        {
            const Scene::SceneDegradation::Skip& skip = rows[index];
            skips.push_back(nlohmann::json{{"entity", skip.entityId},
                                           {"component", skip.component},
                                           {"field", skip.field},
                                           {"message", skip.message},
                                           {"file", skip.file.string()},
                                           {"line", skip.line},
                                           {"preserved", skip.preserved}});
        }
    }

    return nlohmann::json{
        {"path", atLoad->DocumentPath.string()},
        {"loadTimeCount", atLoad->SkippedCount()},
        {"outstandingCount", outstanding.has_value() ? outstanding->SkippedCount() : std::size_t{0}},
        {"droppedCount", outstanding.has_value() ? outstanding->DroppedCount() : std::size_t{0}},
        {"skipsOffset", skipsOffset},
        {"skips", std::move(skips)}};
}

} // namespace GameEngine::Editor
