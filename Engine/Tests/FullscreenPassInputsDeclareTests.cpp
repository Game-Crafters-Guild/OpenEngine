// AddFullscreenPass's input caps. The widest caller, the ocean's underwater overlay with shadowed god
// rays, binds five buffers (params, shadow data, ribbons, materials, the view's ExposureHistory); a cap
// below that aborted DebugFast and silently dropped the fifth binding in Release. Over a cap the pass is
// refused whole. Part of RenderPipelineDeclareTests.
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "TestDeviceHelper.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
// The underwater overlay's buffer inputs with shadowed god rays (OceanUnderwater::DeclareForView).
constexpr uint32_t kUnderwaterShadowedBufferInputs = 5;

struct FullscreenInputsFrame
{
    std::unique_ptr<IDevice> Device;
    std::unique_ptr<RenderGraph::RGResourcePool> Persistent;
    std::unique_ptr<RenderGraph::RGTransientPool> Transient;
    std::unique_ptr<RenderGraph::RGUploadRing> Ring;
    std::unique_ptr<RenderGraph::RGFrame> Frame;

    bool Up()
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            return false;
        Persistent = std::make_unique<RenderGraph::RGResourcePool>(Device.get());
        Transient = std::make_unique<RenderGraph::RGTransientPool>(Device.get());
        Ring = std::make_unique<RenderGraph::RGUploadRing>(Device.get(), 2, 65536);
        Frame = std::make_unique<RenderGraph::RGFrame>(Device.get(), Persistent.get(), Transient.get(), Ring.get());
        Frame->BeginFrame(0);
        return true;
    }

    // `count` buffer inputs, each declaring a read of its own persistent storage buffer.
    RenderGraph::RGPass Declare(uint32_t count, std::set<std::string>& outBufferNames)
    {
        TextureDesc td{};
        td.width = td.height = 4;
        td.depth = td.mipLevels = td.arrayLayers = td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
        const RenderGraph::RGTexture target = Frame->ImportPersistentTexture("FPI.Target", td);

        std::vector<RenderGraph::RGFullscreenBufferInput> inputs(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const std::string name = "FPI.Buffer" + std::to_string(i);
            BufferDesc bd{};
            bd.size = 16;
            bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
            bd.memoryUsage = BufferMemoryUsage::DeviceLocal;
            bd.debugName = name.c_str();
            const RenderGraph::RGBuffer buffer = Frame->ImportPersistentBuffer(name.c_str(), bd);
            inputs[i].Binding = i;
            inputs[i].Buffer = Frame->PhysicalBuffer(buffer);
            inputs[i].Size = 16;
            inputs[i].IsStorage = true;
            inputs[i].Declare = buffer;
        }
        RenderGraph::RGFullscreenDesc desc{};
        desc.Name = "FPI.Pass";
        desc.Target = target;
        desc.Buffers = inputs;
        desc.Ops.Load = RenderGraph::RGLoadOp::DontCare;
        desc.Ops.Store = RenderGraph::RGStoreOp::Store;
        const RenderGraph::RGPass declared = RenderGraph::AddFullscreenPass(*Frame, desc);
        const RenderGraph::RGGraph& g = Frame->Graph();
        for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        {
            const char* passName = g.PassName(a.Pass);
            const char* resourceName = g.ResourceName(a.Resource);
            if (passName && resourceName && std::string(passName) == "FPI.Pass" &&
                std::string(resourceName).rfind("FPI.Buffer", 0) == 0)
                outBufferNames.insert(resourceName);
        }
        return declared;
    }

    void Down()
    {
        Frame.reset();
        Device->WaitForIdle();
        Device->Shutdown();
    }
};
} // namespace

TEST(FullscreenPassInputs, TheUnderwaterOverlaysFiveBuffersAreAllDeclared)
{
    FullscreenInputsFrame f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_GE(RenderGraph::kMaxFullscreenBufferInputs, kUnderwaterShadowedBufferInputs)
        << "the cap must hold the underwater overlay's shadowed inputs";
    std::set<std::string> read;
    const RenderGraph::RGPass pass = f.Declare(kUnderwaterShadowedBufferInputs, read);
    EXPECT_TRUE(pass.IsValid());
    EXPECT_EQ(read.size(), kUnderwaterShadowedBufferInputs) << "every buffer input declares its read";
    f.Down();
}

TEST(FullscreenPassInputs, ADeclarationOverTheCapIsRefusedWhole)
{
    FullscreenInputsFrame f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    std::set<std::string> read;
    const RenderGraph::RGPass pass = f.Declare(RenderGraph::kMaxFullscreenBufferInputs + 1, read);
    EXPECT_FALSE(pass.IsValid()) << "over the cap no pass is declared";
    EXPECT_TRUE(read.empty()) << "no input of a refused pass is half-declared";
    f.Down();
}
