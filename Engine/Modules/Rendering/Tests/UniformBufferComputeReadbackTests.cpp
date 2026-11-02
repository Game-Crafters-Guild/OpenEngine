#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"
#include "ScopedEnvVar.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Utils/BufferHelpers.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace GameEngine::Rendering;

TEST(UniformBuffer, UBOWriteVisibleToComputeShader) {
    // Headless device for CI-friendly run
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc desc{}; desc.applicationName = "UBO_Compute_Readback"; desc.preferredAPI = GraphicsAPI::Vulkan; desc.enableDynamicRendering = true; desc.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(desc)) GTEST_SKIP() << "Device init failed";
    dev->BeginFrame();

    // Create UBO (Upload, persistently mapped) and SSBO (DeviceLocal) + readback buffer
    struct U { float v; } init{ 3.14159f };

    BufferDesc uboD{}; uboD.size = sizeof(U); uboD.usage = (uint32_t)BufferUsage::Uniform; uboD.memoryUsage = BufferMemoryUsage::Upload; uboD.persistent = true; uboD.debugName = "TestUBO";
    BufferHandle ubo = dev->CreateBuffer(uboD);
    ASSERT_TRUE(ubo.IsValid());
    dev->UpdateBuffer(ubo, 0, sizeof(U), &init);

    BufferDesc ssboD{}; ssboD.size = sizeof(float); ssboD.usage = (uint32_t)(BufferUsage::Storage | BufferUsage::TransferSrc); ssboD.memoryUsage = BufferMemoryUsage::DeviceLocal; ssboD.debugName = "SSBO_Out";
    BufferHandle ssbo = dev->CreateBuffer(ssboD);
    ASSERT_TRUE(ssbo.IsValid());

    BufferDesc rbD{}; rbD.size = sizeof(float); rbD.usage = (uint32_t)BufferUsage::TransferDst; rbD.memoryUsage = BufferMemoryUsage::Readback; rbD.debugName = "RB";
    BufferHandle rb = dev->CreateBuffer(rbD);
    ASSERT_TRUE(rb.IsValid());

    // Create compute pipeline: set0 binding0 = UBO, binding1 = SSBO
    std::vector<uint8_t> cs = Utils::LoadShaderFile("ubo_copy.comp.spv");
    ASSERT_FALSE(cs.empty());

    PipelineDesc pd{}; pd.type = PipelineType::Compute; pd.computeShader = cs; pd.debugName = "UBO_Copy_CS";
    DescriptorSetLayoutDesc set{};
    { DescriptorBinding b{}; b.binding=0; b.type=DescriptorType::UniformBuffer; b.count=1; b.shaderStages=0x20; set.bindings.push_back(b); }
    { DescriptorBinding b{}; b.binding=1; b.type=DescriptorType::StorageBuffer; b.count=1; b.shaderStages=0x20; set.bindings.push_back(b); }
    pd.descriptorSetLayouts.push_back(set);

    PipelineHandle pipe = dev->CreatePipeline(pd);
    ASSERT_NE(pipe, INVALID_PIPELINE_HANDLE);

    DescriptorSetDesc dsd{}; dsd.layout=set; dsd.transient=false; dsd.debugName = "UBO_Copy_DS";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
    ASSERT_TRUE(ds.IsValid());

    // Bind UBO and SSBO
    dev->UpdateBufferBinding(ds, 0, ubo, 0, sizeof(U));
    dev->UpdateStorageBufferBinding(ds, 1, ssbo, 0, sizeof(float));

    // Dispatch
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(pipe);
    cl->BindDescriptorSet(0, ds, pipe);
    cl->Dispatch(1,1,1);
    // Ensure compute writes to SSBO are visible before transfer reads
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(ssbo, ResourceState::UnorderedAccess, ResourceState::CopySource));
    cl->CopyBuffer(ssbo, rb, sizeof(float), 0, 0);

    cl->End();
    std::vector<CommandList*> lists{ cl.get() }; dev->ExecuteCommandLists(lists);
    dev->FinalizeFrame();

    dev->WaitForIdle();

    // Read back
    const float* p = reinterpret_cast<const float*>(dev->MapBuffer(rb));
    ASSERT_NE(p, nullptr);
    float got = *p; dev->UnmapBuffer(rb);
    std::cout << "Readback: " << got << " expected: " << init.v << std::endl;

    EXPECT_NEAR(got, init.v, 1e-6f);

    // Cleanup
    dev->DestroyDescriptorSet(ds);
    dev->DestroyPipeline(pipe);
    dev->DestroyBuffer(rb);
    dev->DestroyBuffer(ssbo);
    dev->DestroyBuffer(ubo);
    // Rely on device destructor to shutdown cleanly to avoid double-shutdown quirks in some drivers
}


