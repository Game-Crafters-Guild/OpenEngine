#include "Engine/Rendering/IrradianceReadbackFeature.h"

#include "Components/Rendering/LightPhotometry.h"
#include "Engine/Rendering/IEnvironmentSource.h" // complete type for ImageBasedLightingFeature's unique_ptr
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Mathematics/HalfFloat.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PassPhase.h"
#include "Types/ColorUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace GameEngine::Engine::Renderer
{

namespace
{
constexpr uint32_t kChannels = 4;
constexpr uint32_t kTexelBytes = kChannels * sizeof(uint16_t);

Rendering::BufferDesc ReadbackSlotDesc()
{
    Rendering::BufferDesc bd{};
    bd.size = IrradianceReadbackFeature::kSlotBytes;
    bd.usage = static_cast<uint32_t>(Rendering::BufferUsage::TransferDst);
    bd.memoryUsage = Rendering::BufferMemoryUsage::Readback;
    bd.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
    bd.debugName = "IblIrradiance.Readback";
    return bd;
}

// The cube stores E / pi in the scene-linear unit: sky_diffuse_convolve.comp averages
// cosine-sampled radiance, and with the pdf cos(theta) / pi that mean is the estimator of E / pi.
// So E = pi x the texel's luminance in scene-linear units, which LightPhotometry turns into lux
// (1.0 scene-linear = 203 nits).
float IrradianceTexelToLux(const float linearRgb[3])
{
    const float unitless = std::numbers::pi_v<float> * ColorUtils::LinearRec709Luminance(linearRgb);
    return Components::UnitlessToLightIntensity(unitless, Components::LightUnit::Lux);
}

// Mean of the copied texels' RGB, decoded from RGBA16F rows kRowPitchBytes apart.
void MeanTexelRgb(const uint8_t* slot, float outRgb[3])
{
    outRgb[0] = outRgb[1] = outRgb[2] = 0.0f;
    for (uint32_t y = 0; y < IrradianceReadbackFeature::kTexelSpan; ++y)
    {
        for (uint32_t x = 0; x < IrradianceReadbackFeature::kTexelSpan; ++x)
        {
            uint16_t texel[kChannels];
            std::memcpy(texel, slot + y * IrradianceReadbackFeature::kRowPitchBytes + x * kTexelBytes,
                        kTexelBytes);
            for (uint32_t c = 0; c < 3; ++c)
                outRgb[c] += Mathematics::HalfToFloat(texel[c]);
        }
    }
    constexpr float kTexelCount =
        static_cast<float>(IrradianceReadbackFeature::kTexelSpan * IrradianceReadbackFeature::kTexelSpan);
    for (uint32_t c = 0; c < 3; ++c)
        outRgb[c] /= kTexelCount;
}
} // namespace

IrradianceReadbackFeature::IrradianceReadbackFeature() = default;

IrradianceReadbackFeature::~IrradianceReadbackFeature()
{
    m_Ring.Destroy(m_Device);
}

void IrradianceReadbackFeature::DeclareRestingCopy(Rendering::IDevice& device,
                                                  Rendering::RenderGraph::RGFrame& frame,
                                                  const ImageBasedLightingFeature& ibl)
{
    if (!m_NeedsCopyWithoutBake || ibl.GetLastBakedDigest() == 0)
        return;
    const Rendering::RenderGraph::RGTexture restingCube = frame.ImportExternalTexture(
        "IBL_Irradiance", ibl.GetIrradianceCube(), Rendering::ResourceState::ShaderResource,
        Rendering::TextureFormat::R16G16B16A16_FLOAT, 1, ImageBasedLightingFeature::kNumCaptureFaces);
    DeclareCopy(device, frame, restingCube, ImageBasedLightingFeature::kIrradianceSize, ibl.GetLastBakedDigest());
}

void IrradianceReadbackFeature::DeclareCopy(Rendering::IDevice& device,
                                               Rendering::RenderGraph::RGFrame& frame,
                                               Rendering::RenderGraph::RGTexture irradianceCube,
                                               uint32_t cubeSize, uint64_t bakeDigest)
{
    namespace RG = Rendering::RenderGraph;
    m_Device = &device;
    if (!m_Ring.IsInitialized() &&
        !m_Ring.Init(&device, ReadbackSlotDesc(), std::max(1u, device.GetFramesInFlight()) + 2u))
        return;
    const Rendering::BufferHandle slot = m_Ring.BeginWrite(frame, SlotTag{bakeDigest});
    if (!slot.IsValid())
        return;
    m_NeedsCopyWithoutBake = false;

    // The block straddles the face centre, so its mean stands for the +Y normal to second order.
    const uint32_t origin = (cubeSize - kTexelSpan) / 2u;
    RG::RGRange upFace = RG::RGRange::All();
    upFace.BaseMip = 0;
    upFace.MipCount = 1;
    upFace.BaseLayer = kUpFaceLayer;
    upFace.LayerCount = 1;
    frame.AddPass(
        "IBLGen.IrradianceReadback", Rendering::PassPhase::kEarlySetup,
        [irradianceCube, upFace](RG::RGPassBuilder& p)
        {
            p.Read(irradianceCube, RG::RGTextureRead::CopySrc, upFace);
            p.PreventCulling();
        },
        [irradianceCube, slot, origin](RG::RGContext& ctx)
        {
            const Rendering::TextureHandle cube = ctx.GetTexture(irradianceCube);
            if (!cube.IsValid() || !ctx.Cmd)
                return;
            ctx.Cmd->CopyTextureSubresourceToBuffer(cube, 0, kUpFaceLayer, slot, kTexelSpan, kTexelSpan,
                                                    origin, origin, 0, kRowPitchBytes);
        });
    // The cube's importers claim ShaderResource at rest; the copy leaves it a transfer source.
    frame.MarkOutput(irradianceCube, RG::RGImageLayout::ShaderReadOnly);
}

void IrradianceReadbackFeature::OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                                                      const Rendering::IDevice::GpuSyncToken& token)
{
    m_Ring.OnFrameSubmitted(frame, token);
}

void IrradianceReadbackFeature::OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame)
{
    m_Ring.OnFrameStreamRetired(frame);
}

void IrradianceReadbackFeature::OnDeviceRebuilt(Rendering::IDevice* device)
{
    if (!device)
        return;
    // Handle destroys on the dead device are generational no-ops; the ring re-creates on the
    // next copy. The last reading survives until a new one lands.
    m_Ring.Destroy(device);
    m_Device = device;
    m_NeedsCopyWithoutBake = true;
}

bool IrradianceReadbackFeature::TryGetUpIrradianceLux(float& outLux, uint64_t* outBakeDigest)
{
    SlotTag tag{};
    if (m_Device)
    {
        if (const void* mapped = m_Ring.MapNewestReady(m_Device, &tag))
        {
            float rgb[3];
            MeanTexelRgb(static_cast<const uint8_t*>(mapped), rgb);
            m_Ring.Unmap(m_Device);
            const float lux = IrradianceTexelToLux(rgb);
            if (std::isfinite(lux) && lux >= 0.0f)
            {
                m_LastLux = lux;
                m_LastBakeDigest = tag.BakeDigest;
                m_HasReading = true;
            }
        }
    }
    if (!m_HasReading)
        return false;
    outLux = m_LastLux;
    if (outBakeDigest)
        *outBakeDigest = m_LastBakeDigest;
    return true;
}

} // namespace GameEngine::Engine::Renderer
