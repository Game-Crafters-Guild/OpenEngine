#pragma once

// Test-only seam onto the shader package preload store of FrameOrchestrator. Friended by FrameOrchestrator.

#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"

#include <atomic>
#include <functional>

namespace GameEngine::Engine::Renderer
{
struct FrameOrchestratorTestAccess
{
    // The lookup the spine gives the pipeline compile.
    static Pipeline::ShaderMetaLookup PreLoadedShaderMeta(FrameOrchestrator& spine)
    {
        return std::bind_front(&FrameOrchestrator::CopyPreLoadedShaderMeta, &spine);
    }
    static void ClosePreLoadedShaderPackages(FrameOrchestrator& spine) { spine.ClosePreLoadedShaderPackages(); }
    static bool PreLoadedShaderPackagesClosed(const FrameOrchestrator& spine)
    {
        return spine.m_ShaderPkgCacheClosed.load(std::memory_order_relaxed);
    }
};
} // namespace GameEngine::Engine::Renderer
