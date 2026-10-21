#include "Engine/Rendering/SceneProbeRecaptureSchedule.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine
{
namespace Engine::Renderer
{

SceneProbeWorldEpochs SceneProbeWorldEpochs::Read(const RenderServices& services, uint64_t worldId)
{
    SceneProbeWorldEpochs epochs{};
    epochs.RenderContent = services.RenderContentVersion(worldId);
    epochs.ShadowCasters = services.ShadowCasterContentVersion(worldId);
    epochs.Lights = services.WorldLightListVersion(worldId);
    epochs.DynamicDepth = services.GetIdleElisionFrameState().DepthDynamicEpoch;
    epochs.Materials = Material::GetGlobalContentEpoch();
    epochs.Pipelines = services.Materials().PipelineReadinessEpoch();
    return epochs;
}

void SceneProbeRecaptureSchedule::Tick(float deltaTimeSeconds, const SceneProbeWorldEpochs& epochs,
                                       bool pipelinesBuilding)
{
    m_SecondsSinceBake += deltaTimeSeconds;
    if (pipelinesBuilding || epochs.Pipelines != m_ObservedPipelines)
    {
        m_ObservedPipelines = epochs.Pipelines;
        m_FramesPipelinesRested = 0;
    }
    else if (m_FramesPipelinesRested < kPipelineRestFrames)
    {
        ++m_FramesPipelinesRested;
    }
}

bool SceneProbeRecaptureSchedule::Due(uint64_t inputDigest, const SceneProbeWorldEpochs& epochs,
                                      float realtimeInterval) const
{
    if (inputDigest == 0)
        return false;
    if (inputDigest != m_LastInputDigest)
        return true;
    if (realtimeInterval < 0.0f)
        return m_ConvergenceBakesRemaining > 0 ||
               (epochs.Pipelines != m_LastEpochs.Pipelines && PipelinesRested());
    if (realtimeInterval == 0.0f)
        return true;
    return m_SecondsSinceBake >= realtimeInterval &&
           (m_ConvergenceBakesRemaining > 0 || epochs != m_LastEpochs);
}

void SceneProbeRecaptureSchedule::ConsumeBake(uint64_t inputDigest, const SceneProbeWorldEpochs& epochs,
                                              bool realtime)
{
    // A Once probe takes a Pipelines move only once the epoch has rested; until
    // then it keeps the old value, so the move re-arms it after the burst.
    const bool pipelinesTaken = realtime || PipelinesRested();
    const bool pipelinesLanded = pipelinesTaken && epochs.Pipelines != m_LastEpochs.Pipelines;
    const bool changed = inputDigest != m_LastInputDigest || pipelinesLanded ||
                         (realtime && epochs != m_LastEpochs);
    if (changed)
        m_ConvergenceBakesRemaining = kConvergenceBakes;
    else if (m_ConvergenceBakesRemaining > 0)
        --m_ConvergenceBakesRemaining;
    const uint64_t keptPipelines = m_LastEpochs.Pipelines;
    m_LastInputDigest = inputDigest;
    m_LastEpochs = epochs;
    if (!pipelinesTaken)
        m_LastEpochs.Pipelines = keptPipelines;
    m_SecondsSinceBake = 0.0f;
}

} // namespace Engine::Renderer
} // namespace GameEngine
