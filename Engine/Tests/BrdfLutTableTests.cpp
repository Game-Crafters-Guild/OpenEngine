// The split-sum BRDF table that every image-based specular reads (brdf_lut.comp), built on the
// device from the staged shader and read back. Texel (x, y) holds the scale and bias on F0 for
// NdotV = x / (size - 1) and roughness = y / (size - 1). Four checks:
//
//   * At seven texels, the first column among them, the table equals the same 1024-sample
//     estimate computed on the CPU (the same Hammersley half vectors and frame, in double
//     precision), within the half-float storage error. This checks the build; the fit check
//     below is the independent one.
//   * At six texels it matches the analytic fit in standard_pbr.glsl (GE_EnvBRDFApprox). The fit
//     approximates the table: at these texels it differs from the table by at most 0.035,
//     elsewhere by up to 0.18, so these texels are the ones where it is a fair check.
//   * A dielectric's (F0 = 0.04) reflectance rises toward grazing, the defining shape of the
//     split-sum: several times larger at NdotV 0.1 than facing the viewer.
//   * Every texel is finite and scale + bias is at most 1: a surface never reflects more light
//     than reaches it.

#include "Mathematics/HalfFloat.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "TestDeviceHelper.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numbers>
#include <vector>

using namespace GameEngine::Rendering;
using GameEngine::Mathematics::HalfToFloat;

namespace
{
// The table's edge length in the engine (ImageBasedLightingFeature::kBrdfLutSize).
constexpr uint32_t kTableSize = 512;
// brdf_lut.comp's local_size_x and local_size_y.
constexpr uint32_t kGroupSize = 16;
// brdf_lut.comp's SAMPLE_COUNT.
constexpr uint32_t kSampleCount = 1024;
// The table against the CPU estimate of the same sum: half-float storage rounds a value near 1
// by up to 2.4e-4, and float accumulation of 1024 terms adds about 1e-5.
constexpr double kSameIntegralTolerance = 2e-3;
// How far scale + bias may exceed 1: half-float storage rounds a value near 1 by up to 2.4e-4.
// Beyond that the table reflects more light than reaches the surface.
constexpr double kEnergyTolerance = 2e-3;
// The table against the analytic fit at the six fit texels below (the fit's own deviation there
// is at most 0.035).
constexpr double kFitTolerance = 0.05;

struct ScaleBias
{
    double Scale = 0.0;
    double Bias = 0.0;
};

struct Texel
{
    uint32_t X = 0;
    uint32_t Y = 0;
    double NdotV() const { return static_cast<double>(X) / (kTableSize - 1); }
    double Roughness() const { return static_cast<double>(Y) / (kTableSize - 1); }
};

Texel NearestTexel(double ndotV, double roughness)
{
    return {static_cast<uint32_t>(std::lround(ndotV * (kTableSize - 1))),
            static_cast<uint32_t>(std::lround(roughness * (kTableSize - 1)))};
}

double RadicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<double>(bits) * 2.3283064365386963e-10;
}

// The image-based Schlick-Smith constant k = a / 2, a = roughness^2.
double MaskingConstant(double roughness)
{
    return roughness * roughness / 2.0;
}

// Schlick-Smith masking for one direction.
double MaskingSchlick(double cosine, double roughness)
{
    const double k = MaskingConstant(roughness);
    return cosine / (cosine * (1.0 - k) + k);
}

// The split-sum integral for a +Z normal and V in the XZ plane, estimated with the shader's GGX
// half-vector samples. A sample's pdf over L is D * NdotH / (4 * VdotH), so it weighs
// G * VdotH / (NdotH * NdotV), evaluated with the view's cosine cancelled: G1(NdotV) / NdotV is
// 1 / (NdotV * (1 - k) + k), which stays finite at NdotV = 0.
ScaleBias IntegrateSplitSum(double ndotV, double roughness)
{
    const double vx = std::sqrt(1.0 - ndotV * ndotV);
    const double vz = ndotV;
    const double alpha = roughness * roughness;
    ScaleBias sum;
    for (uint32_t i = 0; i < kSampleCount; ++i)
    {
        const double phi = 2.0 * std::numbers::pi * static_cast<double>(i) / kSampleCount;
        const double u = RadicalInverse(i);
        const double cosTheta = std::sqrt((1.0 - u) / (1.0 + (alpha * alpha - 1.0) * u));
        const double sinTheta = std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta));
        // The shader's tangent frame for N = +Z is T = (0, -1, 0), B = (1, 0, 0).
        const double hx = std::sin(phi) * sinTheta;
        const double hz = cosTheta;
        const double vDotH = vx * hx + vz * hz;
        const double nDotL = 2.0 * vDotH * hz - vz;
        if (nDotL <= 0.0)
            continue;
        const double clampedVdotH = std::max(vDotH, 0.0);
        const double k = MaskingConstant(roughness);
        const double viewMaskingOverCosine = 1.0 / std::max(ndotV * (1.0 - k) + k, 1e-4);
        const double weight =
            MaskingSchlick(nDotL, roughness) * viewMaskingOverCosine * clampedVdotH / std::max(hz, 1e-4);
        const double fresnel = std::pow(1.0 - clampedVdotH, 5.0);
        sum.Scale += (1.0 - fresnel) * weight;
        sum.Bias += fresnel * weight;
    }
    return {sum.Scale / kSampleCount, sum.Bias / kSampleCount};
}

