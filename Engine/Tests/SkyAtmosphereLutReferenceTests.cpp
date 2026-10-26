// The sky's three LUTs, run on a real device, against a CPU reference of the same atmosphere.
//
// The transmittance, multiscatter and sky-view passes (sky_*_lut.comp) are dispatched exactly as
// SkyRenderNode dispatches them, with SkyRenderer::FillDefaultAtmosphere's constants and a white
// unit sun, and their texels are compared against a double-precision integral of the same model:
// Rayleigh and Mie densities with their scale heights, Mie absorption and the ozone layer, Rayleigh
// and the aerosol's three-lobe phase, the sun's transmittance integrated
// to the top of the atmosphere and 0 where the planet is in the way (Hillaire 2020's earthShadow),
// and Hillaire's multiple scattering Psi_ms = L2 / (1 - fms) with the isotropic phase on both sphere
// integrals. The reference integrates each step analytically and uses many more steps than the
// LUTs, so it is the integral the LUTs approximate, not a transcription of their arithmetic.
//
// Skips ONLY without a Vulkan device. A missing staged shader FAILS: this target builds them.

#include <gtest/gtest.h>

#include "EngineTestShaderSetup.h"
#include "Mathematics/HalfFloat.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Sky/SkyRenderer.h"
#include "Rendering/Sky/SkySystemConfig.h"

#include "TestDeviceHelper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

using Rgb = std::array<double, 3>;

constexpr double kPi = 3.14159265358979323846;

// SkyRenderer::Config's LUT sizes.
constexpr uint32_t kTransmittanceWidth = 256u;
constexpr uint32_t kTransmittanceHeight = 64u;
constexpr uint32_t kMultiscatterSize = 32u;
constexpr uint32_t kSkyViewSize = 256u;

// The sky-view LUT's production format for the SPIR-V kernels these tests dispatch.
const TextureFormat kSkyViewFormat = SkyRenderer::SkyViewLutFormat(ShaderSourceKind::SpirV);

// The camera stands 3 m above the ground, on the planet's +Y axis.
constexpr double kCameraHeight = 3.0;

// ---------------------------------------------------------------------------------------------
// The CPU reference.

struct Atmosphere
{
    double PlanetRadius = 0.0;
    double TopRadius = 0.0;
    double RayleighScaleHeight = 0.0;
    double MieScaleHeight = 0.0;
    Rgb BetaRayleigh{};
    Rgb BetaMie{};
    Rgb MieAbsorption{};
    Rgb OzoneAbsorption{};
    double OzoneCenterHeight = 0.0;
    double OzoneHalfWidth = 0.0;
    Rgb GroundAlbedo{};
};

Atmosphere FromGpu(const AtmosphereParametersGPU& a)
{
    Atmosphere atmo;
    atmo.PlanetRadius = a.planetRadius;
    atmo.TopRadius = a.atmosphereRadius;
    atmo.RayleighScaleHeight = a.rayleighScaleHeight;
    atmo.MieScaleHeight = a.mieScaleHeight;
    for (int c = 0; c < 3; ++c)
    {
        atmo.BetaRayleigh[c] = a.betaRayleigh[c];
        atmo.BetaMie[c] = a.betaMie[c];
        atmo.MieAbsorption[c] = a.mieAbsorption[c];
        atmo.OzoneAbsorption[c] = a.ozoneAbsorption[c];
        atmo.GroundAlbedo[c] = a.groundAlbedo[c];
    }
    atmo.OzoneCenterHeight = a.ozoneCenterHeight;
    atmo.OzoneHalfWidth = a.ozoneHalfWidth;
    return atmo;
}

double DistanceToTop(const Atmosphere& a, double r, double mu)
{
    const double disc = r * r * (mu * mu - 1.0) + a.TopRadius * a.TopRadius;
    return std::max(0.0, -r * mu + std::sqrt(std::max(disc, 0.0)));
}

// Nearest forward distance to the ground, or a negative value when the ray misses it.
double DistanceToGround(const Atmosphere& a, double r, double mu)
{
    const double disc = r * r * (mu * mu - 1.0) + a.PlanetRadius * a.PlanetRadius;
    if (mu >= 0.0 || disc < 0.0)
        return -1.0;
    return -r * mu - std::sqrt(disc);
}

struct Medium
{
    Rgb ScatterRayleigh{};
    Rgb ScatterMie{};
    Rgb Extinction{};
};

// Rayleigh and Mie scattering on their exponential profiles, Mie absorption on the Mie profile,
// ozone absorption on its tent.
Medium MediumAt(const Atmosphere& a, double h)
{
    const double dr = std::exp(-h / a.RayleighScaleHeight);
    const double dm = std::exp(-h / a.MieScaleHeight);
    const double dozone = std::max(0.0, 1.0 - std::fabs(h - a.OzoneCenterHeight) / a.OzoneHalfWidth);
    Medium m;
    for (int c = 0; c < 3; ++c)
    {
        m.ScatterRayleigh[c] = a.BetaRayleigh[c] * dr;
        m.ScatterMie[c] = a.BetaMie[c] * dm;
        m.Extinction[c] = m.ScatterRayleigh[c] + (a.BetaMie[c] + a.MieAbsorption[c]) * dm +
                          a.OzoneAbsorption[c] * dozone;
    }
    return m;
}

// Transmittance from radius r along zenith cosine mu to the top of the atmosphere, 0 when the
// planet is in the way. 512 midpoint steps in double precision, as the LUT pass takes in float.
Rgb TransmittanceToTop(const Atmosphere& a, double r, double mu)
{
    if (DistanceToGround(a, r, mu) >= 0.0)
        return {0.0, 0.0, 0.0};
    constexpr int kSteps = 512;
    const double ds = DistanceToTop(a, r, mu) / kSteps;
    Rgb tau{};
    for (int i = 0; i < kSteps; ++i)
    {
        const double s = (i + 0.5) * ds;
        const Medium m = MediumAt(a, std::sqrt(r * r + s * s + 2.0 * r * mu * s) - a.PlanetRadius);
        for (int c = 0; c < 3; ++c)
            tau[c] += m.Extinction[c] * ds;
    }
    Rgb t{};
    for (int c = 0; c < 3; ++c)
        t[c] = std::exp(-tau[c]);
    return t;
}

