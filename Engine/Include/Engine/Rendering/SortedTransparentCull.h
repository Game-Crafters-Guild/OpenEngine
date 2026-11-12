#pragma once

// Pure CPU frustum culling for the sorted transparent path, factored out of
// RenderServices so it can be unit-tested with mock spheres and no GPU state.
// (The back-to-front ORDERING lives on the GPU as of transparency-scale S2 —
// sorted_transparent_drain.comp — with BuildSortedTransparentCpuOrder in
// SortedTransparentRuns.h as the past-capacity CPU twin.)

#include "Rendering/Common/Frustum.h" // TestSphereFrustum, Vector3, Vector4

#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace GameEngine::Engine::Renderer
{

// One visible transparent instance's center plus an opaque caller key (an index the
// caller uses to recover the record after the cull). The center is relative to the
// origin of the view's render origin sector.
struct TransparentCentroid
{
    uint32_t Key;
    Rendering::Vector3 Center;
};

// A transparent draw candidate before culling: its GPUScene bounding sphere plus the
// caller key. The caller composes the sector-local GPUInstance center against the
// view's sector (SectorLocalPointRelativeTo), as frustum_culling.comp does on the GPU.
struct TransparentSphere
{
    uint32_t Key;
    Rendering::Vector3 Center;
    float Radius;
};

// Appends the frustum-visible subset of `candidates` to `outVisible`. `planes` are the
// 6 inward planes of the view in the frame of its render origin sector (from viewProjRel),
// the frame of every candidate center. Returns the number culled.
//
// TestSphereFrustum inflates the radius 1.5x, the margin the GPU cull applies, so the
// test keeps a straddling sphere: a false reject pops a visible transparent.
inline uint32_t CullTransparentsToFrustum(std::span<const TransparentSphere> candidates,
                                          const Rendering::Vector4* planes,
                                          std::pmr::vector<TransparentCentroid>& outVisible)
{
    uint32_t culled = 0;
    for (const TransparentSphere& c : candidates)
    {
        if (!Rendering::TestSphereFrustum(c.Center, c.Radius, planes))
        {
            ++culled;
            continue;
        }
        outVisible.push_back({c.Key, c.Center});
    }
    return culled;
}

} // namespace GameEngine::Engine::Renderer
