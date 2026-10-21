// The view's exposure compensation state as its readers see it (Includes/exposure_compensation.glsl),
// evaluated on the device by exposure_compensation_probe.comp: the factor the star pass undoes the
// curve by, and the metered reading the night tint gates on.
//
// Skips ONLY without a Vulkan device. A missing staged probe FAILS: the same target builds it.

#include <gtest/gtest.h>

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include "Platform/Shell.h"

#include "TestDeviceHelper.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// The 16-byte block the resolve writes (exposure_compensation.glsl).
struct CompensationState
{
    float Stops = 0.0f;
    uint32_t Valid = 0u;
    float MeteredLogLuminance = 0.0f;
    float Pad = 0.0f;
};
static_assert(sizeof(CompensationState) == 16u);

struct ProbeReading
{
    float Undo = 0.0f;
    float MeteredNits = 0.0f;
};

std::filesystem::path FindProbeSpv()
{
    namespace fs = std::filesystem;
    const fs::path exe = GameEngine::Platform::GetExecutablePath();
    fs::path dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int up = 0; up < 6; ++up)
    {
        const fs::path candidate = dir / "Shaders" / "exposure_compensation_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

CompensationState Written(float stops, float meteredLogLuminance = 0.0f)
{
    return CompensationState{stops, 1u, meteredLogLuminance, 0.0f};
}

class ExposureCompensationUndoTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty()) << "exposure_compensation_probe.comp.spv was not found above the test "
                                         "executable; the shader target stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty());
        DescriptorBinding result{};
        result.binding = 0;
        result.type = DescriptorType::StorageBuffer;
        result.count = 1;
        result.shaderStages = kShaderStageCompute;
        DescriptorBinding state = result;
        state.binding = 1;
        m_Layout.debugName = "ExposureCompensationProbe.Set0";
        m_Layout.bindings = {result, state};
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.DebugName = "ExposureCompensationProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (BufferHandle b : m_Buffers)
            m_Device->DestroyBuffer(b);
        m_Device->Shutdown();
    }

    BufferHandle CreateMapped(size_t bytes, const char* debugName)
    {
        BufferDesc bd{};
        bd.size = bytes;
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferSrc) |
                   static_cast<uint32_t>(BufferUsage::TransferDst);
        bd.memoryUsage = BufferMemoryUsage::Readback;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = debugName;
        const BufferHandle buffer = m_Device->CreateBuffer(bd);
        m_Buffers.push_back(buffer);
        return buffer;
    }

    ProbeReading Read(const CompensationState& state)
    {
        constexpr size_t kResultBytes = sizeof(float) * 4u;
        const BufferHandle result = CreateMapped(kResultBytes, "ExposureCompensationProbe.Result");
        const BufferHandle compensation = CreateMapped(sizeof(CompensationState), "ExposureCompensationProbe.State");
        const std::array<float, 4> sentinel{-7.0f, -7.0f, -7.0f, -7.0f};
        m_Device->UpdateBuffer(result, 0, kResultBytes, sentinel.data());
        m_Device->UpdateBuffer(compensation, 0, sizeof(CompensationState), &state);
        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "ExposureCompensationProbe.DS";
        const DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        m_Device->UpdateStorageBufferBinding(ds, 0, result, 0, kResultBytes);
        m_Device->UpdateStorageBufferBinding(ds, 1, compensation, 0, sizeof(CompensationState));
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->Dispatch(1u, 1u, 1u);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        std::array<float, 4> values = sentinel;
        const void* mapped = m_Device->MapBuffer(result);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(values.data(), mapped, kResultBytes);
        m_Device->UnmapBuffer(result);
        return ProbeReading{values[0], values[1]};
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};

} // namespace

// The star pass multiplies by the undo: 2^-stops for a darkening curve, 1 when the resolve has not
// written the state, when the curve brightens and when the word is not a number; never more than 8 stops.
TEST_F(ExposureCompensationUndoTest, TheUndoCancelsADarkeningCurveWithinEightStops)
{
    EXPECT_FLOAT_EQ(Read(CompensationState{-2.5f, 0u, 0.0f, 0.0f}).Undo, 1.0f) << "not written";
    EXPECT_NEAR(Read(Written(-2.5f)).Undo, std::exp2(2.5f), 1e-4f) << "-2.5 stops";
    EXPECT_FLOAT_EQ(Read(Written(1.0f)).Undo, 1.0f) << "+1 stop";
    EXPECT_FLOAT_EQ(Read(Written(std::numeric_limits<float>::quiet_NaN())).Undo, 1.0f) << "NaN";
    EXPECT_NEAR(Read(Written(-20.0f)).Undo, std::exp2(8.0f), 1e-2f) << "-20 stops";
}

// The night tint's gate reads the meter's reading in cd/m2 (log2 units of 203 cd/m2), and none (-1)
// when the resolve has not written it or the word is not a number.
TEST_F(ExposureCompensationUndoTest, TheMeteredReadingIsInCandelasPerSquareMeter)
{
    constexpr float kLampLitRoomNits = 0.31f;
    EXPECT_NEAR(Read(Written(0.0f, std::log2(kLampLitRoomNits / 203.0f))).MeteredNits, kLampLitRoomNits, 1e-4f);
    EXPECT_FLOAT_EQ(Read(CompensationState{0.0f, 0u, std::log2(kLampLitRoomNits / 203.0f), 0.0f}).MeteredNits, -1.0f)
        << "not written";
    EXPECT_FLOAT_EQ(Read(Written(0.0f, std::numeric_limits<float>::quiet_NaN())).MeteredNits, -1.0f) << "NaN";
}