// Hillaire 2020's Psi_ms at radius r for a sun at zenith cosine muSun, per unit sun illuminance:
// the second-order luminance L2 and the transfer fms, both integrated over the sphere of directions
// with the uniform-area weight and the isotropic phase, summed as L2 / (1 - fms). The sphere and
// path sampling are the LUT pass's (8 x 8 directions, 32 analytic steps); the sun's transmittance
// is exact, so the comparison isolates the normalisation and the planet's shadow. A direction that
// ends on the planet adds the sunlight the ground reflects up it (Lambertian, GroundAlbedo).
Rgb PsiMs(const Atmosphere& a, double r, double muSun)
{
    const double sinSun = std::sqrt(std::max(0.0, 1.0 - muSun * muSun));
    const double sun[3] = {sinSun, muSun, 0.0};
    constexpr int kSqrt = 8;
    constexpr int kSteps = 32;
    Rgb l2{};
    Rgb fms{};
    for (int iAz = 0; iAz < kSqrt; ++iAz)
        for (int iZen = 0; iZen < kSqrt; ++iZen)
        {
            const double cosZ = (iZen + 0.5) / kSqrt * 2.0 - 1.0;
            const double sinZ = std::sqrt(std::max(0.0, 1.0 - cosZ * cosZ));
            const double phi = (iAz + 0.5) / kSqrt * 2.0 * kPi;
            const double dir[3] = {sinZ * std::cos(phi), cosZ, sinZ * std::sin(phi)};
            double tEnd = DistanceToTop(a, r, cosZ);
            const double tGround = DistanceToGround(a, r, cosZ);
            if (tGround >= 0.0)
                tEnd = std::min(tEnd, tGround);
            const double dt = tEnd / kSteps;
            Rgb path{1.0, 1.0, 1.0};
            for (int i = 0; i < kSteps; ++i)
            {
                const double t = (i + 0.5) * dt;
                const double p[3] = {dir[0] * t, r + dir[1] * t, dir[2] * t};
                const double rp = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                const Medium m = MediumAt(a, rp - a.PlanetRadius);
                const double sunZenith = (p[0] * sun[0] + p[1] * sun[1] + p[2] * sun[2]) / rp;
                const Rgb toSun = TransmittanceToTop(a, rp, sunZenith);
                for (int c = 0; c < 3; ++c)
                {
                    const double scatter = m.ScatterRayleigh[c] + m.ScatterMie[c];
                    const double stepT = std::exp(-m.Extinction[c] * dt);
                    const double integral = scatter * (1.0 - stepT) / m.Extinction[c];
                    l2[c] += path[c] * integral * toSun[c] / (4.0 * kPi);
                    fms[c] += path[c] * integral;
                    path[c] *= stepT;
                }
            }
            // The ground's Lambertian bounce of the sun, up a ray that ends on the planet.
            if (tGround >= 0.0)
            {
                const double g[3] = {dir[0] * tGround, r + dir[1] * tGround, dir[2] * tGround};
                const double rg = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                const double groundSunCos = (g[0] * sun[0] + g[1] * sun[1] + g[2] * sun[2]) / rg;
                const Rgb toSun = TransmittanceToTop(a, a.PlanetRadius, groundSunCos);
                for (int c = 0; c < 3; ++c)
                    l2[c] += path[c] * toSun[c] * std::max(groundSunCos, 0.0) * a.GroundAlbedo[c] / kPi;
            }
        }
    const double weight = 4.0 * kPi / (kSqrt * kSqrt) / (4.0 * kPi);
    Rgb psi{};
    for (int c = 0; c < 3; ++c)
        psi[c] = l2[c] * weight / (1.0 - fms[c] * weight);
    return psi;
}

// GE_SkyAerosolPhase: the three-lobe Henyey-Greenstein fit to the OPAC continental aerosol's Mie phase.
double HenyeyGreenstein(double g, double cosTheta)
{
    const double d = 1.0 + g * g - 2.0 * g * cosTheta;
    return (1.0 - g * g) / (4.0 * kPi * d * std::sqrt(d));
}

double AerosolPhase(double cosTheta)
{
    return 0.0362 * HenyeyGreenstein(0.9795, cosTheta) + 0.9530 * HenyeyGreenstein(0.6503, cosTheta) +
           0.0108 * HenyeyGreenstein(-0.7055, cosTheta);
}

// Radiance reaching the camera along dir (planet-local, +Y up) per unit sun illuminance: single
// scattering over singleScatterSteps even analytic steps, plus multiple scattering through Psi_ms over
// 128 analytic steps growing quadratically from the camera when withMultiscatter (Psi_ms varies
// slowly along the ray, the aerosol near the ground does not). The scattering angle is the
// same at every step, so miePhase, when given, is the aerosol's phase at it in place of AerosolPhase.
Rgb SkyRadiance(const Atmosphere& a, const double dir[3], const double sun[3], int singleScatterSteps,
                bool withMultiscatter, double cameraHeight = kCameraHeight,
                std::optional<double> miePhase = std::nullopt)
{
    const double r0 = a.PlanetRadius + cameraHeight;
    const double mu = dir[1];
    double tEnd = DistanceToTop(a, r0, mu);
    const double tGround = DistanceToGround(a, r0, mu);
    if (tGround >= 0.0)
        tEnd = std::min(tEnd, tGround);

    const double cosTheta = dir[0] * sun[0] + dir[1] * sun[1] + dir[2] * sun[2];
    const double phaseRayleigh = 3.0 / (16.0 * kPi) * (1.0 + cosTheta * cosTheta);
    const double phaseMie = miePhase ? *miePhase : AerosolPhase(cosTheta);

    Rgb radiance{};
    for (int pass = 0; pass < (withMultiscatter ? 2 : 1); ++pass)
    {
        const bool single = pass == 0;
        const int steps = single ? singleScatterSteps : 128;
        Rgb path{1.0, 1.0, 1.0};
        for (int i = 0; i < steps; ++i)
        {
            const double u0 = static_cast<double>(i) / steps;
            const double u1 = static_cast<double>(i + 1) / steps;
            const double t0 = single ? u0 * tEnd : u0 * u0 * tEnd;
            const double dt = single ? tEnd / steps : (u1 * u1 - u0 * u0) * tEnd;
            const double t = t0 + 0.5 * dt;
            const double p[3] = {dir[0] * t, r0 + dir[1] * t, dir[2] * t};
            const double rp = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            const Medium m = MediumAt(a, rp - a.PlanetRadius);
            const double sunZenith = (p[0] * sun[0] + p[1] * sun[1] + p[2] * sun[2]) / rp;
            Rgb source{};
            if (single)
            {
                const Rgb toSun = TransmittanceToTop(a, rp, sunZenith);
                for (int c = 0; c < 3; ++c)
                    source[c] = (m.ScatterRayleigh[c] * phaseRayleigh + m.ScatterMie[c] * phaseMie) * toSun[c];
            }
            else
            {
                const Rgb psi = PsiMs(a, rp, sunZenith);
                for (int c = 0; c < 3; ++c)
                    source[c] = (m.ScatterRayleigh[c] + m.ScatterMie[c]) * psi[c];
            }
            for (int c = 0; c < 3; ++c)
            {
                const double stepT = std::exp(-m.Extinction[c] * dt);
                radiance[c] += path[c] * source[c] * (1.0 - stepT) / m.Extinction[c];
                path[c] *= stepT;
            }
        }
    }
    return radiance;
}

// The direction a sky-view LUT texel centre stores (GE_SkyViewLutUvToDir with the ContinueHorizon
// fold of sky_view_lut.comp at the planet's horizon), in the planet-local frame the pass builds for a
// camera cameraHeight up on +Y: east = +X, up = +Y, north = +Z.
void SkyViewTexelDirection(const Atmosphere& a, uint32_t x, uint32_t y, double out[3],
                           double cameraHeight = kCameraHeight)
{
    const double u = (x + 0.5) / kSkyViewSize;
    const double v = (y + 0.5) / kSkyViewSize;
    const double az = (u - 0.5) * 2.0 * kPi;
    const double s = v * 2.0 - 1.0;
    const double e = 0.5 + 0.5 * (s < 0.0 ? -1.0 : 1.0) * s * s;
    const double elev = (e - 0.5) * kPi;
    out[0] = std::cos(elev) * std::cos(az);
    out[1] = std::sin(elev);
    out[2] = std::cos(elev) * std::sin(az);
    const double radiusRatio = a.PlanetRadius / (a.PlanetRadius + cameraHeight);
    const double foldSin = -std::sqrt(std::max(0.0, 1.0 - radiusRatio * radiusRatio)) + 0.002;
    if (out[1] < foldSin)
    {
        const double foldCos = std::sqrt(1.0 - foldSin * foldSin);
        out[0] = foldCos * std::cos(az);
        out[1] = foldSin;
        out[2] = foldCos * std::sin(az);
    }
}

