// GPU-executed gate for the transmittance LUT's (r, mu) mapping and its ray-sphere path length.
//
// The LUT bakes exp(-tau) out to the top of the atmosphere over the rays that do not meet the
// planet (Bruneton's mapping, Includes/sky_transmittance_lut.glsl). Both the exit distance and the
// mapping are cancellation-prone: at planet scale r*r and sphereRadius*sphereRadius are ~4e13, so
// the difference of the two squares keeps no float32 precision, and a compiler free to contract
// one product into an FMA leaves a residual of ~1e6 m^2 where the exact answer is 0. That is not
// a hypothetical: it is what made the CPU sun colour resolve a 222-2165 km chord as a 0.25-2.97 m
// near root on arm64, so a set sun was never extinguished.
//
// The shader factors every such term as (a - b) * (a + b) and takes each root in its
// non-cancelling direction. This test checks that on a REAL device, because the compiler that
// decides whether to contract is the driver's -- MoltenVK hands SPIR-V to Metal, which recompiles
// it. A CPU simulation of the arithmetic is not the arithmetic that ships.
//
// The LUT's own texels cannot serve as the instrument: it stores rgba16f, and near the surface a
// 5% path-length error moves a transmittance of ~1 by less than one fp16 ULP. So the probe reports
// the (r, mu) each texel bakes, its path length and the forward mapping's round trip, and this
// compares them against double-precision references.
//
// Skips ONLY without a Vulkan device. A missing staged probe shader FAILS instead: the same target
// builds it, so its absence is a build defect, and an all-skip suite that reads as green is the
// exact failure mode this area has been burned by.

#include <gtest/gtest.h>

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include "Platform/Shell.h"

#include "TestDeviceHelper.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Mirrors sky_transmittance_path_probe.comp's ProbePC.
struct ProbePC
{
    float radiiAndRow[4];
    float columns[4];
};

// SkyRenderer::FillDefaultAtmosphere's Earth-scale shell, which is the scale the cancellation
// needs: at R = 6.36e6 the exact difference of the squares is what a contracted FMA destroys.
constexpr double kPlanetRadius = 6360000.0;
constexpr double kAtmosphereRadius = 6460000.0;

// SkyRenderer's transmittance LUT, probed texel by texel.
constexpr uint32_t kColumns = 256u;
constexpr uint32_t kRows = 64u;

// The reference is the REPLACED general quadratic, evaluated in double: nearest positive root of
// t^2 + 2*r*mu*t + (r^2 - sphereRadius^2), zero when there is none.
//
// Two things follow from that choice, and both are the point. It is independent of the shader's new
// expression -- a reference written in the new form would replicate an algebraic branch error along
// with it -- and it is literally the contract this change claims: the closed forms must agree with
// the quadratic everywhere the quadratic was correct, and the quadratic is correct exactly when it
// is not asked to cancel. In double it never is: r^2 is ~4e13 with an ULP of ~0.008, so the
// difference of the squares keeps every metre the fp32 version loses.
double ReferenceNearestPositiveRoot(double r, double mu, double sphereRadius)
{
    const double b = 2.0 * r * mu;
    const double c = (r - sphereRadius) * (r + sphereRadius);
    const double disc = b * b - 4.0 * c;
    if (disc <= 0.0)
        return 0.0;
    const double sq = std::sqrt(disc);
    const double t0 = 0.5 * (-b - sq);
    const double t1 = 0.5 * (-b + sq);
    if (t0 > 0.0)
        return t0;
    return (t1 > 0.0) ? t1 : 0.0;
}

std::filesystem::path FindProbeSpv()
{
    namespace fs = std::filesystem;
    const fs::path exe = GameEngine::Platform::GetExecutablePath();
    fs::path dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int up = 0; up < 6; ++up)
    {
        const fs::path candidate = dir / "Shaders" / "sky_transmittance_path_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

class SkyTransmittanceLutParityTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty())
            << "sky_transmittance_path_probe.comp.spv was not found above the test executable ("
            << GameEngine::Platform::GetExecutablePath().string()
            << ") -- the shader target stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty()) << "staged probe SPIR-V is empty: " << spvPath.string();

        DescriptorBinding out{};
        out.binding = 0;
        out.type = DescriptorType::StorageBuffer;
        out.count = 1;
        out.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "SkyTransmittancePathProbe.Set0";
        m_Layout.bindings = {out};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(ProbePC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "SkyTransmittancePathProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        if (m_Device)
            m_Device->Shutdown();
    }

    // Returns kColumns x (sAtmosphere, round-trip u error, mu, r) for one LUT row.
    std::vector<float> Probe(uint32_t row)
    {
        ProbePC pc{};
        pc.radiiAndRow[0] = static_cast<float>(kPlanetRadius);
        pc.radiiAndRow[1] = static_cast<float>(kAtmosphereRadius);
        pc.radiiAndRow[2] = static_cast<float>(row);
        pc.radiiAndRow[3] = static_cast<float>(kRows);
        pc.columns[0] = static_cast<float>(kColumns);

        const size_t bytes = static_cast<size_t>(kColumns) * 4u * sizeof(float);
        BufferDesc od{};
        od.size = bytes;
        od.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                   static_cast<uint32_t>(BufferUsage::TransferSrc) |
                   static_cast<uint32_t>(BufferUsage::TransferDst);
        od.memoryUsage = BufferMemoryUsage::Readback;
        od.flags = BufferCreateFlags::PersistentlyMapped;
        od.debugName = "SkyTransmittancePathProbe.Out";
        BufferHandle outBuf = m_Device->CreateBuffer(od);
        m_Buffers.push_back(outBuf);
        const std::vector<float> poison(static_cast<size_t>(kColumns) * 4u, -1.0f);
        m_Device->UpdateBuffer(outBuf, 0, bytes, poison.data());

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "SkyTransmittancePathProbe.DS";
        DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        m_Device->UpdateStorageBufferBinding(ds, 0, outBuf, 0, bytes);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->SetPushConstants(pc);
        cl->Dispatch((kColumns + 63u) / 64u, 1u, 1u);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();

        std::vector<float> out(static_cast<size_t>(kColumns) * 4u);
        void* mapped = m_Device->MapBuffer(outBuf);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(out.data(), mapped, bytes);
        m_Device->UnmapBuffer(outBuf);
        return out;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};