namespace
{

// A large device-local buffer keeps the staging copy busy long enough that a read
// submitted without a wait on it reads zeros. The shader reads the last 256 bytes;
// 256 is a multiple of every minUniformBufferOffsetAlignment Vulkan allows.
constexpr size_t kUploadBytes = 64u * 1024u * 1024u;
constexpr size_t kReadOffset = kUploadBytes - 256u;
constexpr int kUploadReadRounds = 20;

struct UploadReadRig
{
    PipelineHandle Pipeline = INVALID_PIPELINE_HANDLE;
    DescriptorSetLayoutDesc Layout;
    DescriptorSetHandle Set;
    BufferHandle Output;
    BufferHandle Readback;
};

bool CreateUploadReadRig(IDevice& device, UploadReadRig& rig)
{
    const std::vector<uint8_t> shader = Utils::LoadShaderFile("ubo_copy.comp.spv");
    if (shader.empty())
        return false;

    DescriptorBinding uniformBinding{};
    uniformBinding.binding = 0;
    uniformBinding.type = DescriptorType::UniformBuffer;
    uniformBinding.count = 1;
    uniformBinding.shaderStages = kShaderStageCompute;
    DescriptorBinding outputBinding = uniformBinding;
    outputBinding.binding = 1;
    outputBinding.type = DescriptorType::StorageBuffer;
    rig.Layout.bindings = {uniformBinding, outputBinding};

    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Compute;
    pipelineDesc.computeShader = shader;
    pipelineDesc.descriptorSetLayouts.push_back(rig.Layout);
    pipelineDesc.debugName = "UploadRead.Pipeline";
    rig.Pipeline = device.CreatePipeline(pipelineDesc);

    DescriptorSetDesc setDesc{};
    setDesc.layout = rig.Layout;
    setDesc.debugName = "UploadRead.Set";
    rig.Set = device.CreateDescriptorSet(setDesc);

    BufferDesc outputDesc{};
    outputDesc.size = sizeof(float);
    outputDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc);
    outputDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    outputDesc.debugName = "UploadRead.Output";
    rig.Output = device.CreateBuffer(outputDesc);
    rig.Readback = device.CreateReadbackBuffer(sizeof(float), "UploadRead.Readback");
    return rig.Pipeline != INVALID_PIPELINE_HANDLE && rig.Set.IsValid() && rig.Output.IsValid() &&
           rig.Readback.IsValid();
}

void DestroyUploadReadRig(IDevice& device, const UploadReadRig& rig)
{
    device.DestroyBuffer(rig.Readback);
    device.DestroyBuffer(rig.Output);
    device.DestroyDescriptorSet(rig.Set);
    device.DestroyPipeline(rig.Pipeline);
}