// The sky-view row whose texel centres sit closest above the given elevation.
uint32_t SkyViewRowAtElevation(double elevationDegrees)
{
    const double e = 0.5 + elevationDegrees / 180.0;
    const double s = std::sqrt(2.0 * (e - 0.5));
    return static_cast<uint32_t>((0.5 * (s + 1.0)) * kSkyViewSize);
}

std::string Describe(const Rgb& v)
{
    std::ostringstream text;
    text << std::setprecision(4) << "(" << v[0] << ", " << v[1] << ", " << v[2] << ")";
    return text.str();
}

// ---------------------------------------------------------------------------------------------
// The device side: the three passes as SkyRenderNode runs them.

class SkyAtmosphereLutReferenceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        SkyRenderer::FillDefaultAtmosphere(m_AtmoGpu);
        m_Atmo = FromGpu(m_AtmoGpu);

        m_TransmittancePipe = MakePipeline("sky_transmittance_lut", m_TransmittanceLayout,
                                           {{0, DescriptorType::StorageImage}, {1, DescriptorType::UniformBuffer}}, 0);
        m_MultiscatterPipe = MakePipeline("sky_multiscatter_lut", m_MultiscatterLayout,
                                          {{0, DescriptorType::StorageImage},
                                           {1, DescriptorType::CombinedImageSampler},
                                           {2, DescriptorType::UniformBuffer}},
                                          0);
        m_SkyViewPipe = MakePipeline("sky_view_lut", m_SkyViewLayout,
                                     {{0, DescriptorType::StorageImage},
                                      {1, DescriptorType::CombinedImageSampler},
                                      {2, DescriptorType::UniformBuffer},
                                      {3, DescriptorType::UniformBuffer},
                                      {4, DescriptorType::CombinedImageSampler}},
                                     0, kSkyViewFormat);
        m_LookupPipe = MakePipeline("sky_transmittance_lookup_probe", m_LookupLayout,
                                    {{0, DescriptorType::CombinedImageSampler}, {1, DescriptorType::StorageBuffer}},
                                    sizeof(float) * 4u);
        m_MsLookupPipe = MakePipeline("sky_multiscatter_lookup_probe", m_MsLookupLayout,
                                      {{0, DescriptorType::CombinedImageSampler}, {1, DescriptorType::StorageBuffer}},
                                      sizeof(float) * 4u);

        SamplerDesc sd{};
        sd.minFilter = 1;
        sd.magFilter = 1;
        sd.addressModeU = 2;
        sd.addressModeV = 2;
        m_Sampler = m_Device->CreateSampler(sd);
        ASSERT_TRUE(m_Sampler.IsValid());

        m_Transmittance = MakeLut(kTransmittanceWidth, kTransmittanceHeight, "Transmittance");
        m_Multiscatter = MakeLut(kMultiscatterSize, kMultiscatterSize, "Multiscatter");
        m_NoMultiscatter = MakeLut(kMultiscatterSize, kMultiscatterSize, "NoMultiscatter");
        m_SkyView = MakeLut(kSkyViewSize, kSkyViewSize, "SkyView", kSkyViewFormat);

        m_AtmoUbo = MakeBuffer(sizeof(AtmosphereParametersGPU), static_cast<uint32_t>(BufferUsage::Uniform),
                               BufferMemoryUsage::Upload, "Atmosphere");
        m_SkyUbo = MakeBuffer(sizeof(SkyUBOGPU), static_cast<uint32_t>(BufferUsage::Uniform),
                              BufferMemoryUsage::Upload, "Sky");
        Upload(m_AtmoUbo, &m_AtmoGpu, sizeof(m_AtmoGpu));

        RunTransmittanceAndMultiscatter();
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (TextureHandle t : m_Textures)
            m_Device->DestroyTexture(t);
        for (BufferHandle b : m_Buffers)
            m_Device->DestroyBuffer(b);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        m_Device->Shutdown();
    }

    PipelineHandle MakePipeline(const char* shader, DescriptorSetLayoutDesc& layout,
                                std::vector<std::pair<uint32_t, DescriptorType>> bindings, uint32_t pushBytes,
                                TextureFormat storageFormat = TextureFormat::R16G16B16A16_FLOAT)
    {
        const std::string rel = std::string("Shaders/") + shader + ".comp.spv";
        const std::filesystem::path path = GameEngine::Testing::EngineTestShaderPathResolver(rel);
        EXPECT_FALSE(path.empty()) << rel << " was not built into the build directory";
        if (path.empty())
            return {};
        const std::vector<uint8_t> spv = Utils::ReadFile(path.string());
        EXPECT_FALSE(spv.empty()) << path.string();

        layout.debugName = shader;
        for (const auto& [binding, type] : bindings)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = type;
            b.count = 1;
            b.shaderStages = kShaderStageCompute;
            if (type == DescriptorType::StorageImage)
                b.storageTexelFormat = static_cast<uint32_t>(storageFormat);
            if (type == DescriptorType::CombinedImageSampler)
                b.imageFilterableFloat = true;
            layout.bindings.push_back(b);
        }
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
        if (pushBytes > 0u)
        {
            cd.PushConstants.Size = pushBytes;
            cd.PushConstants.StageMask = kShaderStageCompute;
        }
        cd.DebugName = shader;
        const PipelineHandle pipe = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
        EXPECT_TRUE(pipe.IsValid()) << shader;
        return pipe;
    }

    TextureHandle MakeLut(uint32_t w, uint32_t h, const char* name,
                          TextureFormat format = TextureFormat::R16G16B16A16_FLOAT)
    {
        TextureDesc d{};
        d.width = w;
        d.height = h;
        d.format = static_cast<uint32_t>(format);
        d.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess) |
                  static_cast<uint32_t>(TextureUsage::ShaderResource) |
                  static_cast<uint32_t>(TextureUsage::TransferSrc) | static_cast<uint32_t>(TextureUsage::TransferDst);
        d.debugName = name;
        const TextureHandle t = m_Device->CreateTexture(d);
        m_Textures.push_back(t);
        return t;
    }

    BufferHandle MakeBuffer(size_t bytes, uint32_t usage, BufferMemoryUsage memory, const char* name)
    {
        BufferDesc bd{};
        bd.size = bytes;
        bd.usage = usage;
        bd.memoryUsage = memory;
        bd.debugName = name;
        const BufferHandle b = m_Device->CreateBuffer(bd);
        m_Buffers.push_back(b);
        return b;
    }

    void Upload(BufferHandle b, const void* data, size_t bytes)
    {
        void* mapped = m_Device->MapBuffer(b);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, data, bytes);
        m_Device->UnmapBuffer(b);
    }

    DescriptorSetHandle MakeSet(const DescriptorSetLayoutDesc& layout)
    {
        DescriptorSetDesc dsDesc{};
        dsDesc.layout = layout;
        dsDesc.transient = true;
        dsDesc.debugName = layout.debugName;
        return m_Device->CreateDescriptorSet(dsDesc);
    }

    void Submit(const std::function<void(CommandList&)>& record)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        record(*cl);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
    }

    void RunTransmittanceAndMultiscatter()
    {
        const DescriptorSetHandle transDs = MakeSet(m_TransmittanceLayout);
        m_Device->UpdateStorageImageBinding(transDs, 0, m_Transmittance);
        m_Device->UpdateBufferBinding(transDs, 1, m_AtmoUbo, 0, sizeof(AtmosphereParametersGPU));
        const DescriptorSetHandle msDs = MakeSet(m_MultiscatterLayout);
        m_Device->UpdateStorageImageBinding(msDs, 0, m_Multiscatter);
        m_Device->UpdateCombinedImageSamplerBinding(msDs, 1, m_Transmittance, m_Sampler);
        m_Device->UpdateBufferBinding(msDs, 2, m_AtmoUbo, 0, sizeof(AtmosphereParametersGPU));

        const size_t zeroBytes = static_cast<size_t>(kMultiscatterSize) * kMultiscatterSize * 4u * sizeof(uint16_t);
        const BufferHandle zeros = MakeBuffer(zeroBytes, static_cast<uint32_t>(BufferUsage::TransferSrc),
                                              BufferMemoryUsage::Upload, "Zeros");
        const std::vector<uint8_t> zero(zeroBytes, 0u);
        Upload(zeros, zero.data(), zeroBytes);

        Submit([&](CommandList& cl) {
            cl.Barrier(ResourceBarrier::CreateTextureBarrier(m_Transmittance, ResourceState::Undefined,
                                                             ResourceState::UnorderedAccess));
            cl.SetPipeline(m_TransmittancePipe);
            cl.BindDescriptorSet(0, transDs, m_TransmittancePipe);
            cl.Dispatch(kTransmittanceWidth / 8u, kTransmittanceHeight / 8u, 1u);
            cl.Barrier(ResourceBarrier::CreateTextureBarrier(m_Transmittance, ResourceState::UnorderedAccess,
                                                             ResourceState::ShaderResource));
            cl.Barrier(ResourceBarrier::CreateTextureBarrier(m_Multiscatter, ResourceState::Undefined,
                                                             ResourceState::UnorderedAccess));
            cl.SetPipeline(m_MultiscatterPipe);
            cl.BindDescriptorSet(0, msDs, m_MultiscatterPipe);
            cl.Dispatch(kMultiscatterSize / 8u, kMultiscatterSize / 8u, 1u);
            cl.Barrier(ResourceBarrier::CreateTextureBarrier(m_Multiscatter, ResourceState::UnorderedAccess,
                                                             ResourceState::ShaderResource));
            cl.Barrier(ResourceBarrier::CreateTextureBarrier(m_NoMultiscatter, ResourceState::Undefined,
                                                             ResourceState::CopyDest));
            cl.CopyBufferToTextureSubresource(zeros, m_NoMultiscatter, 0u, 0u, kMultiscatterSize, kMultiscatterSize, 0u);
            cl.Barrier(ResourceBarrier::CreateTextureBarrier(m_NoMultiscatter, ResourceState::CopyDest,
                                                             ResourceState::ShaderResource));
        });
    }

    // Reads an rgba16f or rgba32f texture back as rgb doubles, row-major.
    std::vector<Rgb> ReadBack(TextureHandle tex, uint32_t w, uint32_t h,
                              TextureFormat format = TextureFormat::R16G16B16A16_FLOAT)
    {
        const bool wide = format == TextureFormat::R32G32B32A32_FLOAT;
        const size_t channelBytes = wide ? sizeof(float) : sizeof(uint16_t);
        const size_t bytes = static_cast<size_t>(w) * h * 4u * channelBytes;
        const BufferHandle readback = MakeBuffer(bytes, static_cast<uint32_t>(BufferUsage::TransferDst),
                                                 BufferMemoryUsage::Readback, "Readback");
        Submit([&](CommandList& cl) {
            cl.Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::ShaderResource,
                                                             ResourceState::CopySource));
            cl.CopyTextureSubresourceToBuffer(tex, 0u, 0u, readback, w, h);
            cl.Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopySource,
                                                             ResourceState::ShaderResource));
        });
        std::vector<uint8_t> raw(bytes);
        const void* mapped = m_Device->MapBuffer(readback);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(raw.data(), mapped, bytes);
        m_Device->UnmapBuffer(readback);
        std::vector<Rgb> out(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < out.size(); ++i)
            for (size_t c = 0; c < 3u; ++c)
            {
                const size_t offset = (i * 4u + c) * channelBytes;
                if (wide)
                {
                    float value = 0.0f;
                    std::memcpy(&value, raw.data() + offset, sizeof(value));
                    out[i][c] = value;
                }
                else
                {
                    uint16_t half = 0u;
                    std::memcpy(&half, raw.data() + offset, sizeof(half));
                    out[i][c] = Mathematics::HalfToFloat(half);
                }
            }
        return out;
    }

    // Runs the sky-view pass for a white sun of the given intensity along sunDir, and a white unit
    // moon along moonDir when one is given, for a camera cameraHeight up, and returns its texels.
    std::vector<Rgb> RunSkyView(const double sunDir[3], bool withMultiscatter, double cameraHeight = kCameraHeight,
                                float sunIntensity = 1.0f, const double* moonDir = nullptr)
    {
        SkyUBOGPU sky{};
        sky.cameraPosition[1] = static_cast<float>(cameraHeight);
        for (int c = 0; c < 3; ++c)
        {
            sky.sunDirection[c] = static_cast<float>(sunDir[c]);
            sky.sunColor[c] = 1.0f;
            sky.moonDirection[c] = moonDir ? static_cast<float>(moonDir[c]) : 0.0f;
        }
        sky.sunIntensity = sunIntensity;
        sky.moonIntensity = moonDir ? 1.0f : 0.0f;
        Upload(m_SkyUbo, &sky, sizeof(sky));

        const DescriptorSetHandle ds = MakeSet(m_SkyViewLayout);
        m_Device->UpdateStorageImageBinding(ds, 0, m_SkyView);
        m_Device->UpdateCombinedImageSamplerBinding(ds, 1, m_Transmittance, m_Sampler);
        m_Device->UpdateBufferBinding(ds, 2, m_AtmoUbo, 0, sizeof(AtmosphereParametersGPU));
        m_Device->UpdateBufferBinding(ds, 3, m_SkyUbo, 0, sizeof(SkyUBOGPU));
        m_Device->UpdateCombinedImageSamplerBinding(ds, 4, withMultiscatter ? m_Multiscatter : m_NoMultiscatter,
                                                    m_Sampler);
        Submit([&](CommandList& cl) {
            cl.Barrier(ResourceBarrier::CreateTextureBarrier(m_SkyView, ResourceState::Undefined,
                                                             ResourceState::UnorderedAccess));
            cl.SetPipeline(m_SkyViewPipe);
            cl.BindDescriptorSet(0, ds, m_SkyViewPipe);
            cl.Dispatch(kSkyViewSize / 8u, kSkyViewSize / 8u, 1u);
            cl.Barrier(ResourceBarrier::CreateTextureBarrier(m_SkyView, ResourceState::UnorderedAccess,
                                                             ResourceState::ShaderResource));
        });
        return ReadBack(m_SkyView, kSkyViewSize, kSkyViewSize, kSkyViewFormat);
    }

    // The transmittance LUT read through GE_SkyTransmittanceToTop at each (r, mu).
    std::vector<Rgb> LookUpTransmittance(const std::vector<std::array<double, 2>>& queries)
    {
        const float pc[4] = {m_AtmoGpu.planetRadius, m_AtmoGpu.atmosphereRadius, static_cast<float>(queries.size()),
                             0.0f};
        return RunLookupProbe(m_LookupPipe, m_LookupLayout, m_Transmittance, queries, pc);
    }

    // A lookup probe: each query's two values in, the texture read through the shaders' mapping out.
    std::vector<Rgb> RunLookupProbe(PipelineHandle pipe, const DescriptorSetLayoutDesc& layout, TextureHandle lut,
                                    const std::vector<std::array<double, 2>>& queries, const float (&pc)[4])
    {
        const size_t bytes = queries.size() * 4u * sizeof(float);
        BufferDesc bd{};
        bd.size = bytes;
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferSrc) |
                   static_cast<uint32_t>(BufferUsage::TransferDst);
        bd.memoryUsage = BufferMemoryUsage::Readback;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "LookupProbe";
        const BufferHandle data = m_Device->CreateBuffer(bd);
        m_Buffers.push_back(data);
        std::vector<float> in(queries.size() * 4u, 0.0f);
        for (size_t i = 0; i < queries.size(); ++i)
        {
            in[i * 4u + 0] = static_cast<float>(queries[i][0]);
            in[i * 4u + 1] = static_cast<float>(queries[i][1]);
        }
        m_Device->UpdateBuffer(data, 0, bytes, in.data());
        const DescriptorSetHandle ds = MakeSet(layout);
        m_Device->UpdateCombinedImageSamplerBinding(ds, 0, lut, m_Sampler);
        m_Device->UpdateStorageBufferBinding(ds, 1, data, 0, bytes);
        Submit([&](CommandList& cl) {
            cl.SetPipeline(pipe);
            cl.BindDescriptorSet(0, ds, pipe);
            cl.SetPushConstants(pc);
            cl.Dispatch(static_cast<uint32_t>((queries.size() + 63u) / 64u), 1u, 1u);
        });
        std::vector<float> out(queries.size() * 4u);
        const void* mapped = m_Device->MapBuffer(data);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(out.data(), mapped, bytes);
        m_Device->UnmapBuffer(data);
        std::vector<Rgb> result(queries.size());
        for (size_t i = 0; i < queries.size(); ++i)
            result[i] = {out[i * 4u + 0], out[i * 4u + 1], out[i * 4u + 2]};
        return result;
    }

    std::unique_ptr<IDevice> m_Device;
    AtmosphereParametersGPU m_AtmoGpu{};
    Atmosphere m_Atmo{};
    DescriptorSetLayoutDesc m_TransmittanceLayout{};
    DescriptorSetLayoutDesc m_MultiscatterLayout{};
    DescriptorSetLayoutDesc m_SkyViewLayout{};
    DescriptorSetLayoutDesc m_LookupLayout{};
    DescriptorSetLayoutDesc m_MsLookupLayout{};
    PipelineHandle m_TransmittancePipe{};
    PipelineHandle m_MultiscatterPipe{};
    PipelineHandle m_SkyViewPipe{};
    PipelineHandle m_LookupPipe{};
    PipelineHandle m_MsLookupPipe{};
    SamplerHandle m_Sampler{};
    TextureHandle m_Transmittance{};
    TextureHandle m_Multiscatter{};
    TextureHandle m_NoMultiscatter{};
    TextureHandle m_SkyView{};
    BufferHandle m_AtmoUbo{};
    BufferHandle m_SkyUbo{};
    std::vector<TextureHandle> m_Textures;
    std::vector<BufferHandle> m_Buffers;
};

