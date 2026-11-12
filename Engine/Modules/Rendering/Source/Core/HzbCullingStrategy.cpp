#include "Rendering/Core/HzbCullingStrategy.h"

#include "Rendering/Core/GPUCulling.h"

#include <cstdlib>

namespace GameEngine
{
namespace Rendering
{

void HzbCullingStrategy::ScheduleCulling(const ViewCullingContext& ctx)
{
    if (ctx.CullingPipeline == nullptr)
    {
        return;
    }

    ViewCullingInput input = MakeViewCullingInput(ctx);
    // The one difference from FrustumCullingStrategy:
    // reserve the phase-B slice for the view's own (non-cascade) generation.
    input.reserveOcclusionSlice = ReservesOcclusionSlice() && ctx.CascadeIndex == kCullingCascadeIndexNone;

    ctx.CullingPipeline->SubmitView(input);
}

std::shared_ptr<ICullingStrategy> MakeDefaultOcclusionStrategyOrNull()
{
    static const std::shared_ptr<ICullingStrategy> s_Strategy =
        []() -> std::shared_ptr<ICullingStrategy>
    {
        if (const char* env = std::getenv("GE_HZB_OCCLUSION"); env && env[0] == '0')
            return nullptr;
        return std::make_shared<HzbCullingStrategy>();
    }();
    return s_Strategy;
}

} // namespace Rendering
} // namespace GameEngine
