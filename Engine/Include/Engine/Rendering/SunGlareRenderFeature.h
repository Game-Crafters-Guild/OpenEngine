#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GameEngine::Engine::Renderer
{

// std140/push-constant mirror of GlarePC in sun_glare.vert / sun_glare.frag.
struct SunGlarePushConstants
{
    ::GameEngine::Mathematics::Vector4 SunDirRadius{0.0f, 1.0f, 0.0f, 0.0f};       // xyz toward-sun dir, w angular radius
    ::GameEngine::Mathematics::Vector4 SunColorIrradiance{1.0f, 1.0f, 1.0f, 0.0f}; // rgb sun color, w irradiance E
    ::GameEngine::Mathematics::Vector4 Probe{0.5f, 0.5f, 0.0f, 0.0f};              // xy sun UV, zw probe radius UV per axis
    ::GameEngine::Mathematics::Vector4 Params{1.0f, 1.0f, 0.0f, 0.0f};             // x sky scale, y sun mu, z altitude,
                                                                                   // w scene-depth sample count, 0 = the
                                                                                   // screen probe is off this frame
    // x = terrain-skyline visibility in [0, 1], computed on the CPU from the heightfield so it
    // holds whether or not the sun is on screen. y = planet radius, z = atmosphere radius (the
    // transmittance LUT's shell, meters). w = 1 when the cascade term's buffer holds this
    // frame's value, 0 when the view has no cascades.
    ::GameEngine::Mathematics::Vector4 Horizon{1.0f, 0.0f, 0.0f, 0.0f};
};
// The GPU ABI is the member OFFSETS, so those are what is asserted. The struct carries no
// alignment specifier: each Vector4 is four floats, so the members already land on the 16-byte
// boundaries a std140 vec4 needs, and an alignas here only pads the render-pass lambda that
// captures it by value.
static_assert(sizeof(SunGlarePushConstants) == 80,
              "SunGlarePushConstants must match the shaders' GlarePC block");
static_assert(offsetof(SunGlarePushConstants, SunDirRadius) == 0);
static_assert(offsetof(SunGlarePushConstants, SunColorIrradiance) == 16);
static_assert(offsetof(SunGlarePushConstants, Probe) == 32);
static_assert(offsetof(SunGlarePushConstants, Params) == 48);
static_assert(offsetof(SunGlarePushConstants, Horizon) == 64);
// The packager sums the PER-STAGE ranges against a 128-byte limit. The vertex stage declares
// all 80 and the fragment stage a 32-byte prefix: 112 of the Vulkan-guaranteed 128.
static_assert(sizeof(SunGlarePushConstants) + 32u <= 128u,
              "vertex block + fragment prefix must fit the guaranteed push-constant limit");

// Push-constant mirror of CastersPC in sun_glare_casters.comp.
struct SunGlareCastersPushConstants
{
    ::GameEngine::Mathematics::Vector4 Eye{0.0f, 0.0f, 0.0f, 0.0f};          // xyz camera, w unused
    ::GameEngine::Mathematics::Vector4 SunDirRadius{0.0f, 1.0f, 0.0f, 0.0f}; // xyz toward sun, w disc radius (rad)
};
static_assert(sizeof(SunGlareCastersPushConstants) == 32,
              "SunGlareCastersPushConstants must match sun_glare_casters.comp's CastersPC");

// Owns the GPU resources for the analytic sun glare: the additive fullscreen pipeline, the
// samplers its hoisted vertex-stage lookups use, and the one-workgroup compute pipeline that
// evaluates the cascade term (sun_glare_casters.comp) into a float the vertex stage reads.
// SunGlareRenderNode declares both passes and fills the push constants per view.
//
// TWO pipelines, differing only in the vertex stage's depth uniform. The glare's occlusion
// probe texel-fetches the scene depth ATTACHMENT the world pass wrote, and that attachment is
// multisampled whenever MSAA is on, so it needs a sampler2DMS variant. The shaderpkg packager
// has no keyword mechanism, so a variant is a second package — the same shape as
// ddgi_trace_hw/_sw and world_debug/world_depthonly.
class SunGlareRenderFeature : public IRenderFeature
{
  public:
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsInitialized() const { return m_Initialized; }

    void OnDeviceRebuilt(::GameEngine::Rendering::IDevice* device) override;

    // Terrain skyline along the sun's azimuth, per view, as a TANGENT (rise over run) of the
    // highest terrain elevation an eye sees in that direction.
    //
    // Published by the TERRAIN side rather than computed here, and that is a layering fact
    // rather than a preference: TerrainECS depends on the renderer (it owns a render feature),
    // so the renderer cannot include TerrainECS without a cycle. The terrain extraction system
    // already runs every frame with the world and with RenderServices, so it marches each
    // view's camera and leaves the answer here as a plain float.
    //
    // kNoSkyline means "this term has nothing to say" — no terrain in the scene, or no station
    // along the ray that could be sampled — and a reader must treat it as fully visible rather
    // than as flat ground.
    static constexpr float kNoSkyline = -1.0e30f;

    struct ViewSkyline
    {
        uint32_t ViewId = 0u;
        float Tangent = kNoSkyline;
    };

    // Replaces the WHOLE set every frame rather than accumulating into it. View ids churn as
    // previews and thumbnails come and go, and an accumulating table filled up with dead ids
    // and then starved the live scene view of its terrain term -- silently, because a missing
    // entry reads as "no terrain" rather than as an error. One call per frame, so there is no
    // begin/end pair to get wrong and no capacity to overflow.
    void SetSkylines(const ViewSkyline* entries, size_t count);
    float GetSkylineTangent(uint32_t viewId) const;

    // `sampleCount` is the scene depth attachment's own sample count. An MSAA view with no
    // multisampled variant available returns an invalid id rather than binding a
    // multisampled image to a sampler2D, which is undefined.
    ::GameEngine::Rendering::GraphicsPipelineId GetPipelineId(uint32_t sampleCount) const
    {
        return sampleCount > 1u ? m_PipelineIdMS : m_PipelineId;
    }
    // The layout that matches GetPipelineId(sampleCount): the multisampled one declares the
    // depth probe as a multisampled image, which WebGPU bakes into the bind group layout.
    const ::GameEngine::Rendering::DescriptorSetLayoutDesc& GetLayout(uint32_t sampleCount) const
    {
        return sampleCount > 1u ? m_LayoutMS : m_Layout;
    }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }

    ::GameEngine::Rendering::SamplerHandle GetShadowSampler() const { return m_ShadowSampler; }

    uint32_t GetTransmittanceBinding() const { return m_TransmittanceBinding; }
    uint32_t GetDepthBinding() const { return m_DepthBinding; }
    uint32_t GetViewParamsBinding() const { return m_ViewParamsBinding; }
    uint32_t GetCastersBinding() const { return m_CastersBinding; }
    uint32_t GetExposureBinding() const { return m_ExposureBinding; }

    // The cascade term's pass. An invalid id (the package failed to load) leaves the glare to
    // the screen probe and the skyline, which the node signals through Horizon[3] = 0.
    ::GameEngine::Rendering::ComputePipelineId GetCastersPipelineId() const
    {
        return m_CastersPipelineId;
    }
    const ::GameEngine::Rendering::DescriptorSetLayoutDesc& GetCastersLayout() const
    {
        return m_CastersLayout;
    }
    uint32_t GetCastersShadowArrayBinding() const { return m_CastersShadowArrayBinding; }
    uint32_t GetCastersShadowDataBinding() const { return m_CastersShadowDataBinding; }
    uint32_t GetCastersOutputBinding() const { return m_CastersOutputBinding; }

  private:
    void InitializeCastersPipeline(::GameEngine::Rendering::IDevice& device);

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Initialized = false;
    // A failed Initialize is final for this device: the node asks every frame.
    bool m_InitializeFailed = false;
    ::GameEngine::Rendering::PipelineDesc m_Pipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_PipelineId{};
    ::GameEngine::Rendering::GraphicsPipelineId m_PipelineIdMS{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_LayoutMS{};
    ::GameEngine::Rendering::SamplerHandle m_Sampler{};
    ::GameEngine::Rendering::SamplerHandle m_ShadowSampler{};
    uint32_t m_TransmittanceBinding = 0;
    uint32_t m_DepthBinding = 1;
    uint32_t m_ViewParamsBinding = 2;
    uint32_t m_CastersBinding = 3;
    uint32_t m_ExposureBinding = 5;
    ::GameEngine::Rendering::ComputePipelineId m_CastersPipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_CastersLayout{};
    uint32_t m_CastersShadowArrayBinding = 0;
    uint32_t m_CastersShadowDataBinding = 1;
    uint32_t m_CastersOutputBinding = 2;
    // Flat rather than a map: a frame has a handful of views, this is written once and read
    // once per view per frame, and a linear scan beats a hash at that size. Reused across
    // frames so the per-frame refill does not allocate.
    std::vector<ViewSkyline> m_Skyline;
};

} // namespace GameEngine::Engine::Renderer
