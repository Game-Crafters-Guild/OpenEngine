#include "Engine/Rendering/CascadeMotionScheduler.h"

#include "Engine/Rendering/CascadeShadowCache.h"

#include <algorithm>
#include <cassert>

namespace GameEngine
{
namespace Engine::Renderer
{

CascadeUpdateClass ClassifyCascadeDirtyCause(CascadeCacheDirtyCause cause)
{
    switch (cause)
    {
    case CascadeCacheDirtyCause::Cached:
        return CascadeUpdateClass::Cached;
    case CascadeCacheDirtyCause::CameraChanged:
    case CascadeCacheDirtyCause::CascadeFitChanged:
    case CascadeCacheDirtyCause::CasterDeformed:
        return CascadeUpdateClass::Motion;
    default:
        // FirstRender, ContributorPresent, ExecUnderDraw, ContributorChanged,
        // PhysicalChanged, CasterContentChanged, ConfigChanged, CullNotSettled:
        // the retained layer is (or may be) WRONG, not merely stale — render now.
        return CascadeUpdateClass::Correctness;
    }
}

CascadeUpdateClass ClassifyCascadePair(CascadeCacheDirtyCause depthCause,
                                       const CascadeCacheDirtyCause* tintCause,
                                       bool contentFitsLockstep)
{
    CascadeUpdateClass cls = ClassifyCascadeDirtyCause(depthCause);
    if (!tintCause)
        return cls;
    const CascadeUpdateClass tintCls = ClassifyCascadeDirtyCause(*tintCause);
    if (tintCls > cls)
        cls = tintCls;
    // Deferral keeps BOTH retained layers under the depth-content fit; a
    // Motion pair whose families hold content from DIFFERENT fits must render
    // instead (see the header contract).
    if (cls == CascadeUpdateClass::Motion && !contentFitsLockstep)
        cls = CascadeUpdateClass::Correctness;
    return cls;
}

namespace
{
bool IsCameraMotionCause(CascadeCacheDirtyCause cause)
{
    return cause == CascadeCacheDirtyCause::CameraChanged ||
           cause == CascadeCacheDirtyCause::CascadeFitChanged;
}
} // namespace

bool IsCameraMotionPair(CascadeUpdateClass pairClass, CascadeCacheDirtyCause depthCause,
                        const CascadeCacheDirtyCause* tintCause)
{
    if (pairClass != CascadeUpdateClass::Motion)
        return false;
    return IsCameraMotionCause(depthCause) || (tintCause && IsCameraMotionCause(*tintCause));
}

CascadeMotionScheduler::FramePlan CascadeMotionScheduler::PlanFrame(
    uint32_t viewId, uint32_t numCascades,
    const std::array<CascadeUpdateClass, kMaxCascades>& classes,
    const std::array<uint64_t, kMaxCascades>& staleness, uint32_t motionCap, uint32_t maxAge,
    CascadeMotionSource source)
{
    assert(numCascades <= kMaxCascades);
    numCascades = std::min(numCascades, kMaxCascades);

    FramePlan plan{};
    if (motionCap == 0)
        return plan;

    // Cached and Correctness slots resolve outside the budget: Cached content
    // equals the current fit (byte-equality is the cache's skip condition) and
    // a correctness render replaces the content this frame.
    uint32_t motionSlots[kMaxCascades];
    uint32_t motionCount = 0;
    for (uint32_t c = 0; c < numCascades; ++c)
    {
        if (classes[c] == CascadeUpdateClass::Motion)
            motionSlots[motionCount++] = c;
    }
    if (motionCount == 0)
        return plan;

    Stats& stats = m_Views[viewId];

    // A slot past the staleness bound always renders, most stale first; the rest
    // take the budget nearest-first or defer. A forced slot takes budget under
    // Camera and renders on top of it under CasterDeformation.
    const auto forced = [&](uint32_t c) { return staleness[c] > maxAge; };
    std::sort(motionSlots, motionSlots + std::min(motionCount, kMaxCascades),
              [&](uint32_t a, uint32_t b)
              {
                  const bool fa = forced(a);
                  const bool fb = forced(b);
                  if (fa != fb)
                      return fa; // forced slots first
                  if (fa && staleness[a] != staleness[b])
                      return staleness[a] > staleness[b]; // most stale first
                  return a < b;                           // nearest-first otherwise
              });
    const bool forcedTakesBudget = source == CascadeMotionSource::Camera;
    uint32_t budget = motionCap;
    uint32_t renders = 0;
    for (uint32_t i = 0; i < motionCount; ++i)
    {
        const uint32_t c = motionSlots[i];
        const bool isForced = forced(c);
        if (isForced)
            ++stats.ForcedByAge;
        if (budget > 0 && (!isForced || forcedTakesBudget))
        {
            --budget;
            ++renders;
            continue;
        }
        if (isForced)
        {
            ++renders;
            continue;
        }
        plan.DeferMask |= 1u << c;
        ++stats.Deferred;
    }
    if (renders > motionCap)
        stats.ForcedBeyondCap += renders - motionCap;
    return plan;
}

void CascadeMotionScheduler::Reset()
{
    m_Views.clear();
}

const CascadeMotionScheduler::Stats& CascadeMotionScheduler::GetStats(uint32_t viewId) const
{
    static const Stats kZero{};
    const auto it = m_Views.find(viewId);
    return it != m_Views.end() ? it->second : kZero;
}

} // namespace Engine::Renderer
} // namespace GameEngine
