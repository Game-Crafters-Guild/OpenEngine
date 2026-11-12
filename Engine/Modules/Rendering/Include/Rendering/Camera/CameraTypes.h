#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector4.h"
#include "Types/Color.h"
#include "Types/Types.h"
#include "Rendering/RenderLayerMask.h"

#include <cstddef>
#include <memory>

namespace GameEngine
{
namespace Rendering
{

class ICullingStrategy;

using CameraId = uint32;

// Shared per-frame camera data used for rendering.
// Layout is column-major 4x4 matrices suitable for direct GPU upload,
// matching the existing Scene View camera uniform.
struct CameraData
{
    // The matrices are zero until a camera fills them.
    Mathematics::Matrix4x4 view{glm::mat4(0.0f)};     // View matrix (full world space)
    Mathematics::Matrix4x4 proj{glm::mat4(0.0f)};     // Projection matrix
    Mathematics::Matrix4x4 viewProj{glm::mat4(0.0f)}; // View-projection matrix (full world space)
    // World-space camera position in xyz; w is 1 for an editor 2D orthographic view, else 0.
    // A cascade shadow view puts the light direction in xyz. A spot, point or area shadow view
    // puts the light position in xyz and its far plane in w (negative for an area light).
    Mathematics::Vector4 cameraPos;
    // View and view-proj relative to the render origin, the eye's own sector. The vertex stage
    // projects origin-relative positions through them. At sector (0,0,0) they equal view and viewProj.
    // RenderOrigin.h SetCameraEye fills them from the double eye; GLSL mirror camera_ubo_fields.glsl.
    Mathematics::Matrix4x4 viewRel{glm::mat4(0.0f)};
    Mathematics::Matrix4x4 viewProjRel{glm::mat4(0.0f)};
    int32 renderOriginSector[4]{}; // xyz = render origin sector; w = sector size (meters), 0 until set
};
static_assert(sizeof(CameraData) == 352,
              "CameraData must be 352 bytes (5 mat4 + 1 vec4 + 1 ivec4) to match camera_ubo_fields.glsl");
static_assert(offsetof(CameraData, proj) == 64 && offsetof(CameraData, viewProj) == 128 &&
                  offsetof(CameraData, cameraPos) == 192 && offsetof(CameraData, viewRel) == 208 &&
                  offsetof(CameraData, viewProjRel) == 272 && offsetof(CameraData, renderOriginSector) == 336,
              "CameraData member offsets must match camera_ubo_fields.glsl");

/// A camera's near distance and culling range, in meters. The owner of the camera sets them;
/// no code recovers them from the projection matrix.
struct CameraRanges
{
    float Near = 0.0f;
    float CullingRange = 0.0f;
};

// Optional aggregate that carries a camera identifier, its data,
// and an optional debug label. The debugName is non-owning and
// intended only for tooling / logging / UI, not core logic.
struct CameraInfo
{
    CameraId id;
    CameraData data;
    const char* debugName; // may be nullptr
    CameraRanges ranges;
    // True after the registry logged that this camera has no eye. It logs once per camera.
    bool missingEyeLogged = false;
};

// --- Views ---

// Small, opaque identifier for a logical view.
using ViewId = uint32;

// Minimal handle type for view render targets. This is intentionally
// just a 32-bit integer so it can be populated from different backends
// (e.g. RenderGraph logical texture handles) without coupling this
// header to a particular RG implementation.
using ViewTextureHandle = uint32;

struct ViewTargets
{
    // Main color target used for world/entity rendering for this view.
    // For MSAA, this is typically the multisampled color; resolve target
    // (if any) is expressed via `resolve` below.
    ViewTextureHandle color = 0; // 0 = unused

    // Depth-stencil attachment for this view.
    ViewTextureHandle depth = 0; // 0 = unused

    // Optional single-sample resolve target exposed to UI/post (e.g.
    // SceneView.Color when color is SceneView.ColorMSAA). 0 when not used.
    ViewTextureHandle resolve = 0; // 0 = unused

    // Sample count of the color target, tracked so a runtime MSAA change that
    // re-specs the target in place (same handle, new sample count) is still seen as
    // a target change — handle comparison alone misses it.
    uint32_t colorSampleCount = 0;