// Uploads `value` into a fresh device-local buffer, submits at once a dispatch that
// reads it on `queue`, and returns what the dispatch read.
float ReadFreshUpload(IDevice& device, const UploadReadRig& rig, IDevice::QueueType queue, bool viaQueueSubmit,
                      float value)
{
    std::vector<uint8_t> contents(kUploadBytes, 0u);
    std::memcpy(contents.data() + kReadOffset, &value, sizeof(value));

    BufferDesc uploadDesc{};
    uploadDesc.size = kUploadBytes;
    uploadDesc.usage = static_cast<uint32_t>(BufferUsage::Uniform | BufferUsage::TransferDst);
    uploadDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    uploadDesc.debugName = "UploadRead.Upload";
    const BufferHandle upload = CreateBufferInitialized(&device, uploadDesc, contents.data(), contents.size());
    if (!upload.IsValid())
        return 0.0f;
    device.UpdateBufferBinding(rig.Set, 0, upload, kReadOffset, sizeof(float));
    device.UpdateStorageBufferBinding(rig.Set, 1, rig.Output, 0, sizeof(float));

    auto commandList = device.CreateCommandList(queue);
    commandList->Begin();
    commandList->SetPipeline(rig.Pipeline);
    commandList->BindDescriptorSet(0, rig.Set, rig.Pipeline);
    commandList->Dispatch(1, 1, 1);
    commandList->Barrier(
        ResourceBarrier::CreateBufferBarrier(rig.Output, ResourceState::UnorderedAccess, ResourceState::CopySource));
    commandList->CopyBuffer(rig.Output, rig.Readback, sizeof(float), 0, 0);
    commandList->End();

    std::vector<CommandList*> lists{commandList.get()};
    if (viaQueueSubmit)
        device.QueueSubmit(queue, lists, {}, {});
    else
        device.ExecuteCommandLists(lists);
    device.WaitForIdle();

    float read = 0.0f;
    if (const void* mapped = device.MapBuffer(rig.Readback))
    {
        std::memcpy(&read, mapped, sizeof(read));
        device.UnmapBuffer(rig.Readback);
    }
    device.DestroyBuffer(upload);
    return read;
}

// Counts the rounds whose dispatch did not read the value uploaded just before it.
int CountMissedUploads(IDevice::QueueType queue, bool viaQueueSubmit)
{
    Tests::SetEnvVar("GE_HEADLESS_TEST", "1");
    DeviceDesc desc{};
    desc.applicationName = "UploadRead";
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        return -1;
    device->BeginFrame();

    UploadReadRig rig;
    if (!CreateUploadReadRig(*device, rig))
        return -1;

    int missed = 0;
    for (int round = 0; round < kUploadReadRounds; ++round)
    {
        const float expected = 1.0f + static_cast<float>(round);
        if (ReadFreshUpload(*device, rig, queue, viaQueueSubmit, expected) != expected)
            ++missed;
    }
    DestroyUploadReadRig(*device, rig);
    device->FinalizeFrame();
    device->WaitForIdle();
    return missed;
}

} // namespace

// IDevice::UpdateBuffer contract: the data is in the buffer for any work submitted
// after the call returns, also when a device-local buffer takes a staging copy.
TEST(UniformBuffer, DeviceLocalUploadIsVisibleToTheNextGraphicsSubmit)
{
    const int missed = CountMissedUploads(IDevice::QueueType::Graphics, false);
    if (missed < 0)
        GTEST_SKIP() << "no Vulkan device or ubo_copy shader";
    EXPECT_EQ(missed, 0) << missed << " of " << kUploadReadRounds << " dispatches read the buffer before its upload";
}

TEST(UniformBuffer, DeviceLocalUploadIsVisibleToTheNextComputeQueueSubmit)
{
    const int missed = CountMissedUploads(IDevice::QueueType::Compute, true);
    if (missed < 0)
        GTEST_SKIP() << "no Vulkan device or ubo_copy shader";
    EXPECT_EQ(missed, 0) << missed << " of " << kUploadReadRounds << " dispatches read the buffer before its upload";
}

TEST(UniformBuffer, DeviceLocalUploadIsVisibleToTheNextComputeBatch)
{
    const int missed = CountMissedUploads(IDevice::QueueType::Compute, false);
    if (missed < 0)
        GTEST_SKIP() << "no Vulkan device or ubo_copy shader";
    EXPECT_EQ(missed, 0) << missed << " of " << kUploadReadRounds << " dispatches read the buffer before its upload";
}
