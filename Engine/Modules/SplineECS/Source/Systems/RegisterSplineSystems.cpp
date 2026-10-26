#include "SplineECS/Systems/RegisterSplineSystems.h"
#include "SplineECS/Systems/SplineExtractionSystem.h"
#include "SplineECS/Systems/SplineNetworkSystem.h"
#include "ECS/SystemScheduling.h"

namespace GameEngine::SplineECS
{

void AddSplineSystemsToSchedule(ECS::SystemScheduleBuilder& b)
{
    b.Add<SplineExtractionSystem>("SplineExtraction", ECS::SystemPhase::Extraction, 1, {});
    b.Add<SplineNetworkSystem>("SplineNetwork", ECS::SystemPhase::Extraction, 1,
                               {"SplineExtraction", "TransformHierarchy"});
}

} // namespace GameEngine::SplineECS