    // Clear configuration
    bool clearColor = false;
    bool clearDepth = false;
    bool clearStencil = false;
    ColorLinear clearColorValue{0.0f, 0.0f, 0.0f, 0.0f};
    float clearDepthValue = 0.0f;  // reverse-Z: clear to far
    uint8 clearStencilValue = 0;
};

// What a view exists FOR. Behavioral decisions key off this, never off
// debugName substrings — name matching already rotted once (an ocean gate
// compared against a view name that no longer existed anywhere).
enum class ViewPurpose : uint8_t
{
    Game,          // gameplay view (Player, editor Game View)
    EditorScene,   // editor scene viewport
    EditorPreview, // thumbnails, bookmark previews, asset previews
    UtilityCapture // hidden capture views (probes, mirrors, depth captures)
};

// Number of ViewPurpose values, for tables indexed by purpose (the per-view-class
// LOD budget overrides). Keep in step when adding a purpose.
inline constexpr size_t kViewPurposeCount = 4;

// Whether extraction/culling feed the view every frame, or only for frames
// its owner explicitly requested (RenderServices::RequestViewFrame). OnDemand
// views auto-expire — no manual disarm, no hand-rolled mask flipping.
enum class ViewParticipation : uint8_t
{
    Always,
    OnDemand
};

// Lightweight description of a view: which camera to look through, which
// render targets to use, and an optional debug label. Higher-level pipeline
// selection (Forward+, Deferred, debug) will be layered on separately.
struct ViewDesc
{
    ViewId id;
    CameraId cameraId;
    ViewTargets targets;
    // The render layers this view draws: a renderable is submitted to the view
    // when its layers intersect these. Default: every layer.
    RenderLayerMask renderLayerMask = RenderLayerMask::All();
    // When false, the active render pipeline skips this view. The view can
    // still render through explicit passes such as AddWorldPassForView.
    bool activeRenderPipeline = true;
    ViewPurpose purpose = ViewPurpose::Game;
    // OnDemand views participate in extraction, batch-key build, and the GPU
    // culling loop only while participationFrames > 0 (RequestViewFrame sets
    // it; BeginWorldDrawFrame decrements it once per frame). Extraction and
    // culling gate through ActiveRenderLayerMask() and the pipeline through
    // Participates(), so an expired view costs nothing: its effective layers
    // are None while it sleeps.
    ViewParticipation participation = ViewParticipation::Always;
    uint32 participationFrames = 0;
    // While true, the view draws no world: the pipeline only clears its output to the clear
    // colour, so the UI over it still presents. Culling, extraction and shadows skip the view.
    bool worldPaused = false;
    // True for a game view whose worldPaused follows its world's active camera. Extraction then
    // copies the camera's pause into worldPaused before it reads the view, so the frame that
    // resumes the camera extracts the view and draws it.
    bool worldPauseFollowsActiveCamera = false;

    // Whether the view's passes, its own shadow passes included, leave out what the
    // Scene View eye hides (kInstanceFlagEditorHidden): the editor Scene View only.
    // Every other view keeps the instance.
    bool DropsEditorHidden() const { return purpose == ViewPurpose::EditorScene; }

    // Whether the view takes part in this frame: always for an Always view, while armed for an
    // OnDemand one. A view that takes part runs its pipeline even when its layers are None.
    bool Participates() const
    {
        return participation != ViewParticipation::OnDemand || participationFrames > 0;
    }
    // Whether the active render pipeline renders the view this frame: its switch is on and it takes
    // part. A view on no layer still renders its clear and its sky.
    bool RenderedByPipeline() const { return activeRenderPipeline && Participates(); }
    RenderLayerMask ActiveRenderLayerMask() const
    {
        return worldPaused ? RenderLayerMask::None() : UnpausedRenderLayerMask();
    }
    // The layers the view culls and draws once its world is not paused.
    RenderLayerMask UnpausedRenderLayerMask() const
    {
        return Participates() ? renderLayerMask : RenderLayerMask::None();
    }
    // Optional association of this view to a specific ECS world.
    // When non-zero, render extraction should only submit entities from the
    // matching world id. A value of 0 means "accept submissions from any world"
    // (back-compat default).
    uint64 worldId = 0;
    const char* debugName; // may be nullptr

    // Per-view culling strategy. Null = use the engine's default
    // FrustumCullingStrategy at schedule time. Editor thumbnails / asset
    // previews opt out via NoneCullingStrategy; future strategies (HZB,
    // spatial-partition, shared-from-other-view) plug in by assigning a
    // different ICullingStrategy implementation. shared_ptr because the
    // owner (typically the caller of AllocateView) may want the same
    // strategy instance to back multiple views.
    std::shared_ptr<ICullingStrategy> cullingStrategy;
};

// Optional clear configuration used by SetViewTargets variants. Canonical home
// is here (Rendering::ViewClearConfig).
struct ViewClearConfig
{
    bool clearColor;
    ColorLinear clearColorValue;
    bool clearDepth;
    float clearDepthValue;
    bool clearStencil;
    uint8_t clearStencilValue;

    constexpr ViewClearConfig(
        bool clearColorIn = false,
        ColorLinear clearColorValueIn = ColorLinear(0.0f, 0.0f, 0.0f, 1.0f),
        bool clearDepthIn = false,
        float clearDepthValueIn = 0.0f,
        bool clearStencilIn = false,
        uint8_t clearStencilValueIn = 0)
        : clearColor(clearColorIn),
          clearColorValue(clearColorValueIn),
          clearDepth(clearDepthIn),
          clearDepthValue(clearDepthValueIn),
          clearStencil(clearStencilIn),
          clearStencilValue(clearStencilValueIn)
    {
    }
};

} // namespace Rendering
} // namespace GameEngine