void SunDirection(double elevationDegrees, double out[3])
{
    const double e = elevationDegrees * kPi / 180.0;
    out[0] = std::cos(e);
    out[1] = std::sin(e);
    out[2] = 0.0;
}

// The binary16 encoding of a small non-negative integer (exact below 2048): 2^e x (1 + m / 1024).
uint16_t HalfOfSmallInteger(uint32_t n)
{
    if (n == 0u)
        return 0u;
    uint32_t e = 0u;
    while ((n >> (e + 1u)) != 0u)
        ++e;
    const uint32_t mantissa = (n - (1u << e)) << (10u - e);
    return static_cast<uint16_t>(((e + 15u) << 10u) | mantissa);
}

// |actual / expected - 1| per channel, the largest of the three.
double WorstRatioError(const Rgb& actual, const Rgb& expected)
{
    double worst = 0.0;
    for (int c = 0; c < 3; ++c)
        worst = std::max(worst, std::fabs(actual[c] / expected[c] - 1.0));
    return worst;
}

} // namespace

// The transmittance the sky reads at the horizon from 3 m: the LUT's edge column is the grazing
// ray, so a read there returns the grazing path, not a mix of it with a short path into the ground
// (0.59/0.50/0.47 against the exact 0.156/0.017/0.0001), and a sun below the horizon reads 0.
TEST_F(SkyAtmosphereLutReferenceTest, TheTransmittanceLookupMatchesTheIntegralAtTheHorizonAndShadowsBelowIt)
{
    const double r = m_Atmo.PlanetRadius + kCameraHeight;
    const std::vector<std::array<double, 2>> queries = {{r, 0.5}, {r, 0.05}, {r, 0.0}, {r, -0.02}};
    const std::vector<Rgb> got = LookUpTransmittance(queries);
    ASSERT_EQ(got.size(), queries.size());
    for (size_t i = 0; i < 3u; ++i)
    {
        const Rgb expected = TransmittanceToTop(m_Atmo, r, queries[i][1]);
        // Compared as optical depth, -ln T: a transmittance's relative error is its optical depth's
        // absolute error, so blue at the horizon (depth 9.8, T 5e-5) reads 4.7 % off in T from a
        // 0.5 % error in depth, the bilinear read across the grazing columns. That error grows with
        // the aerosol near the ground: measured worst 0.5 % in pristine air and 1.2 % through the
        // default crisp day's aerosol (green at the horizon, depth 9.6, T 7e-5); the absolute term
        // covers fp16 near T = 1. Through the default aerosol the grazing ray from the ground carries
        // blue at an optical depth over 14, and through a typical day's (0.15) every channel at 16 to
        // 23 (T 1e-7 to 1e-10), below fp16's smallest normal value: there the LUT has to read as dark
        // as fp16 can hold, no more.
        for (int c = 0; c < 3; ++c)
        {
            constexpr double kHalfSmallestNormal = 6.103515625e-5;
            if (expected[c] < kHalfSmallestNormal)
            {
                EXPECT_LT(got[i][c], kHalfSmallestNormal)
                    << "mu " << queries[i][1] << " channel " << c << ": LUT " << Describe(got[i]);
                continue;
            }
            const double depth = -std::log(expected[c]);
            EXPECT_NEAR(-std::log(got[i][c]), depth, depth * 0.015 + 1e-3)
                << "mu " << queries[i][1] << " channel " << c << ": LUT " << Describe(got[i]) << ", exact "
                << Describe(expected);
        }
    }
    for (int c = 0; c < 3; ++c)
        EXPECT_EQ(got[3][c], 0.0) << "a sun 1.1 degrees below the horizon reaches the camera through the planet: "
                                  << Describe(got[3]);
}

