#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "ScopedEnvVar.h"
#include "TestUtils.h"
#include "Vulkan/VulkanCommandList.h"
#include "Vulkan/VulkanDevice/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kWidth = 8;
constexpr uint32_t kMipCount = 2;
constexpr uint32_t kLayerCount = 2;
constexpr uint32_t kPixelCount = kLayerCount * (kWidth * kWidth + (kWidth / 2) * (kWidth / 2));
constexpr uint64_t kTimeoutNanoseconds = 5'000'000'000;

uint32_t Pixel(uint32_t x, uint32_t y, uint32_t mip, uint32_t layer)
{
    return (17u + x * 19u) | ((23u + y * 17u) << 8u) |
           ((31u + mip * 53u + layer * 71u) << 16u) | 0xff000000u;
}
} // namespace

class TextureUploadSynchronization : public ::testing::TestWithParam<bool>
{
  protected:
    void SetUp() override
    {
        DeviceDesc description{};
        description.preferredAPI = GraphicsAPI::Vulkan;
        description.enableDebugLayer = true;
        description.enableSwapchain = false;
        m_Device = DeviceFactory::CreateDevice(description);
        if (!m_Device || !m_Device->Initialize(description))
        {
            m_Device.reset();
            GTEST_SKIP() << "Vulkan device is unavailable";
        }
        if (!m_Device->GetValidationStats().Enabled)
            GTEST_SKIP() << "VK_LAYER_KHRONOS_validation is unavailable";
        auto* vulkan = static_cast<VulkanDevice*>(m_Device.get());
        if (vulkan->GetTransferQueueFamilyIndex() == vulkan->GetGraphicsQueueFamilyIndex())
            GTEST_SKIP() << "A dedicated transfer queue family is required";
        RecordProperty("synchronization2", vulkan->SupportsSynchronization2() ? "true" : "false");
        m_Device->ResetValidationStats();
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_Descriptor.IsValid())
            m_Device->DestroyDescriptorSet(m_Descriptor);
        if (m_Pipeline.IsValid())
            m_Device->DestroyPipeline(m_Pipeline);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        if (m_Readback.IsValid())
            m_Device->DestroyBuffer(m_Readback);
        if (m_Output.IsValid())
            m_Device->DestroyBuffer(m_Output);
        if (m_Texture.IsValid())
            m_Device->DestroyTexture(m_Texture);
        if (m_Completion.IsValid())
            m_Device->DestroySemaphore(m_Completion);
        const ValidationStats validation = m_Device->GetValidationStats();
        EXPECT_EQ(validation.ErrorCount, 0u);
        for (const auto& vuid : validation.Vuids)
            EXPECT_FALSE(vuid.IsError) << vuid.Vuid << ": " << vuid.FirstMessage;
        m_Device.reset();
    }

    Tests::ScopedEnvVar m_ValidationAssert{"GE_VK_VALIDATION_ASSERT", "0"};
    Tests::ScopedEnvVar m_SyncValidation{"GE_VK_SYNC_VALIDATION", "1"};
    // Shader-access synchronization checks require ordinary descriptor sets.
    Tests::ScopedEnvVar m_CaptureCompat{"GE_VK_CAPTURE_COMPAT", "1"};
    Tests::ScopedEnvVar m_ShaderAccessValidation{"VK_KHRONOS_VALIDATION_SYNCVAL_SHADER_ACCESSES_HEURISTIC", "1"};
    std::unique_ptr<IDevice> m_Device;
    TextureHandle m_Texture{};
    BufferHandle m_Output{};
    BufferHandle m_Readback{};
    SamplerHandle m_Sampler{};
    PipelineHandle m_Pipeline{};
    DescriptorSetHandle m_Descriptor{};
    SemaphoreHandle m_Completion{};
};

