#include "SplineRecipes/SplineRebuildGate.h"

#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"

#include <cstring>

namespace GameEngine::SplineRecipes
{

bool SplineObservedInputs::operator==(const SplineObservedInputs& other) const
{
    return Valid == other.Valid && SplineVersion == other.SplineVersion &&
           DataIndex == other.DataIndex && DataGeneration == other.DataGeneration &&
           std::memcmp(WorldMatrix.Data(), other.WorldMatrix.Data(), sizeof(float32) * 16u) == 0;
}

SplineObservedInputs ObserveSpline(const SplineECS::SplineService& splineService,
                                   uint32 dataIndex,
                                   uint32 dataGeneration,
                                   const Mathematics::Matrix4x4& worldMatrix)
{
    SplineObservedInputs inputs;
    if (const auto* data = splineService.GetSplineData(SplineECS::SplineHandle(dataIndex, dataGeneration)))
    {
        inputs.SplineVersion = data->Version;
        inputs.Valid = data->IsValid();
    }
    inputs.DataIndex = dataIndex;
    inputs.DataGeneration = dataGeneration;
    inputs.WorldMatrix = worldMatrix;
    return inputs;
}

} // namespace GameEngine::SplineRecipes