// GE_EnvBRDFApprox in Shaders/Includes/standard_pbr.glsl.
ScaleBias AnalyticFit(double ndotV, double roughness)
{
    const std::array<double, 4> c0{-1.0, -0.0275, -0.572, 0.022};
    const std::array<double, 4> c1{1.0, 0.0425, 1.04, -0.04};
    std::array<double, 4> r{};
    for (size_t i = 0; i < r.size(); ++i)
        r[i] = roughness * c0[i] + c1[i];
    const double a004 = std::min(r[0] * r[0], std::exp2(-9.28 * ndotV)) * r[0] + r[1];
    return {-1.04 * a004 + r[2], 1.04 * a004 + r[3]};
}

class BrdfLutTableTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        BuildAndReadTable();
    }

    void TearDown() override
    {
        if (m_Device)
            m_Device->Shutdown();
    }

    ScaleBias Read(const Texel& texel) const
    {
        const size_t index = (static_cast<size_t>(texel.Y) * kTableSize + texel.X) * 2u;
        return {HalfToFloat(m_Halves[index]), HalfToFloat(m_Halves[index + 1])};
    }

    std::unique_ptr<IDevice> m_Device;
    std::vector<uint16_t> m_Halves;

  private:
    // Dispatches the staged brdf_lut.comp over a kTableSize RG16F image and copies it back.
    void BuildAndReadTable()
    {
        TextureDesc tableDesc{};
        tableDesc.width = kTableSize;
        tableDesc.height = kTableSize;
        tableDesc.format = static_cast<uint32_t>(TextureFormat::R16G16_FLOAT);
        tableDesc.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess) |
                          static_cast<uint32_t>(TextureUsage::TransferSrc);
        tableDesc.debugName = "BrdfLutTableTest.Table";
        const TextureHandle table = m_Device->CreateTexture(tableDesc);
        ASSERT_TRUE(table.IsValid());

        const std::vector<uint8_t> shader = Utils::LoadShaderFile("Shaders/brdf_lut.comp.spv");
        ASSERT_FALSE(shader.empty()) << "Shaders/brdf_lut.comp.spv is not staged";

        DescriptorSetLayoutDesc layout{};
        DescriptorBinding binding{};
        binding.binding = 0;
        binding.type = DescriptorType::StorageImage;
        binding.count = 1;
        binding.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(binding);
        layout.debugName = "BrdfLutTableTest.Set0";

        ComputePipelineDesc pipelineDesc{};
        pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(shader);
        pipelineDesc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
        pipelineDesc.DebugName = "BrdfLutTableTest.Bake";
        const PipelineHandle pipeline =
            m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipelineDesc));
        ASSERT_TRUE(pipeline.IsValid());

        DescriptorSetDesc setDesc{};
        setDesc.layout = layout;
        setDesc.transient = true;
        setDesc.debugName = "BrdfLutTableTest.Set0";
        const DescriptorSetHandle set = m_Device->CreateDescriptorSet(setDesc);
        ASSERT_TRUE(set.IsValid());
        m_Device->UpdateStorageImageBinding(set, 0, table);

        const uint32_t readbackBytes = kTableSize * kTableSize * 2u * sizeof(uint16_t);
        const BufferHandle readback = m_Device->CreateReadbackBuffer(readbackBytes);
        ASSERT_TRUE(readback.IsValid());

        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_TRUE(commands);
        commands->Begin();
        commands->Barrier(
            ResourceBarrier::CreateTextureBarrier(table, ResourceState::Undefined, ResourceState::UnorderedAccess));
        commands->SetPipeline(pipeline);
        commands->BindDescriptorSet(0, set, pipeline);
        const uint32_t groups = (kTableSize + kGroupSize - 1) / kGroupSize;
        commands->Dispatch(groups, groups, 1);
        commands->Barrier(
            ResourceBarrier::CreateTextureBarrier(table, ResourceState::UnorderedAccess, ResourceState::CopySource));
        commands->CopyTextureToBuffer(table, readback, kTableSize, kTableSize);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();

        const auto* mapped = static_cast<const uint8_t*>(m_Device->MapBuffer(readback));
        ASSERT_NE(mapped, nullptr);
        m_Halves.resize(readbackBytes / sizeof(uint16_t));
        std::memcpy(m_Halves.data(), mapped, readbackBytes);
        m_Device->UnmapBuffer(readback);

        m_Device->DestroyBuffer(readback);
        m_Device->DestroyTexture(table);
    }
};
} // namespace

