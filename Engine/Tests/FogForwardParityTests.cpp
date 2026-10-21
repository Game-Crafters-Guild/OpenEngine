// A surface fogged in its own shader takes the same fog as the passes that fog the resolved scene
// give a pixel showing a surface at the same distance along the same direction. Both sides run as they
// ship. For the height fog: height_fog_pixel_probe.frag, which evaluates height_fog_pixel.glsl (the
// cloud composites and the fog glow) over a 1x1 scene whose depth puts the surface there, against
// forward_fog_probe*.frag, which evaluate forward_fog.glsl (the forward adapter's fog) for a surface at
// that place. For the volumetric fog: volumetric_fog_composite.frag over a surface at the same
// distance, reading a froxel volume this file fills, against volumetric_fog_forward_probe*.frag. The
// composite filters the volume over five screen taps, each at the depth under it, and the forward fog
// reads one trilinear tap at the surface's own distance: the composite's surface is one at the same
// distance under every tap, each tap's depth texel putting it there along its own ray. The CPU places
// the camera, writes the parameter blocks and the volume, composites the volumetric fog's output by
// its blend (scene * transmittance + in-scatter) and compares the pixels; it evaluates no fog.
//
// The blend weights are pinned against the same draws: an additive surface takes the scene's fog less
// its in-scatter, and a premultiplied one of alpha a takes it less (1 - a) of the in-scatter.
//
// Skips only without a Vulkan device; a missing shader fails.
#include <gtest/gtest.h>

#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/HeightFogParamsUBO.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;
using GameEngine::Engine::Renderer::HeightFogColorSource;
using GameEngine::Engine::Renderer::HeightFogFlagDistanceEnabled;
using GameEngine::Engine::Renderer::HeightFogFlagHeightEnabled;
using GameEngine::Engine::Renderer::HeightFogFlagNoiseEnabled;
using GameEngine::Engine::Renderer::HeightFogParamsUBO;

namespace
{
using Pixel = std::array<float, 4>;
using Vec3 = std::array<float, 3>;

void Require(bool condition, const std::string& why)
{
    if (!condition)
        throw std::runtime_error(why);
}

// Where the camera stands and looks: yaw about +Y, then pitch (positive looks up), looking down
// view -Z.
struct Pose
{
    Vec3 Camera{};
    float YawDeg = 0, PitchDeg = 0;
    float Distance = 0;
};

constexpr float kNear = 0.5f;
constexpr float kFar = 20000.0f;

// The view's rotation (world from view), column-major 3x3 as R[col][row].
std::array<Vec3, 3> Rotation(const Pose& pose)
{
    const float y = pose.YawDeg * std::numbers::pi_v<float> / 180.0f;
    const float p = pose.PitchDeg * std::numbers::pi_v<float> / 180.0f;
    const float cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p);
    // Ry(yaw) * Rx(pitch), by columns.
    return {Vec3{cy, 0.0f, -sy}, Vec3{sy * sp, cp, cy * sp}, Vec3{sy * cp, -sp, cy * cp}};
}

// The fragment's place: the camera plus Distance along view -Z.
Vec3 Target(const Pose& pose)
{
    const auto r = Rotation(pose);
    Vec3 out{};
    for (int i = 0; i < 3; ++i)
        out[i] = pose.Camera[i] - r[2][i] * pose.Distance;
    return out;
}

// A 1x1 view looking at the target through its centre. The projection is an infinite reversed-Z
// perspective, so the surface Distance ahead has depth kNear / Distance; the sky is depth 0.
ViewParamsUBO View(const Pose& pose)
{
    ViewParamsUBO v{};
    const auto r = Rotation(pose);
    // view = [R^T | -R^T * camera], column-major.
    float view[16] = {};
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row)
            view[c * 4 + row] = r[row][c];
    for (int row = 0; row < 3; ++row)
    {
        float t = 0;
        for (int k = 0; k < 3; ++k)
            t -= r[row][k] * pose.Camera[k];
        view[12 + row] = t;
    }
    view[15] = 1;
    std::memcpy(v.ge_view, view, sizeof(view));
    // invView = [R | camera].
    float invView[16] = {};
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row)
            invView[c * 4 + row] = r[c][row];
    for (int row = 0; row < 3; ++row)
        invView[12 + row] = pose.Camera[row];
    invView[15] = 1;
    std::memcpy(v.ge_invView, invView, sizeof(invView));
    // P rows: [s 0 0 0], [0 s 0 0], [0 0 0 n], [0 0 -1 0]; its inverse rows:
    // [1/s 0 0 0], [0 1/s 0 0], [0 0 0 -1], [0 0 1/n 0].
    const float s = 1.0f;
    float proj[16] = {}, invProj[16] = {};
    proj[0] = s;
    proj[5] = s;
    proj[14] = kNear;
    proj[11] = -1.0f;
    invProj[0] = 1.0f / s;
    invProj[5] = 1.0f / s;
    invProj[14] = -1.0f;
    invProj[11] = 1.0f / kNear;
    std::memcpy(v.ge_proj, proj, sizeof(proj));
    std::memcpy(v.ge_invProj, invProj, sizeof(invProj));
    v.ge_nearFar[0] = kNear;
    v.ge_nearFar[1] = kFar;
    for (int i = 0; i < 3; ++i)
        v.ge_cameraPosWS[i] = pose.Camera[i];
    v.ge_cameraPosWS[3] = 1;
    for (float& f : v.ge_screenSize)
        f = 1.0f;
    return v;
}

