#include "Rendering/Core/FrustumCullingStrategy.h"

#include "Rendering/Core/GPUCulling.h"

namespace GameEngine
{
namespace Rendering
{

void FrustumCullingStrategy::ScheduleCulling(const ViewCullingContext& ctx)
{
    if (ctx.CullingPipeline == nullptr)
    {
        return;
    }

    ctx.CullingPipeline->SubmitView(MakeViewCullingInput(ctx));
}

} // namespace Rendering
} // namespace GameEngine