// Psi_ms, the multiple-scattering LUT, against Hillaire's normalisation: both sphere integrals carry
// the uniform-area weight and the isotropic phase, so L2 is 1 / (4 pi) of a sum weighted by the
// solid angle alone. The texels span a high sun, a low sun and a sun below the horizon (where only
// the air the planet does not shadow is lit), on the ground (the lowest row) and at 9.7 km.
TEST_F(SkyAtmosphereLutReferenceTest, TheMultiscatterLutIsHillairesPsi)
{
    const std::vector<Rgb> lut = ReadBack(m_Multiscatter, kMultiscatterSize, kMultiscatterSize);
    const double shell = m_Atmo.TopRadius - m_Atmo.PlanetRadius;
    const std::array<std::array<uint32_t, 2>, 5> texels = {{{26u, 0u}, {17u, 0u}, {14u, 0u}, {26u, 3u}, {20u, 3u}}};
    for (const auto& [x, y] : texels)
    {
        // The LUT's texel centres span the full range (the half-texel inset).
        const double muSun = static_cast<double>(x) / (kMultiscatterSize - 1u) * 2.0 - 1.0;
        const double r = m_Atmo.PlanetRadius + static_cast<double>(y) / (kMultiscatterSize - 1u) * shell;
        const Rgb expected = PsiMs(m_Atmo, r, muSun);
        const Rgb& actual = lut[static_cast<size_t>(y) * kMultiscatterSize + x];
        EXPECT_LT(WorstRatioError(actual, expected), 0.03)
            << "texel (" << x << ", " << y << ") sun zenith cosine " << muSun << ", height " << (r - m_Atmo.PlanetRadius)
            << " m: LUT " << Describe(actual) << ", Hillaire " << Describe(expected);
    }
}