using Mat4 = std::array<float, 16>; // column-major

Mat4 Multiply(const float* a, const float* b)
{
    Mat4 out{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k)
                out[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    return out;
}

// General 4x4 inverse by cofactors (column-major in and out).
Mat4 Inverse(const Mat4& m)
{
    Mat4 inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = double(m[0]) * inv[0] + double(m[1]) * inv[4] + double(m[2]) * inv[8] + double(m[3]) * inv[12];
    for (float& f : inv)
        f = float(f / det);
    return inv;
}

// The volumetric fog's parameter block (volumetric_fog_params_fields.glsl, std140).
struct VolumetricFogParams
{
    float invViewProj[16];
    float view[16];
    float cameraPos[4];
    float sunDirection[4];
    float sunColor[4];
    float ambientColor[4];
    float albedoDensity[4];
    float emissionAnisotropy[4];
    float heightParams[4];
    float localVolumeCenterShape[4];
    float localVolumeAxisXHalfExtent[4];
    float localVolumeAxisYHalfExtent[4];
    float localVolumeAxisZHalfExtent[4];
    float localVolumeParams[4];
    float noiseParams[4];
    float noiseVelocityTime[4];
    float noiseChannelWeights[4];
    float densityParams[4];
    float volumeParams[4];
    float gridParams[4];
    float jitterParams[4];
    float fogExtraParams[4];
    float prevViewProj[16];
    float prevCameraPos[4];
};
static_assert(sizeof(VolumetricFogParams) == 528, "VolumetricFogParams must match volumetric_fog_params_fields.glsl");

constexpr uint32_t kGridW = 8, kGridH = 6, kGridSlices = 16;
// The depth the composite reads its taps from: fine enough that its five taps fall in five texels.
constexpr uint32_t kSphereDepthSize = 64;

// A global fog reaching 800 units, with slices spread by the square, whose light past the volume
// continues analytically (heightParams.w), plus the parameters only the view writes.
VolumetricFogParams BaseVolumetricFog(const Pose& pose)
{
    VolumetricFogParams f{};
    const ViewParamsUBO v = View(pose);
    const Mat4 viewProj = Multiply(v.ge_proj, v.ge_view);
    const Mat4 invViewProj = Inverse(viewProj);
    std::memcpy(f.invViewProj, invViewProj.data(), sizeof(f.invViewProj));
    std::memcpy(f.view, v.ge_view, sizeof(f.view));
    for (int i = 0; i < 3; ++i)
        f.cameraPos[i] = pose.Camera[i];
    f.cameraPos[3] = 1;
    const float sun[3] = {0.3f, 0.8f, -0.52f};
    for (int i = 0; i < 3; ++i)
        f.sunDirection[i] = sun[i];
    f.sunColor[0] = 2.0f;
    f.sunColor[1] = 1.8f;
    f.sunColor[2] = 1.5f;
    f.ambientColor[0] = 0.4f;
    f.ambientColor[1] = 0.45f;
    f.ambientColor[2] = 0.55f;
    f.albedoDensity[0] = 0.8f;
    f.albedoDensity[1] = 0.82f;
    f.albedoDensity[2] = 0.86f;
    f.albedoDensity[3] = 0.0015f;
    f.emissionAnisotropy[0] = 0.01f;
    f.emissionAnisotropy[1] = 0.012f;
    f.emissionAnisotropy[2] = 0.015f;
    f.emissionAnisotropy[3] = 0.35f;
    f.heightParams[0] = 0.0f;   // base height
    f.heightParams[1] = 220.0f; // falloff
    f.heightParams[3] = 1.0f;   // global: continues past the volume
    f.localVolumeParams[2] = 0.15f; // sky fade
    f.volumeParams[0] = 800.0f;
    f.volumeParams[1] = 2.0f;
    f.gridParams[0] = float(kGridW);
    f.gridParams[1] = float(kGridH);
    f.gridParams[2] = float(kGridSlices);
    f.fogExtraParams[0] = 0.75f; // composite depth bias
    return f;
}

HeightFogParamsUBO BaseFog()
{
    HeightFogParamsUBO f{};
    f.intensity = 1.0f;
    f.density = 0.6f;
    f.minDistance = 30.0f;
    f.smoothLength = 200.0f;
    f.baseHeight = 0.0f;
    f.transitionLength = 150.0f;
    f.maxDistance = 3000.0f;
    f.maxOpacity = 0.95f;
    f.sunIntensity = 1.5f;
    f.flags = HeightFogFlagDistanceEnabled | HeightFogFlagHeightEnabled;
    return f;
}

struct Image
{
    TextureHandle Handle{};
    uint32_t Width = 0, Height = 0;
};

struct Pipeline
{
    PipelineHandle Handle{};
    PipelineDesc Desc{};
    ShaderMeta Meta{};
};

class FogForwardParity : public ::testing::Test
{
  protected:
    void SetUp() override
    {
#ifdef _WIN32
        _putenv_s("GE_HEADLESS_TEST", "1");
#else
        setenv("GE_HEADLESS_TEST", "1", 1);
#endif
        Utils::SetShaderPathResolver(+[](const std::filesystem::path& rel)
                                     { return std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR) / rel; });
        DeviceDesc dd{};
        dd.preferredAPI = GraphicsAPI::Vulkan;
        dd.enableDynamicRendering = true;
        m_Device = DeviceFactory::CreateDevice(dd);
        if (!m_Device || !m_Device->Initialize(dd))
            GTEST_SKIP() << "No usable Vulkan device";
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("HeightFogParity"));
        ASSERT_TRUE(m_Sampler.IsValid());
        BufferDesc bd{};
        bd.size = 512;
        bd.usage = static_cast<uint32_t>(BufferUsage::Uniform);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        m_ViewBuffer = m_Device->CreateBuffer(bd);
        m_FogBuffer = m_Device->CreateBuffer(bd);
        bd.size = sizeof(VolumetricFogParams);
        m_VolumetricBuffer = m_Device->CreateBuffer(bd);
        // The view's ExposureHistory, as the pipeline binds it: scene colour stored at pre-exposure 1.
        bd.size = GameEngine::Engine::Renderer::ExposureReadbackFeature::kStateBytes;
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
        m_ExposureBuffer = m_Device->CreateBuffer(bd);
        ASSERT_TRUE(m_ViewBuffer.IsValid() && m_FogBuffer.IsValid() && m_VolumetricBuffer.IsValid() &&
                    m_ExposureBuffer.IsValid());
        GameEngine::Engine::Renderer::ExposureReadbackFeature::State exposure{};
        exposure.Scale = 1.0f;
        exposure.Valid = 1u;
        exposure.PreExposure = 1.0f;
        exposure.PreviousPreExposure = 1.0f;
        m_Device->UpdateBuffer(m_ExposureBuffer, 0, sizeof(exposure), &exposure);
        // A backdrop that changes with direction, for the sky colour sources.
        std::vector<Pixel> backdrop(64 * 32);
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 64; ++x)
                backdrop[y * 64 + x] = {0.2f + 0.6f * x / 63.0f, 0.3f + 0.5f * y / 31.0f, 0.9f - 0.4f * x / 63.0f, 1};
        m_Backdrop = Upload(64, 32, backdrop);
        m_Depth = Allocate(1, 1);
        m_Scene = Allocate(1, 1);
        m_Volume = UploadVolume();
        m_SphereDepth = Allocate(kSphereDepthSize, kSphereDepthSize);
        m_Cube = UploadCube();
    }

    // A froxel volume whose in-scatter grows and transmittance falls with the slice, and which
    // changes across the screen too, so both the slice a distance picks and the composite's
    // screen-space taps show.
    Image UploadVolume()
    {
        std::vector<Pixel> voxels(size_t(kGridW) * kGridH * kGridSlices);
        for (uint32_t z = 0; z < kGridSlices; ++z)
            for (uint32_t y = 0; y < kGridH; ++y)
                for (uint32_t x = 0; x < kGridW; ++x)
                {
                    const float depth = (z + 0.5f) / kGridSlices;
                    const float across = (x + 0.5f) / kGridW, down = (y + 0.5f) / kGridH;
                    voxels[(size_t(z) * kGridH + y) * kGridW + x] = {
                        0.5f * depth + 0.2f * across, 0.45f * depth + 0.15f * down, 0.6f * depth * (0.7f + 0.3f * across),
                        std::exp(-2.2f * depth) * (0.9f + 0.1f * down)};
                }
        TextureDesc td{};
        td.width = kGridW;
        td.height = kGridH;
        td.depth = kGridSlices;
        td.mipLevels = td.arrayLayers = td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        td.debugName = "FogParity.Volume";
        Image image{m_Device->CreateTexture(td), kGridW, kGridH};
        Require(image.Handle.IsValid(), "volume allocation failed");
        GameEngine::Rendering::UploadTexture3D(m_Device.get(), image.Handle, voxels.data(), kGridW, kGridH, kGridSlices, kGridW * sizeof(Pixel));
        m_Images.push_back(image);
        return image;
    }

    // The irradiance cube the fog past the volume reads when the view has IBL: a different colour
    // on every face.
    Image UploadCube()
    {
        TextureDesc td{};
        td.width = td.height = 1;
        td.depth = 1;
        td.mipLevels = td.sampleCount = 1;
        td.arrayLayers = 6;
        td.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        td.flags = TextureCreateFlags::CubeCompatible;
        td.debugName = "FogParity.Irradiance";
        Image cube{m_Device->CreateTexture(td), 1, 1};
        Require(cube.Handle.IsValid(), "cube allocation failed");
        const std::array<Pixel, 6> faces{{{0.9f, 0.8f, 0.7f, 1}, {0.3f, 0.35f, 0.4f, 1}, {1.2f, 1.25f, 1.4f, 1},
                                          {0.2f, 0.18f, 0.15f, 1}, {0.6f, 0.65f, 0.7f, 1}, {0.5f, 0.55f, 0.62f, 1}}};
        auto upload = m_Device->CreateUploadBuffer(sizeof(faces), "FogParity.CubeUpload");
        Require(upload.IsValid(), "upload allocation failed");
        m_Device->UpdateBuffer(upload, 0, sizeof(faces), faces.data());
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(cube.Handle, ResourceState::Undefined, ResourceState::CopyDest,
                                                          0, 1, 0, 6));
        for (uint32_t face = 0; face < 6; ++face)
            cl->CopyBufferToTextureSubresource(upload, cube.Handle, 0, face, 1, 1, face * sizeof(Pixel), sizeof(Pixel));
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(cube.Handle, ResourceState::CopyDest,
                                                          ResourceState::ShaderResource, 0, 1, 0, 6));
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(upload);
        m_Images.push_back(cube);
        return cube;
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (auto image : m_Images)
            m_Device->DestroyTexture(image.Handle);
        for (auto& [name, pipeline] : m_Pipelines)
            m_Device->DestroyPipeline(pipeline.Handle);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        if (m_ViewBuffer.IsValid())
            m_Device->DestroyBuffer(m_ViewBuffer);
        if (m_FogBuffer.IsValid())
            m_Device->DestroyBuffer(m_FogBuffer);
        if (m_VolumetricBuffer.IsValid())
            m_Device->DestroyBuffer(m_VolumetricBuffer);
        if (m_ExposureBuffer.IsValid())
            m_Device->DestroyBuffer(m_ExposureBuffer);
        m_Device->Shutdown();
    }

    Image Allocate(uint32_t w, uint32_t h)
    {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        td.depth = 1;
        td.mipLevels = td.arrayLayers = td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget |
                                         TextureUsage::TransferSrc | TextureUsage::TransferDst);
        td.debugName = "HeightFogParity.Image";
        Image image{m_Device->CreateTexture(td), w, h};
        Require(image.Handle.IsValid(), "image allocation failed");
        m_Images.push_back(image);
        return image;
    }

    void Write(Image image, const std::vector<Pixel>& pixels)
    {
        auto upload = m_Device->CreateUploadBuffer(pixels.size() * sizeof(Pixel), "HeightFogParity.Upload");
        Require(upload.IsValid(), "upload allocation failed");
        m_Device->UpdateBuffer(upload, 0, pixels.size() * sizeof(Pixel), pixels.data());
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image.Handle, ResourceState::Undefined, ResourceState::CopyDest));
        cl->CopyBufferToTextureSubresource(upload, image.Handle, 0, 0, image.Width, image.Height, 0,
                                          image.Width * sizeof(Pixel));
        cl->Barrier(
            ResourceBarrier::CreateTextureBarrier(image.Handle, ResourceState::CopyDest, ResourceState::ShaderResource));
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(upload);
    }

    Image Upload(uint32_t w, uint32_t h, const std::vector<Pixel>& pixels)
    {
        auto image = Allocate(w, h);
        Write(image, pixels);
        return image;
    }

    Pipeline& GetPipeline(const std::string& shader)
    {
        auto [it, added] = m_Pipelines.try_emplace(shader);
        if (!added)
            return it->second;
        auto& p = it->second;
        auto vs = Utils::LoadShaderFile("fullscreen_noinput.vert.spv");
        auto fs = Utils::LoadShaderFile((shader + ".frag.spv").c_str());
        Require(!vs.empty() && !fs.empty(), "missing shader: " + shader);
        StageReflectionResult rvs{}, rfs{};
        ReflectionOptions opts{};
        Require(ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vs.data()), vs.size() / 4,
                             opts, rvs, nullptr),
                "vertex reflection failed");
        Require(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fs.data()), fs.size() / 4,
                             opts, rfs, nullptr),
                "fragment reflection failed: " + shader);
        p.Meta = MergeStages({rvs, rfs});
        p.Desc.type = PipelineType::Graphics;
        p.Desc.vertexShader = vs;
        p.Desc.pixelShader = fs;
        MaterialBuilder::FormatsHint formats{};
        formats.ColorFormats = {static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT)};
        std::string why;
        Require(MaterialBuilder::BuildPipelineDescFromMeta(p.Meta, p.Desc, formats, MaterialBuilder::MergeMode::Auto,
                                                           MaterialBuilder::PushConstantPolicy{}, &why),
                why);
        p.Handle = m_Device->CreatePipeline(p.Desc);
        Require(p.Handle.IsValid(), "pipeline creation failed: " + shader);
        return p;
    }

    // One fullscreen draw of `shader` into a 1x1 target, its set 0 bound by name.
    Pixel Draw(const std::string& shader, const std::vector<uint8_t>& pushConstants = {})
    {
        auto& p = GetPipeline(shader);
        Require(p.Meta.Sets.size() == 1 && p.Meta.Sets[0].Set == 0, "unexpected descriptor sets in " + shader);
        DescriptorSetDesc sd{};
        sd.layout = p.Desc.descriptorSetLayouts[0];
        sd.transient = false;
        auto ds = m_Device->CreateDescriptorSet(sd);
        Require(ds.IsValid(), "descriptor allocation failed");
        for (const auto& binding : p.Meta.Sets[0].Bindings)
        {
            if (binding.Type == ShaderMetaBindingType::kCombinedImageSampler)
            {
                Image image{};
                if (binding.Name == "uSceneColor")
                    image = m_Scene;
                else if (binding.Name == "uDepth")
                    image = m_DrawDepth.Handle.IsValid() ? m_DrawDepth : m_Depth;
                else if (binding.Name == "uSkyBackdrop" || binding.Name == "ge_skyBackdrop")
                    image = m_Backdrop;
                else if (binding.Name == "uIntegrated" || binding.Name == "ge_volumetricFog")
                    image = m_Volume;
                else if (binding.Name == "uFogIrradianceCube" || binding.Name == "ge_volumetricFogIrradiance")
                    image = m_Cube;
                else
                    throw std::runtime_error("unexpected texture " + binding.Name + " in " + shader);
                m_Device->UpdateCombinedImageSamplerBinding(ds, binding.Binding, image.Handle, m_Sampler);
            }
            else if (binding.Type == ShaderMetaBindingType::kUniformBuffer)
            {
                BufferHandle buffer{};
                size_t size = 0;
                if (binding.Name.find("HeightFog") != std::string::npos)
                {
                    buffer = m_FogBuffer;
                    size = sizeof(HeightFogParamsUBO);
                }
                else if (binding.Name.find("VolumetricFog") != std::string::npos || binding.Name == "fog" ||
                         binding.Name == "FogParams")
                {
                    buffer = m_VolumetricBuffer;
                    size = sizeof(VolumetricFogParams);
                }
                else if (binding.Name.find("ViewParams") != std::string::npos)
                {
                    buffer = m_ViewBuffer;
                    size = sizeof(ViewParamsUBO);
                }
                else
                    throw std::runtime_error("unexpected uniform block " + binding.Name + " in " + shader);
                m_Device->UpdateBufferBinding(ds, binding.Binding, buffer, 0, size);
            }
            else if (binding.Name == "uExposure" || binding.Name == "ExposureHistory")
                m_Device->UpdateStorageBufferBinding(ds, binding.Binding, m_ExposureBuffer, 0,
                                                     GameEngine::Engine::Renderer::ExposureReadbackFeature::kStateBytes);
            else
                throw std::runtime_error("unexpected binding " + binding.Name + " in " + shader);
        }
        auto target = Allocate(1, 1);
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(
            ResourceBarrier::CreateTextureBarrier(target.Handle, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc rp{};
        rp.colorTargets[0] = target.Handle;
        rp.colorTargetCount = 1;
        rp.clearColor[0] = true;
        rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        cl->BeginRenderPass(rp);
        cl->SetViewport(0.0f, 0.0f, 1.0f, 1.0f);
        cl->SetScissor(0, 0, 1, 1);
        cl->SetPipeline(p.Handle);
        cl->BindDescriptorSet(0, ds, p.Handle);
        if (!p.Meta.PushConstants.empty())
        {
            std::vector<uint8_t> bytes(p.Meta.PushConstants[0].Size, 0);
            std::memcpy(bytes.data(), pushConstants.data(), std::min(bytes.size(), pushConstants.size()));
            cl->SetPushConstantsById(0, bytes.data(), bytes.size(), 0);
        }
        cl->Draw(3, 1);
        cl->EndRenderPass();
        cl->Barrier(
            ResourceBarrier::CreateTextureBarrier(target.Handle, ResourceState::RenderTarget, ResourceState::CopySource));
        auto rb = m_Device->CreateReadbackBuffer(sizeof(Pixel), "HeightFogParity.Readback");
        Require(rb.IsValid(), "readback allocation failed");
        cl->CopyTextureToBuffer(target.Handle, rb, 1, 1, 0, 0, 0, 0);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        Pixel px{};
        auto* data = m_Device->MapBuffer(rb);
        Require(data != nullptr, "readback map failed");
        std::memcpy(px.data(), data, sizeof(px));
        m_Device->UnmapBuffer(rb);
        m_Device->DestroyBuffer(rb);
        m_Device->DestroyDescriptorSet(ds);
        m_Device->DestroyTexture(target.Handle);
        m_Images.pop_back();
        for (float channel : px)
            Require(std::isfinite(channel), "nonfinite output from " + shader);
        return px;
    }

    void SetView(const Pose& pose)
    {
        const ViewParamsUBO v = View(pose);
        m_Device->UpdateBuffer(m_ViewBuffer, 0, sizeof(v), &v);
    }

    void SetFog(const HeightFogParamsUBO& fog) { m_Device->UpdateBuffer(m_FogBuffer, 0, sizeof(fog), &fog); }

    // The fog of the resolved scene over an opaque surface of `color` at the pose's distance.
    Pixel PostProcess(const Pose& pose, const Pixel& color)
    {
        SetView(pose);
        Write(m_Scene, {color});
        Write(m_Depth, {Pixel{kNear / pose.Distance, 0, 0, 0}});
        return Draw("height_fog_pixel_probe");
    }

    // A surface of `color` and `alpha` at the same place, fogged in its own shader by the probe
    // `probe` (forward_fog_probe and its blend variants, or the volumetric fog's).
    Pixel Forward(const Pose& pose, const Pixel& color, float alpha, const std::string& probe)
    {
        SetView(pose);
        const Vec3 at = Target(pose);
        const float pc[8] = {color[0], color[1], color[2], alpha, at[0], at[1], at[2], 1.0f};
        std::vector<uint8_t> bytes(sizeof(pc));
        std::memcpy(bytes.data(), pc, sizeof(pc));
        return Draw(probe, bytes);
    }

    // The VolumetricFog composite over an opaque surface of `color` at the pose's distance, blended
    // as its pipeline blends (scene * transmittance + in-scatter).
    Pixel Composite(const Pose& pose, const Pixel& color, const VolumetricFogParams& fog)
    {
        SetView(pose);
        m_Device->UpdateBuffer(m_VolumetricBuffer, 0, sizeof(fog), &fog);
        // The composite's taps, as volumetric_fog_composite.frag places them: the pixel's centre and
        // one froxel either way across and down. Each tap's texel holds the reversed-Z depth of the
        // point the pose's distance away along the tap's own ray (view direction (x, y, -1) at NDC
        // (x, y), so depth = near * |(x, y, -1)| / distance).
        std::vector<Pixel> depth(size_t(kSphereDepthSize) * kSphereDepthSize, Pixel{});
        const float offsets[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& o : offsets)
        {
            const float u = 0.5f + o[0] / float(kGridW), v = 0.5f + o[1] / float(kGridH);
            const uint32_t px = uint32_t(u * kSphereDepthSize), py = uint32_t(v * kSphereDepthSize);
            const float x = u * 2.0f - 1.0f, y = (1.0f - v) * 2.0f - 1.0f;
            depth[size_t(py) * kSphereDepthSize + px] = {kNear * std::sqrt(x * x + y * y + 1.0f) / pose.Distance, 0, 0, 0};
        }
        Write(m_SphereDepth, depth);
        m_DrawDepth = m_SphereDepth;
        const Pixel fogged = Draw("volumetric_fog_composite");
        m_DrawDepth = {};
        return {color[0] * fogged[3] + fogged[0], color[1] * fogged[3] + fogged[1], color[2] * fogged[3] + fogged[2], 1};
    }

    Pixel ForwardVolumetric(const Pose& pose, const Pixel& color, float alpha, const std::string& probe,
                            const VolumetricFogParams& fog)
    {
        m_Device->UpdateBuffer(m_VolumetricBuffer, 0, sizeof(fog), &fog);
        return Forward(pose, color, alpha, probe);
    }

    // Every pose, inside the volume and past it: the opaque surface and the surface fogged in its own
    // shader agree, and each blend takes off exactly its share of the in-scatter. `adjust` changes
    // the base fog for the case.
    template <class Adjust>
    void CheckVolumetricParity(const std::string& name, Adjust adjust)
    {
        const Pixel color{0.9f, 0.35f, 0.1f, 1.0f};
        const Pixel black{0, 0, 0, 1};
        float strongest = 0;
        for (Pose pose : Poses())
            for (float distance : {35.0f, 260.0f, 790.0f, 1300.0f, 6000.0f})
            {
                pose.Distance = distance;
                VolumetricFogParams fog = BaseVolumetricFog(pose);
                adjust(fog);
                const Pixel opaque = Composite(pose, color, fog);
                const Pixel inScatter = Composite(pose, black, fog);
                const std::string what = name + ", " + Describe(pose);
                ExpectNear(opaque, ForwardVolumetric(pose, color, 1.0f, "volumetric_fog_forward_probe", fog),
                           what + ", straight alpha");
                Pixel additive{}, premultiplied{};
                for (int c = 0; c < 3; ++c)
                {
                    additive[c] = opaque[c] - inScatter[c];
                    premultiplied[c] = opaque[c] - 0.6f * inScatter[c];
                    strongest = std::max(strongest, std::abs(opaque[c] - color[c]));
                }
                ExpectNear(additive, ForwardVolumetric(pose, color, 1.0f, "volumetric_fog_forward_probe_additive", fog),
                           what + ", additive");
                ExpectNear(premultiplied,
                           ForwardVolumetric(pose, color, 0.4f, "volumetric_fog_forward_probe_premultiplied", fog),
                           what + ", premultiplied at alpha 0.4");
                if (HasFatalFailure() || HasNonfatalFailure())
                    return;
            }
        EXPECT_GT(strongest, 0.3f) << name << ": the fog barely changes the scene in any pose";
    }

    static std::vector<Pose> Poses()
    {
        std::vector<Pose> poses;
        for (float height : {15.0f, 350.0f})
            for (float yaw : {0.0f, 128.0f})
                for (float pitch : {-35.0f, -8.0f, 0.0f, 6.0f, 30.0f})
                    for (float distance : {20.0f, 150.0f, 600.0f, 1400.0f, 2800.0f})
                        poses.push_back(Pose{{40.0f, height, -25.0f}, yaw, pitch, distance});
        return poses;
    }

    static std::string Describe(const Pose& pose)
    {
        return "camera height " + std::to_string(pose.Camera[1]) + ", yaw " + std::to_string(pose.YawDeg) +
               ", pitch " + std::to_string(pose.PitchDeg) + ", distance " + std::to_string(pose.Distance);
    }

    static void ExpectNear(const Pixel& expected, const Pixel& actual, const std::string& what)
    {
        for (int c = 0; c < 3; ++c)
            EXPECT_NEAR(actual[c], expected[c], 2e-5f + 2e-4f * std::abs(expected[c])) << what << ", channel " << c;
    }

    // Every pose under `fog`: the scene's fog and the surface fogged in its own shader agree, and each
    // blend takes off exactly its share of the in-scatter.
    void CheckParity(const HeightFogParamsUBO& fog, const std::string& name)
    {
        SetFog(fog);
        const Pixel color{0.9f, 0.35f, 0.1f, 1.0f};
        const Pixel black{0, 0, 0, 1};
        float strongest = 0;
        for (const Pose& pose : Poses())
        {
            const Pixel opaque = PostProcess(pose, color);
            const Pixel inScatter = PostProcess(pose, black);
            const std::string what = name + ", " + Describe(pose);
            ExpectNear(opaque, Forward(pose, color, 1.0f, "forward_fog_probe"), what + ", straight alpha");
            Pixel additive{}, premultiplied{};
            for (int c = 0; c < 3; ++c)
            {
                additive[c] = opaque[c] - inScatter[c];
                premultiplied[c] = opaque[c] - 0.6f * inScatter[c];
                strongest = std::max(strongest, std::abs(opaque[c] - color[c]));
            }
            ExpectNear(additive, Forward(pose, color, 1.0f, "forward_fog_probe_additive"), what + ", additive");
            ExpectNear(premultiplied, Forward(pose, color, 0.4f, "forward_fog_probe_premultiplied"),
                       what + ", premultiplied at alpha 0.4");
            if (HasFatalFailure() || HasNonfatalFailure())
                return;
        }
        // The fog must matter in these poses, or agreement proves nothing.
        EXPECT_GT(strongest, 0.3f) << name << ": the fog barely changes the scene in any pose";
    }

    std::unique_ptr<IDevice> m_Device;
    SamplerHandle m_Sampler{};
    BufferHandle m_ViewBuffer{}, m_FogBuffer{}, m_VolumetricBuffer{}, m_ExposureBuffer{};
    Image m_Backdrop{}, m_Depth{}, m_Scene{}, m_Volume{}, m_Cube{}, m_SphereDepth{}, m_DrawDepth{};
    std::vector<Image> m_Images;
    std::map<std::string, Pipeline> m_Pipelines;
};
} // namespace

