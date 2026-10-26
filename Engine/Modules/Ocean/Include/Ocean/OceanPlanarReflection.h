#pragma once

#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include "Types/FlatMap.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
} // namespace GameEngine::Engine::Renderer::Pipeline

namespace GameEngine::Ocean
{

// Renders the opaque world a second time, reflected across the sea plane, into a
// scaled colour texture the ocean surface samples for mirror-like reflections
// (the reference's planar OceanPlanarReflection + _ReflectionTex).
//
// One mirror per view that draws water, keyed by that view's id, because each
// view's reflection is its own camera mirrored: the editor's Scene View and Game
// View (and every recording) each see their own. Two views that both see water
// cost two reflection world passes.
//
// Unlike the other ocean sims (compute passes), this re-renders scene GEOMETRY,
// so it reuses the engine's multi-view machinery instead of a bespoke shader:
//   - Per mirror, a persistent secondary view + camera. The view is marked
//     activeRenderPipeline=false (the main pipeline skips it) but keeps the main
//     view's render-layer mask + world id, so render extraction STILL submits the
//     whole scene to it (extraction gates on the view's render layers, not on the
//     pipeline-active flag). We then drive its world pass explicitly, mirroring
//     the editor thumbnail path.
//   - Each frame: mirror the view's camera across y = SeaLevel (a Householder
//     reflection), build the secondary view's draw stream (lights, batch keys,
//     bucketer), and AddWorldPassForView into a target sized to that view.
//   - The mirror matrix flips triangle winding; the view is flagged
//     SetViewWorldPassFlipY so the world pass inverts the viewport Y to keep the
//     baked front face correct. The image lands vertically mirrored, which the
//     surface compensates for by flipping V when sampling.
//
// The colour targets are owned here (stable physicals) so the forward
// contributor can bind a view's target descriptor-direct on that view's surface
// draw, exactly like OceanSceneGrab's refraction grab; a settle pass returns it
// to ShaderResource each frame. A mirror whose view goes longer than
// kOceanViewHistoryIdleTimeout without declaring (the view closed or stopped
// seeing water) is released with its view, camera and target. IsOceanViewIdle
// measures that time up to the declaring view's previous declare, because a
// re-created mirror draws nothing on its first frame and one long frame must not
// cost a view its reflection. The sweep runs inside a view's declare, so while
// no view sees water the mirrors of the views that saw it stay allocated until
// the feature is destroyed. Degrades cleanly: any decline (no
// camera, the bucketer can't schedule this frame) leaves the surface on the
// procedural sky dome (the PlanarReflectionAvailable gate stays 0).
class OceanPlanarReflection
{
public:
    // One view's mirror: the hidden reflection view and camera, and the target
    // its world pass renders into.
    struct Mirror
    {
        ::GameEngine::Rendering::CameraId CameraId = 0;
        ::GameEngine::Rendering::ViewId ViewId = 0;
        ::GameEngine::Rendering::TextureHandle ColorTexture; // scaled RGBA16F
        uint32_t Width = 0;
        uint32_t Height = 0;
        // The device frame slot the reflection was produced in; UINT32_MAX when
        // it declined.
        uint32_t ReadyFrameIndex = UINT32_MAX;
        std::chrono::steady_clock::time_point LastDeclared{};
        // "Ocean_PlanarReflection_View<id>": the reflection view, its camera, its
        // colour target and its render-graph import. The view registry holds it by
        // pointer.
        std::string Name;
        std::string DepthName;

        bool IsReady(uint32_t frameIndex) const
        {
            return ColorTexture.IsValid() && ReadyFrameIndex == frameIndex;
        }
    };

    ~OceanPlanarReflection();

    // Records the reflection capture of d.View for this frame. Returns true when
    // the view's reflection texture is available to bind on its ocean surface
    // this frame. seaLevel is the calm water plane the camera is mirrored across;
    // now is the steady-clock time the mirrors' idle release is judged against.
    bool DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d, float seaLevel,
                        float resolutionScale, uint32_t frameIndex,
                        std::chrono::steady_clock::time_point now);

    // The mirror of a main view; null when the view has none.
    const Mirror* Find(::GameEngine::Rendering::ViewId viewId) const;

    // True for a hidden reflection view this object owns. The ocean's own
    // contributor skips the surface there (water must not reflect itself).
    bool IsReflectionView(::GameEngine::Rendering::ViewId viewId) const;

    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }

    // Re-import a view's reflection colour into this frame's graph so a
    // same-frame consumer can declare a Sampled read (external imports dedup by
    // physical handle, so this returns the id DeclareForView created — one
    // declared RAW edge orders the consumer after the mirror world pass). Invalid
    // when the view's reflection isn't ready this frame.
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportForSampling(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame, ::GameEngine::Rendering::ViewId viewId,
        uint32_t frameIndex) const;

private:
    // Allocate the mirror's persistent reflection view + camera, configured to
    // receive extraction submissions but skip the active pipeline.
    void EnsureView(Engine::Renderer::RenderServices& rs, Mirror& mirror,
                    ::GameEngine::Rendering::RenderLayerMask renderLayerMask, uint64_t worldId);

    // (Re)create the mirror's scaled colour target (and the shared sampler) when
    // the size changes.
    bool EnsureColorTexture(::GameEngine::Rendering::IDevice& device, Mirror& mirror, uint32_t width,
                            uint32_t height);

    // Release every mirror that IsOceanViewIdle judges idle against the declaring
    // view's previous declare.
    void ReleaseIdleMirrors(std::chrono::steady_clock::time_point declarerPrevious);
    void ReleaseMirror(Mirror& mirror);

    // Build the mirrored camera from the main camera by reflecting across the
    // horizontal plane y = seaLevel (position + orientation), keeping the main
    // projection. viewProj is recomputed so culling + the world pass agree.
    static ::GameEngine::Rendering::CameraData MakeMirroredCamera(
        const ::GameEngine::Rendering::CameraData& main, float seaLevel);

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;

    ::GameEngine::Rendering::SamplerHandle m_Sampler;
    // Boxed so each mirror's names keep their address while the map grows.
    FlatMap<::GameEngine::Rendering::ViewId, std::unique_ptr<Mirror>> m_MirrorsByView;
};

} // namespace GameEngine::Ocean