// Under a hazy day's aerosol (optical depth 0.11 at single-scattering albedo 0.9) the multiple
// scattering on the ground and 9.7 km up, sun 48 degrees high, with the ground's bounce at the
// default albedo 0.3: without the bounce the LUT holds 0.28x (ground) to 0.41x (9.7 km) of this.
TEST_F(SkyAtmosphereLutReferenceTest, UnderHazeTheMultiscatterLutCarriesTheGroundsBounce)
{
    for (int c = 0; c < 3; ++c)
    {
        m_AtmoGpu.betaMie[c] = 8.333e-5f;
        m_AtmoGpu.mieAbsorption[c] = 9.259e-6f;
    }
    m_Atmo = FromGpu(m_AtmoGpu);
    Upload(m_AtmoUbo, &m_AtmoGpu, sizeof(m_AtmoGpu));
    RunTransmittanceAndMultiscatter();
    const std::vector<Rgb> lut = ReadBack(m_Multiscatter, kMultiscatterSize, kMultiscatterSize);
    const double shell = m_Atmo.TopRadius - m_Atmo.PlanetRadius;
    for (const uint32_t y : {0u, 3u})
    {
        constexpr uint32_t x = 27u;
        const double muSun = static_cast<double>(x) / (kMultiscatterSize - 1u) * 2.0 - 1.0;
        const double r = m_Atmo.PlanetRadius + static_cast<double>(y) / (kMultiscatterSize - 1u) * shell;
        const Rgb expected = PsiMs(m_Atmo, r, muSun);
        const Rgb& actual = lut[static_cast<size_t>(y) * kMultiscatterSize + x];
        EXPECT_LT(WorstRatioError(actual, expected), 0.03)
            << "row " << y << ": LUT " << Describe(actual) << ", Hillaire with the ground " << Describe(expected);
    }
}

// The sky-view march's single scattering at the horizon, multiple scattering left out: each step's
// in-scatter integrated against the transmittance from the step's start. Weighted by the
// transmittance after the step, the 17.7 km horizon steps come out at 0.95/0.88/0.73 of the integral.
TEST_F(SkyAtmosphereLutReferenceTest, TheSkyViewSingleScatterMatchesTheIntegralAtTheHorizon)
{
    double sun[3];
    SunDirection(41.6, sun);
    const std::vector<Rgb> lut = RunSkyView(sun, false);
    const uint32_t horizonRow = kSkyViewSize / 2u;
    for (const uint32_t column : {kSkyViewSize / 2u, 0u})
    {
        double dir[3];
        SkyViewTexelDirection(m_Atmo, column, horizonRow, dir);
        const Rgb expected = SkyRadiance(m_Atmo, dir, sun, 4096, false);
        const Rgb& actual = lut[static_cast<size_t>(horizonRow) * kSkyViewSize + column];
        EXPECT_LT(WorstRatioError(actual, expected), 0.03)
            << "column " << column << " (toward the sun is " << kSkyViewSize / 2u << "): LUT " << Describe(actual)
            << ", integral " << Describe(expected);
    }
}

// The horizon 90 degrees from the sun at 10 h, against the integral with the Mie theory phase of the
// OPAC continental aerosol there (0.02526 per steradian at 90.5 degrees, Hess, Koepke and Schult
// 1998; the computation is in the sky follow-on evidence, mie_opac.py). The aerosol's side scatter
// is what lifts the horizon around the sky: the Cornette-Shanks lobe at g 0.8 the sky used before
// scatters 0.0078 there, and under the pristine aerosol it left this horizon 10 % (red) to 3 % (blue)
// under the integral. Measured 1.3 % under the default crisp day's aerosol.
TEST_F(SkyAtmosphereLutReferenceTest, TheAerosolScattersToTheSideAsMieTheoryDoes)
{
    double sun[3];
    SunDirection(41.6, sun);
    const std::vector<Rgb> lut = RunSkyView(sun, false);
    const uint32_t horizonRow = kSkyViewSize / 2u;
    const uint32_t sideColumn = kSkyViewSize * 3u / 4u; // 90.7 degrees of azimuth from the sun
    double dir[3];
    SkyViewTexelDirection(m_Atmo, sideColumn, horizonRow, dir);
    const double cosTheta = dir[0] * sun[0] + dir[1] * sun[1] + dir[2] * sun[2];
    ASSERT_NEAR(std::acos(cosTheta) * 180.0 / kPi, 90.5, 0.1);
    constexpr double kOpacMiePhaseAt90_5 = 0.02526;
    const Rgb expected = SkyRadiance(m_Atmo, dir, sun, 4096, false, kCameraHeight, kOpacMiePhaseAt90_5);
    const Rgb& actual = lut[static_cast<size_t>(horizonRow) * kSkyViewSize + sideColumn];
    EXPECT_LT(WorstRatioError(actual, expected), 0.03)
        << "LUT " << Describe(actual) << ", integral with the Mie phase " << Describe(expected);
}

// The sun 2 degrees below the horizon: the planet shadows the near-ground air, so only the air high
// enough to still see the sun scatters it. Without the shadow the horizon toward the sun is 22x
// (red) to 19000x (blue) brighter than this.
TEST_F(SkyAtmosphereLutReferenceTest, ASunBelowTheHorizonIsShadowedByThePlanet)
{
    double sun[3];
    SunDirection(-2.0, sun);
    const std::vector<Rgb> lut = RunSkyView(sun, false);
    for (const uint32_t row : {kSkyViewSize / 2u, SkyViewRowAtElevation(10.0)})
    {
        double dir[3];
        SkyViewTexelDirection(m_Atmo, kSkyViewSize / 2u, row, dir);
        const Rgb expected = SkyRadiance(m_Atmo, dir, sun, 4096, false);
        const Rgb& actual = lut[static_cast<size_t>(row) * kSkyViewSize + kSkyViewSize / 2u];
        for (int c = 0; c < 3; ++c)
            EXPECT_NEAR(actual[c], expected[c], expected[c] * 0.03 + 1e-6)
                << "row " << row << " channel " << c << ": LUT " << Describe(actual) << ", integral "
                << Describe(expected);
    }
}