TEST_F(BrdfLutTableTest, EqualsTheSameEstimateOnTheCpu)
{
    // The first column (NdotV = 0, the grazing limit), grazing to facing the viewer, smooth to rough.
    const std::array<Texel, 7> texels{NearestTexel(0.0, 0.3),  NearestTexel(0.05, 0.1), NearestTexel(0.1, 0.3),
                                      NearestTexel(0.5, 0.5),  NearestTexel(1.0, 0.5),  NearestTexel(0.3, 0.9),
                                      NearestTexel(0.9, 0.2)};
    for (const Texel& texel : texels)
    {
        const ScaleBias table = Read(texel);
        const ScaleBias expected = IntegrateSplitSum(texel.NdotV(), texel.Roughness());
        EXPECT_NEAR(table.Scale, expected.Scale, kSameIntegralTolerance)
            << "scale at NdotV " << texel.NdotV() << ", roughness " << texel.Roughness();
        EXPECT_NEAR(table.Bias, expected.Bias, kSameIntegralTolerance)
            << "bias at NdotV " << texel.NdotV() << ", roughness " << texel.Roughness();
    }
}

TEST_F(BrdfLutTableTest, MatchesTheAnalyticFitWhereTheFitIsAccurate)
{
    const std::array<Texel, 6> texels{NearestTexel(0.05, 0.5), NearestTexel(0.1, 0.5), NearestTexel(0.3, 0.3),
                                      NearestTexel(0.3, 0.7),  NearestTexel(0.5, 0.5), NearestTexel(0.7, 0.7)};
    for (const Texel& texel : texels)
    {
        const ScaleBias table = Read(texel);
        const ScaleBias fit = AnalyticFit(texel.NdotV(), texel.Roughness());
        EXPECT_NEAR(table.Scale, fit.Scale, kFitTolerance)
            << "scale at NdotV " << texel.NdotV() << ", roughness " << texel.Roughness();
        EXPECT_NEAR(table.Bias, fit.Bias, kFitTolerance)
            << "bias at NdotV " << texel.NdotV() << ", roughness " << texel.Roughness();
    }
}

TEST_F(BrdfLutTableTest, ADielectricReflectsMoreTowardGrazing)
{
    constexpr double kDielectricF0 = 0.04;
    const ScaleBias grazing = Read(NearestTexel(0.1, 0.3));
    const ScaleBias facing = Read(NearestTexel(1.0, 0.3));
    const double grazingReflectance = kDielectricF0 * grazing.Scale + grazing.Bias;
    const double facingReflectance = kDielectricF0 * facing.Scale + facing.Bias;
    EXPECT_NEAR(facingReflectance, kDielectricF0, 0.005) << "facing the viewer the reflectance is about F0";
    EXPECT_GT(grazingReflectance, 3.0 * facingReflectance)
        << "at NdotV 0.1 the reflectance rises toward 1 (grazing " << grazingReflectance << ", facing "
        << facingReflectance << ")";
}

TEST_F(BrdfLutTableTest, EveryTexelIsFiniteAndReflectsAtMostTheLight)
{
    uint32_t failures = 0;
    for (uint32_t y = 0; y < kTableSize; ++y)
    {
        for (uint32_t x = 0; x < kTableSize; ++x)
        {
            const ScaleBias table = Read(Texel{x, y});
            const bool finite = std::isfinite(table.Scale) && std::isfinite(table.Bias);
            if (finite && table.Scale + table.Bias <= 1.0 + kEnergyTolerance)
                continue;
            if (failures++ < 8)
                ADD_FAILURE() << "texel (" << x << ", " << y << "): scale " << table.Scale << ", bias " << table.Bias;
        }
    }
    EXPECT_EQ(failures, 0u) << "texels that are not finite or reflect more than 1";
}