// float32 relative bound on the exit distance. The replaced quadratic misses the shell exit by
// 1.2e-4 at planet scale; the closed form clears this bound by 400x, decided by that measured gap.
constexpr double kRelativeTolerance = 1e-4;

// A reader asking for a texel's own (r, mu) must land on that texel: the round trip of the
// mapping may drift by a hundredth of a texel at most.
constexpr double kRoundTripTexels = 0.01;

// The lowest altitude a texel's ray reaches, in double: r itself for an upward ray, the
// perigee r * sqrt(1 - mu^2) above the planet for a downward one.
double LowestAltitude(double r, double mu)
{
    return (mu >= 0.0 ? r : r * std::sqrt(1.0 - mu * mu)) - kPlanetRadius;
}

// The grazing column is tangent to the planet, so its perigee is 0 up to float32 at planet scale
// (one ULP of 6.36e6 is 0.5 m). A ray that enters the planet goes kilometres deep.
constexpr double kTangentToleranceMetres = 1.0;

} // namespace

// The LUT holds only rays that leave the atmosphere: a ray that meets the planet is occluded, and
// a texel that bakes one puts a discontinuity on the horizon -- a bilinear read there mixing a
// grazing path with a short path to the ground (0.59/0.50/0.47 against the exact
// 0.156/0.017/0.0001 at 3 m).
TEST_F(SkyTransmittanceLutParityTest, NoLutTexelBakesARayThatMeetsThePlanet)
{
    for (uint32_t row = 0; row < kRows; ++row)
    {
        const std::vector<float> got = Probe(row);
        ASSERT_EQ(got.size(), static_cast<size_t>(kColumns) * 4u);
        ASSERT_NE(got[0], -1.0f) << "the probe never wrote: the dispatch did not reach the buffer";
        for (uint32_t i = 0; i < kColumns; ++i)
        {
            const double mu = static_cast<double>(got[i * 4u + 2]);
            const double r = static_cast<double>(got[i * 4u + 3]);
            ASSERT_GE(LowestAltitude(r, mu), -kTangentToleranceMetres)
                << "texel (" << i << ", " << row << ") bakes r - R = " << (r - kPlanetRadius)
                << " m, mu " << mu << ": a ray into the planet";
        }
    }
}

// The distance out through the shell, which every texel integrates over. Its cancelling direction
// is an outward ray, which subtracts near-equal terms.
TEST_F(SkyTransmittanceLutParityTest, AtmosphereExitIsExactOnEveryLutTexel)
{
    double worstRel = 0.0;
    for (uint32_t row = 0; row < kRows; ++row)
    {
        const std::vector<float> got = Probe(row);
        ASSERT_EQ(got.size(), static_cast<size_t>(kColumns) * 4u);

        for (uint32_t i = 0; i < kColumns; ++i)
        {
            const double mu = static_cast<double>(got[i * 4u + 2]);
            const double expected =
                ReferenceNearestPositiveRoot(static_cast<double>(got[i * 4u + 3]), mu, kAtmosphereRadius);
            const double actual = static_cast<double>(got[i * 4u + 0]);
            if (expected <= 0.0)
            {
                // The top row looking outward stands on the boundary: there is no shell left.
                EXPECT_LE(std::fabs(actual), 1.0) << "texel (" << i << ", " << row << ") mu " << mu;
                continue;
            }
            const double rel = std::fabs(actual - expected) / expected;
            worstRel = std::max(worstRel, rel);
            if (rel > kRelativeTolerance)
            {
                ADD_FAILURE() << "texel (" << i << ", " << row << ") mu " << mu << ": shell exit "
                              << actual << " m, exact " << expected << " m, relative error " << rel;
                row = kRows;
                break;
            }
        }
    }
    RecordProperty("worstShellExitRelativeError", std::to_string(worstRel));
}

// The forward mapping of each texel's decoded (r, mu) returns the texel: the readers and the
// baking pass agree on where a ray is stored, the grazing column included.
TEST_F(SkyTransmittanceLutParityTest, TheMappingRoundTripsOnEveryLutTexel)
{
    double worstTexels = 0.0;
    for (uint32_t row = 0; row < kRows; ++row)
    {
        const std::vector<float> got = Probe(row);
        ASSERT_EQ(got.size(), static_cast<size_t>(kColumns) * 4u);
        for (uint32_t i = 0; i < kColumns; ++i)
        {
            const double texels = std::fabs(static_cast<double>(got[i * 4u + 1])) * kColumns;
            worstTexels = std::max(worstTexels, texels);
            ASSERT_LE(texels, kRoundTripTexels)
                << "texel (" << i << ", " << row << "): the forward mapping lands " << texels
                << " texels away";
        }
    }
    RecordProperty("worstRoundTripTexels", std::to_string(worstTexels));
}
