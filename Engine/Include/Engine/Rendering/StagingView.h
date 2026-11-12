#pragma once

#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Types/Types.h"

#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Rendering
{
enum class MaterialKeyword : uint64_t;
namespace Passes
{
struct TonemapParams;
}
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
class Camera;
class StagingViewPool;

struct StagingViewTag
{
};
/// The pool's key for one entry. Only StagingViewPool and StagingView use it.
using StagingViewHandle = Rendering::StrongHandle<StagingViewTag>;

/// What DeclareWorldPass declared: the color the world pass wrote and its depth.
struct StagingWorldPassRG
{
    Rendering::RenderGraph::RGTexture Color{};
    Rendering::RenderGraph::RGTexture Depth{};
    uint32 Width = 0;
    uint32 Height = 0;
    bool IsValid() const { return Color.IsValid() && Depth.IsValid(); }
};

/**
 * @brief One staging view that a host holds: an ECS world, a camera, an on-demand view and an output texture.
 * StagingViewPool::Acquire makes it, and the destructor gives the view back to the pool. Main thread only,
 * outside a system or a job. Every function except IsAlive requires a live view.
 */
class StagingView final
{
  public:
    /// Holds no view.
    StagingView() = default;
    ~StagingView();
    /// The source holds no view after the move, so one object owns the view.
    StagingView(StagingView&& other) noexcept;
    /// Releases the view this object holds, then takes the view of `other`.
    StagingView& operator=(StagingView&& other) noexcept;
    StagingView(const StagingView&) = delete;
    StagingView& operator=(const StagingView&) = delete;

    /// False for an empty object, and after the renderer shut down and destroyed the pool.
    bool IsAlive() const;
    /// The world the view shows. The host spawns its subject here and owns those entities until the
    /// view is released; the release destroys the entities that are left.
    ECS::World& World();
    /// The view's id, for per-view settings on the ViewRegistry (forced LOD, post-process override).
    /// The view renders only in a frame that ViewRegistry::RequestViewFrame arms.
    Rendering::ViewId ViewId() const;
    /// The camera, projected at the output aspect (Camera::ToCameraData). It stays until the next
    /// SetCamera; a new view has a default Camera. A look-at pose is Inverse(MakeLookAtLH(eye, target, up)).
    void SetCamera(const Camera& camera);
    /// The size the next declared frame renders at, in device pixels; a host frames its content to this aspect.
    /// A StagingViewport control sets the size from its layout. A view without a control keeps the
    /// StagingViewDesc size.
    uint32 RenderWidth() const;
    uint32 RenderHeight() const;
    /// Declares the world pass at the render size. It clears to alpha 0, so the subject composites over the
    /// panel. Invalid when the frame cannot take it (no device, no draw-stream spine); the host tries again later.
    StagingWorldPassRG DeclareWorldPass(Rendering::RenderGraph::RGFrame& frame, Rendering::MaterialKeyword keywords);
    /// Tonemaps `source` into the output texture. A host can add passes between the two declare calls.
    /// False when nothing wrote the output.
    bool DeclareOutput(Rendering::RenderGraph::RGFrame& frame, Rendering::RenderGraph::RGTexture source,
                       const Rendering::Passes::TonemapParams& params);
    /// The name a UI registers the output under (UIManager::SetExternalTexture, DisplayLinearSdr).
    const std::string& UiTextureName() const;
    /// Invalid until the first DeclareOutput, and again after a device rebuild until the next one.
    Rendering::TextureHandle OutputTexture() const;
    /// Zero while OutputTexture is invalid.
    uint32 OutputWidth() const;
    uint32 OutputHeight() const;
    /// Increments each time DeclareOutput writes the output; a UI rebinds when it changes.
    uint64 OutputRevision() const;

  private:
    friend class StagingViewPool;

    StagingView(StagingViewPool& pool, StagingViewHandle handle);
    void Release();
    void Detach();

    StagingViewPool* m_Pool = nullptr;
    StagingViewHandle m_Handle{};
};
} // namespace GameEngine::Engine::Renderer