TEST_F(FogForwardParity, HeightFogFixedColourFogAgreesWithTheResolvedScenesFogAtTheSameDistance)
{
    CheckParity(BaseFog(), "fixed colour");
}

TEST_F(FogForwardParity, HeightFogPartialIntensityAgreesWithTheResolvedScenesFog)
{
    HeightFogParamsUBO fog = BaseFog();
    fog.intensity = 0.65f;
    CheckParity(fog, "intensity 0.65");
}

TEST_F(FogForwardParity, HeightFogDistanceOnlyAndHeightOnlyFogAgreeWithThePostProcess)
{
    HeightFogParamsUBO fog = BaseFog();
    fog.flags = HeightFogFlagDistanceEnabled;
    CheckParity(fog, "distance only");
    fog.flags = HeightFogFlagHeightEnabled;
    fog.density = 2.0f;
    CheckParity(fog, "height only");
}

TEST_F(FogForwardParity, HeightFogSkyColourSourcesAgreeWithThePostProcess)
{
    HeightFogParamsUBO fog = BaseFog();
    fog.colorSource = static_cast<int32_t>(HeightFogColorSource::SkyConstant);
    fog.skyHorizonColorR = 0.55f;
    fog.skyHorizonColorG = 0.7f;
    fog.skyHorizonColorB = 0.95f;
    CheckParity(fog, "sky constant");
    // The backdrop read by direction, so a wrong direction on either side shows.
    fog.colorSource = static_cast<int32_t>(HeightFogColorSource::SkyHdri);
    fog.skyHorizonScale = 1.0f;
    CheckParity(fog, "sky HDRI");
}

TEST_F(FogForwardParity, HeightFogNoisyFogAgreesWithTheResolvedScenesFog)
{
    HeightFogParamsUBO fog = BaseFog();
    fog.flags |= HeightFogFlagNoiseEnabled;
    fog.noiseStrength = 0.6f;
    fog.noiseScale = 60.0f;
    fog.noiseTime = 3.25f;
    fog.noiseVelX = 4.0f;
    CheckParity(fog, "noise");
}

TEST_F(FogForwardParity, VolumetricFogAgreesWithTheCompositeInsideAndPastTheVolume)
{
    CheckVolumetricParity("volumetric", [](VolumetricFogParams&) {});
}

TEST_F(FogForwardParity, VolumetricFogLitByIrradiancePastTheVolumeAgreesWithTheComposite)
{
    CheckVolumetricParity("volumetric with IBL", [](VolumetricFogParams& fog) { fog.densityParams[3] = 1.0f; });
}

TEST_F(FogForwardParity, LocalVolumetricFogStopsAtTheVolumeAsTheCompositeDoes)
{
    // Not global: nothing past the volume's far plane, on either side.
    CheckVolumetricParity("local volumetric", [](VolumetricFogParams& fog) { fog.heightParams[3] = 0.0f; });
}