// The whole chain at the horizon and 10 degrees up, with the multiscatter LUT the pass reads: what
// the sky draws there for the Basic 3D template's 10 h sun. Before the model was fixed this was 2 to
// 5 times the integral and blue where the integral is near-white. Under the default crisp day's
// aerosol, with the ground's bounce at albedo 0.3, the worst channel is 1.1 % off, 10 degrees up
// (2.1 % under a typical day's, optical depth 0.15).
// Without the multiscatter LUT's
// half-texel inset its lowest row sits 1.6 km up, the near-ground samples of the horizon ray read
// the multiple scattering of thinner air, and the horizon's blue comes out 5.7 % over the integral
// (17 % under a hazy day's aerosol).
TEST_F(SkyAtmosphereLutReferenceTest, TheSkyViewRadianceMatchesTheIntegralWithMultipleScattering)
{
    double sun[3];
    SunDirection(41.6, sun);
    const std::vector<Rgb> lut = RunSkyView(sun, true);
    for (const uint32_t row : {kSkyViewSize / 2u, SkyViewRowAtElevation(10.0)})
    {
        double dir[3];
        SkyViewTexelDirection(m_Atmo, kSkyViewSize / 2u, row, dir);
        const Rgb expected = SkyRadiance(m_Atmo, dir, sun, 4096, true);
        const Rgb& actual = lut[static_cast<size_t>(row) * kSkyViewSize + kSkyViewSize / 2u];
        EXPECT_LT(WorstRatioError(actual, expected), 0.03)
            << "row " << row << ": LUT " << Describe(actual) << ", integral " << Describe(expected);
    }
}

// The moon lights the night sky as the sun lights the day, from its own direction through the same
// transmittance, phases and multiple scattering, in the moon's colour (SkySystemConfig::moonColor,
// sunlight off a surface redder than grey). The sun 30 degrees under contributes nothing, so the night sky toward the
// moon at the horizon and 10 degrees up is the moon's integral alone.
TEST_F(SkyAtmosphereLutReferenceTest, TheMoonLightsTheNightSkyFromItsOwnDirection)
{
    double sun[3];
    SunDirection(-30.0, sun);
    const double moonElevation = 40.0 * kPi / 180.0;
    const double moon[3] = {-std::cos(moonElevation), std::sin(moonElevation), 0.0};
    const Rendering::SkySystemConfig config{};
    const float moonColor[3] = {config.moonColor.r, config.moonColor.g, config.moonColor.b};
    const std::vector<Rgb> lut = RunSkyView(sun, true, kCameraHeight, 1.0f, moon);
    for (const uint32_t row : {kSkyViewSize / 2u, SkyViewRowAtElevation(10.0)})
    {
        double dir[3];
        SkyViewTexelDirection(m_Atmo, 0u, row, dir);
        Rgb expected = SkyRadiance(m_Atmo, dir, moon, 4096, true);
        for (int c = 0; c < 3; ++c)
            expected[c] *= moonColor[c];
        const Rgb& actual = lut[static_cast<size_t>(row) * kSkyViewSize];
        EXPECT_LT(WorstRatioError(actual, expected), 0.03)
            << "row " << row << ": LUT " << Describe(actual) << ", the moon's integral " << Describe(expected);
    }
}

// In the twilight band both bodies light the sky in the same bake: the sun 6 degrees under and the
// moon 25 degrees up on the opposite side. Scattering is linear in each source, so the two-source
// bake is the sum of the sun's bake and the moon's bake at every texel; the moon's share must be a
// real part of the sky there, or the sum would hold for a bake that drops the moon.
TEST_F(SkyAtmosphereLutReferenceTest, InTheTwilightBandTheSkyIsTheSumOfTheSunsAndTheMoonsLight)
{
    double sun[3];
    SunDirection(-6.0, sun);
    const double moonElevation = 25.0 * kPi / 180.0;
    const double moon[3] = {std::cos(moonElevation), std::sin(moonElevation), 0.0};
    const std::vector<Rgb> both = RunSkyView(sun, true, kCameraHeight, 1.0f, moon);
    const std::vector<Rgb> sunOnly = RunSkyView(sun, true, kCameraHeight, 1.0f);
    const std::vector<Rgb> moonOnly = RunSkyView(sun, true, kCameraHeight, 0.0f, moon);

    double worst = 0.0;
    size_t worstTexel = 0;
    for (size_t texel = 0; texel < both.size(); ++texel)
        for (int c = 0; c < 3; ++c)
        {
            const double sum = sunOnly[texel][c] + moonOnly[texel][c];
            const double error = std::abs(both[texel][c] - sum) / std::max(sum, 1e-30);
            if (error > worst)
            {
                worst = error;
                worstTexel = texel;
            }
        }
    const size_t zenith = static_cast<size_t>(SkyViewRowAtElevation(89.0)) * kSkyViewSize;
    const double moonShareAtZenith = moonOnly[zenith][1] / std::max(both[zenith][1], 1e-30);
    EXPECT_LT(worst, 1e-4) << "texel " << worstTexel << ": both " << Describe(both[worstTexel]) << ", sun "
                           << Describe(sunOnly[worstTexel]) << " + moon " << Describe(moonOnly[worstTexel]);
    EXPECT_GT(moonShareAtZenith, 0.05) << "the moon's share of the zenith's green, " << moonShareAtZenith
                                       << ", is too small for the sum to say anything about the moon";
}

// A camera 30 km up sees the sky down to the planet's horizon, 5.6 degrees below the horizontal:
// 3 degrees down, the view ray grazes the air 21 km up. A bake that folds every texel below the
// horizontal up to it reads the thin air above the camera there instead, 0.26x to 0.31x this.
TEST_F(SkyAtmosphereLutReferenceTest, AtAltitudeTheSkyViewReachesDownToThePlanetsHorizon)
{
    constexpr double kAltitude = 30000.0;
    double sun[3];
    SunDirection(41.6, sun);
    const std::vector<Rgb> lut = RunSkyView(sun, false, kAltitude);
    // The row whose texel centre sits nearest 3 degrees below the horizontal (the inverse of the
    // sky-view LUT's elevation warp).
    const double s = -std::sqrt(2.0 * 3.0 / 180.0);
    const uint32_t row = static_cast<uint32_t>(std::lround(0.5 * (s + 1.0) * kSkyViewSize - 0.5));
    for (const uint32_t column : {kSkyViewSize / 2u, 0u})
    {
        double dir[3];
        SkyViewTexelDirection(m_Atmo, column, row, dir, kAltitude);
        ASSERT_LT(dir[1], -0.05) << "row " << row << " is not below the horizontal";
        const Rgb expected = SkyRadiance(m_Atmo, dir, sun, 4096, false, kAltitude);
        const Rgb& actual = lut[static_cast<size_t>(row) * kSkyViewSize + column];
        EXPECT_LT(WorstRatioError(actual, expected), 0.03)
            << "column " << column << " (toward the sun is " << kSkyViewSize / 2u << "): LUT " << Describe(actual)
            << ", integral " << Describe(expected);
    }
}