TEST_P(TextureUploadSynchronization, GraphicsSamplesEveryUploadedMipAndLayerWithoutIdleDrain)
{
    TextureDesc texture{};
    texture.width = kWidth;
    texture.height = kWidth;
    texture.mipLevels = kMipCount;
    texture.arrayLayers = kLayerCount;
    texture.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    texture.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    m_Texture = m_Device->CreateTexture(texture);
    ASSERT_TRUE(m_Texture.IsValid());

    std::array<std::vector<uint32_t>, kMipCount * kLayerCount> source;
    std::vector<IDevice::TextureUploadRequest> requests;
    std::vector<uint32_t> expected;
    for (uint32_t mip = 0; mip < kMipCount; ++mip)
    {
        const uint32_t width = kWidth >> mip;
        for (uint32_t layer = 0; layer < kLayerCount; ++layer)
        {
            auto& pixels = source[mip * kLayerCount + layer];
            const uint32_t rowLength = width + 4;
            pixels.assign(rowLength * width, 0xdeadbeefu);
            for (uint32_t y = 0; y < width; ++y)
                for (uint32_t x = 0; x < width; ++x)
                {
                    pixels[y * rowLength + x] = Pixel(x, y, mip, layer);
                    expected.push_back(Pixel(x, y, mip, layer));
                }
            requests.push_back({m_Texture, pixels.data(), width, width,
                                rowLength * sizeof(uint32_t), mip, layer});
        }
    }
    // A repeated subresource also checks the pre-copy barrier on a subresource this list already uploaded.
    requests.push_back(requests.front());

    BufferDesc output{};
    output.size = kPixelCount * sizeof(uint32_t);
    output.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc);
    output.memoryUsage = BufferMemoryUsage::DeviceLocal;
    m_Output = m_Device->CreateBuffer(output);
    m_Readback = m_Device->CreateReadbackBuffer(output.size);
    ASSERT_TRUE(m_Output.IsValid());
    ASSERT_TRUE(m_Readback.IsValid());

    DescriptorSetLayoutDesc layout{};
    layout.bindings.push_back({0, DescriptorType::CombinedImageSampler, 1, VK_SHADER_STAGE_COMPUTE_BIT});
    layout.bindings.push_back({1, DescriptorType::StorageBuffer, 1, VK_SHADER_STAGE_COMPUTE_BIT});
    PipelineDesc pipeline{};
    pipeline.type = PipelineType::Compute;
    pipeline.computeShader = Tests::ReadSpirvBytes("texture_upload_readback.comp.spv");
    ASSERT_FALSE(pipeline.computeShader.empty());
    pipeline.pushConstantSize = 4 * sizeof(uint32_t);
    pipeline.descriptorSetLayouts.push_back(layout);
    m_Pipeline = m_Device->CreatePipeline(pipeline);
    ASSERT_TRUE(m_Pipeline.IsValid());
    m_Sampler = m_Device->CreateSampler(SamplerDesc{});
    ASSERT_TRUE(m_Sampler.IsValid());
    DescriptorSetDesc descriptor{};
    descriptor.layout = layout;
    descriptor.transient = false;
    m_Descriptor = m_Device->CreateDescriptorSet(descriptor);
    ASSERT_TRUE(m_Descriptor.IsValid());
    m_Device->UpdateCombinedImageSamplerBinding(m_Descriptor, 0, m_Texture, m_Sampler);
    m_Device->UpdateStorageBufferBinding(m_Descriptor, 1, m_Output, 0, output.size);
    m_Completion = m_Device->CreateTimelineSemaphore();
    ASSERT_TRUE(m_Completion.IsValid());

    const uint64_t beforeInstrument = m_Device->GetIdleDrainCount();
    m_Device->WaitForIdle();
    const uint64_t idleDrains = m_Device->GetIdleDrainCount();
    ASSERT_EQ(idleDrains, beforeInstrument + 1);
    const IDevice::GpuSyncToken uploaded = m_Device->SubmitTextureUploads(requests.data(),
                                                                          static_cast<uint32_t>(requests.size()));
    ASSERT_TRUE(uploaded.IsValid());
    if (GetParam())
    {
        ASSERT_TRUE(m_Device->WaitGpuSyncToken(uploaded, kTimeoutNanoseconds));
    }
    for (auto& pixels : source)
        std::fill(pixels.begin(), pixels.end(), 0u);

    auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commands, nullptr);
    commands->Begin();
    commands->SetPipeline(m_Pipeline);
    commands->BindDescriptorSet(0, m_Descriptor, m_Pipeline);
    uint32_t offset = 0;
    for (uint32_t mip = 0; mip < kMipCount; ++mip)
    {
        const uint32_t width = kWidth >> mip;
        // Four uints match the compute shader's push-constant layout.
        const std::array<uint32_t, 4> parameters{mip, width, width, offset};
        commands->SetPushConstants(parameters);
        commands->Dispatch(1, 1, kLayerCount);
        // Shader-access validation tracks the whole storage-buffer descriptor.
        if (mip + 1 < kMipCount)
            commands->Barrier(ResourceBarrier::CreateBufferBarrier(m_Output, ResourceState::UnorderedAccess,
                                                                   ResourceState::UnorderedAccess));
        offset += kLayerCount * width * width;
    }
    commands->Barrier(ResourceBarrier::CreateBufferBarrier(m_Output, ResourceState::UnorderedAccess,
                                                           ResourceState::CopySource));
    commands->CopyBuffer(m_Output, m_Readback, output.size);
    VkMemoryBarrier hostRead{};
    hostRead.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(static_cast<VulkanCommandList*>(commands.get())->GetVkCommandBuffer(),
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &hostRead, 0, nullptr, 0, nullptr);
    commands->End();
    ASSERT_TRUE(m_Device->QueueSubmit(IDevice::QueueType::Graphics, {commands.get()},
                                      {{uploaded.sem, uploaded.value}}, {{m_Completion, 1}}));
    ASSERT_TRUE(m_Device->WaitTimelineSemaphoreValue(m_Completion, 1, kTimeoutNanoseconds));
    EXPECT_EQ(m_Device->QueryGpuSyncToken(uploaded), IDevice::GpuSyncStatus::Complete);
    EXPECT_EQ(m_Device->GetIdleDrainCount(), idleDrains);
    const auto* actual = static_cast<const uint32_t*>(m_Device->MapBuffer(m_Readback));
    ASSERT_NE(actual, nullptr);
    for (size_t pixel = 0; pixel < expected.size(); ++pixel)
        EXPECT_EQ(actual[pixel], expected[pixel]) << "pixel " << pixel;
    m_Device->UnmapBuffer(m_Readback);
}

INSTANTIATE_TEST_SUITE_P(HostAndQueueWaits, TextureUploadSynchronization, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info)
                         { return info.param ? "HostThenQueueWait" : "QueueWait"; });