// Every texel below the horizon stores its own azimuth's horizon, down to the nadir: on the ground
// and at 30 km the bottom row equals the row just below the planet's horizon. A fold that raised y
// and renormalised turned the texels near the nadir, whose horizontal part is tiny, toward the
// zenith on the ground and toward the nadir at altitude.
TEST_F(SkyAtmosphereLutReferenceTest, TheTexelsNearTheNadirHoldTheirAzimuthsHorizon)
{
    double sun[3];
    SunDirection(41.6, sun);
    for (const double altitude : {kCameraHeight, 30000.0})
    {
        const std::vector<Rgb> lut = RunSkyView(sun, false, altitude);
        // 6 degrees below the horizontal: below the planet's horizon at both altitudes.
        const double s = -std::sqrt(2.0 * 6.0 / 180.0);
        const uint32_t foldedRow = static_cast<uint32_t>(std::lround(0.5 * (s + 1.0) * kSkyViewSize - 0.5));
        for (const uint32_t column : {kSkyViewSize / 2u, kSkyViewSize / 4u})
        {
            const Rgb& nadir = lut[column];
            const Rgb& folded = lut[static_cast<size_t>(foldedRow) * kSkyViewSize + column];
            EXPECT_LT(WorstRatioError(nadir, folded), 0.01)
                << "altitude " << altitude << " m, column " << column << ": bottom row " << Describe(nadir)
                << ", row " << foldedRow << " " << Describe(folded);
        }
    }
}

// The sky-view march reads the multiscatter LUT at the texel the bake wrote for its height and sun:
// with a LUT whose every texel holds its own column and row, a read at the parameters a texel
// centre stands for (the ground row at height 0, the top row at the top) returns that texel
// exactly. A reader that maps the height straight onto [0, 1] lands between rows: on the ground it
// reads the multiple scattering baked for 1.6 km up.
TEST_F(SkyAtmosphereLutReferenceTest, TheSkyViewReadsTheMultiscatterTexelTheBakeWrote)
{
    // rgba16f like the real LUT: always filterable, and it holds the indices 0 to 31 exactly.
    const size_t texels = static_cast<size_t>(kMultiscatterSize) * kMultiscatterSize;
    std::vector<uint16_t> values(texels * 4u);
    for (uint32_t y = 0; y < kMultiscatterSize; ++y)
        for (uint32_t x = 0; x < kMultiscatterSize; ++x)
        {
            uint16_t* t = values.data() + (static_cast<size_t>(y) * kMultiscatterSize + x) * 4u;
            t[0] = HalfOfSmallInteger(x);
            t[1] = HalfOfSmallInteger(y);
            t[2] = HalfOfSmallInteger(0u);
            t[3] = HalfOfSmallInteger(1u);
        }
    const TextureHandle indexed = MakeLut(kMultiscatterSize, kMultiscatterSize, "IndexedMultiscatter");
    const BufferHandle upload = MakeBuffer(values.size() * sizeof(uint16_t),
                                           static_cast<uint32_t>(BufferUsage::TransferSrc), BufferMemoryUsage::Upload,
                                           "IndexedUpload");
    Upload(upload, values.data(), values.size() * sizeof(uint16_t));
    Submit([&](CommandList& cl) {
        cl.Barrier(ResourceBarrier::CreateTextureBarrier(indexed, ResourceState::Undefined, ResourceState::CopyDest));
        cl.CopyBufferToTextureSubresource(upload, indexed, 0u, 0u, kMultiscatterSize, kMultiscatterSize, 0u);
        cl.Barrier(ResourceBarrier::CreateTextureBarrier(indexed, ResourceState::CopyDest, ResourceState::ShaderResource));
    });

    std::vector<std::array<double, 2>> queries;
    std::vector<std::array<uint32_t, 2>> want;
    for (const uint32_t y : {0u, 1u, 7u, 16u, kMultiscatterSize - 1u})
        for (const uint32_t x : {0u, 13u, kMultiscatterSize - 1u})
        {
            const double sunCos = static_cast<double>(x) / (kMultiscatterSize - 1u) * 2.0 - 1.0;
            const double height = static_cast<double>(y) / (kMultiscatterSize - 1u);
            queries.push_back({sunCos, height});
            want.push_back({x, y});
        }
    const float pc[4] = {static_cast<float>(queries.size()), 0.0f, 0.0f, 0.0f};
    const std::vector<Rgb> got = RunLookupProbe(m_MsLookupPipe, m_MsLookupLayout, indexed, queries, pc);
    ASSERT_EQ(got.size(), queries.size());
    for (size_t i = 0; i < got.size(); ++i)
    {
        EXPECT_NEAR(got[i][0], want[i][0], 0.01) << "sun cosine " << queries[i][0] << ": read column " << got[i][0];
        EXPECT_NEAR(got[i][1], want[i][1], 0.01)
            << "height " << queries[i][1] << " of the shell: read row " << got[i][1] << ", the bake wrote row "
            << want[i][1] << " there";
    }
}

// A moonless night sky is about 1.7e-4 cd/m2 (Leinert et al. 1998): 8.4e-7 scene-linear on the
// 203-nit anchor, below binary16's smallest normal, 6.1e-5. The sky-view march is linear in the
// sun's intensity, so a sun dimmed until the zenith texel holds that level makes the real pass
// write a night radiance into the LUT. Every texel must then read back as the day sky times the
// dimming, within 1 %. A binary16 LUT keeps such values only as subnormals, 6e-8 apart (about 7 %
// of the zenith's level), or flushes them to 0.
TEST_F(SkyAtmosphereLutReferenceTest, ANightRadianceBelowTheHalfFloatFloorReadsBackWithinOnePercent)
{
    constexpr double kNightZenithRadiance = 1.7e-4 / 203.0;
    constexpr double kHalfSmallestNormal = 6.103515625e-5;

    double sun[3];
    SunDirection(41.6, sun);
    const std::vector<Rgb> day = RunSkyView(sun, true);
    const Rgb& dayZenith = day[static_cast<size_t>(kSkyViewSize - 1u) * kSkyViewSize + kSkyViewSize / 2u];
    const double dimming = kNightZenithRadiance / std::max({dayZenith[0], dayZenith[1], dayZenith[2]});

    double brightest = 0.0;
    for (const Rgb& texel : day)
        brightest = std::max({brightest, texel[0], texel[1], texel[2]});
    ASSERT_LT(brightest * dimming, kHalfSmallestNormal) << "the dimmed sky must lie wholly below the binary16 floor";

    const std::vector<Rgb> night = RunSkyView(sun, true, kCameraHeight, static_cast<float>(dimming));
    double worst = 0.0;
    size_t worstTexel = 0u;
    size_t outside = 0u;
    for (size_t i = 0; i < day.size(); ++i)
    {
        if (day[i][0] <= 0.0 || day[i][1] <= 0.0 || day[i][2] <= 0.0)
            continue;
        const Rgb expected{day[i][0] * dimming, day[i][1] * dimming, day[i][2] * dimming};
        const double error = WorstRatioError(night[i], expected);
        if (error > 0.01)
            ++outside;
        if (error > worst)
        {
            worst = error;
            worstTexel = i;
        }
    }
    const Rgb& zenith = night[static_cast<size_t>(kSkyViewSize - 1u) * kSkyViewSize + kSkyViewSize / 2u];
    EXPECT_EQ(outside, 0u) << outside << " of " << day.size() << " texels are more than 1 % off; the worst, texel "
                           << worstTexel << ", is " << worst * 100.0 << " % off: read " << Describe(night[worstTexel])
                           << ". The zenith reads " << Describe(zenith) << " for " << kNightZenithRadiance;
}
