// A swapchain acquire that returns VK_SUBOPTIMAL_KHR has acquired an image and
// will signal the frame slot's imageAvailable semaphore, so the frame has to
// render and present it; the swapchain is recreated after the present. Skipping
// the frame instead leaves that binary semaphore signaled for the slot's next
// acquire (VUID-vkAcquireNextImageKHR-semaphore-01286).
//
// GE_VK_FORCE_ACQUIRE_SUBOPTIMAL (VulkanFaultInjection.h) reports every Nth
// successful acquire as SUBOPTIMAL; drivers return it on a resize or a monitor
// move, which a test cannot drive. Two driver behaviours are covered: the present
// that follows reports SUBOPTIMAL as well, and the present succeeds.
//
// Instruments: swapchain creations and forced acquires are counted from the
// device's own log lines, read between BeginFrame and Present so a recreate is
// placed before or after the present; the validation layer, when it loads on
// this agent, reports the reused semaphore.

#include "Source/Vulkan/VulkanDevice.h"

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Tests/ScopedEnvVar.h"

#include <gtest/gtest.h>

#if defined(HAVE_GLFW)
#include <GLFW/glfw3.h>
#endif

#include <atomic>
#include <memory>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
#if defined(HAVE_GLFW)
constexpr uint32_t kWindowWidth = 320;
constexpr uint32_t kWindowHeight = 200;
// Both forcings below report every second successful acquire, so forced and
// ordinary frames alternate. Frames run until kForcedFrames were forced; an
// acquire that times out under load is not counted, and kMaxFrames bounds the run.
constexpr int kForcedFrames = 6;
constexpr int kMaxFrames = 60;
constexpr const char* kSemaphoreReuseVuid = "VUID-vkAcquireNextImageKHR-semaphore-01286";

using GameEngine::Rendering::Tests::ScopedEnvVar;

struct SwapchainLogCounts
{
    std::atomic<int> Created{0};
    std::atomic<int> ForcedAcquires{0};
};

struct CountsSnapshot
{
    int Created = 0;
    int ForcedAcquires = 0;
};

void CountSwapchainLogLine(SwapchainLogCounts& counts, const Logger::LogMessage& msg)
{
    if (msg.Message.find("VulkanDevice: Created window swapchain") != Logger::String::npos)
        counts.Created.fetch_add(1);
    else if (msg.Message.find("[fault-injection] forcing VK_SUBOPTIMAL_KHR on acquire") != Logger::String::npos)
        counts.ForcedAcquires.fetch_add(1);
}

// Delivery to sinks is asynchronous; the flush makes the snapshot current.
CountsSnapshot Snapshot(const SwapchainLogCounts& counts)
{
    Logger::Log::Flush();
    return CountsSnapshot{counts.Created.load(), counts.ForcedAcquires.load()};
}

uint64_t VuidCount(const IDevice& device, const char* vuid)
{
    for (const ValidationVuidStat& v : device.GetValidationStats().Vuids)
    {
        if (v.Vuid == vuid)
            return v.Count;
    }
    return 0;
}

void SubmitEmptyGraphicsList(IDevice& dev)
{
    auto cl = dev.CreateCommandList(IDevice::QueueType::Graphics);
    if (!cl)
        return;
    cl->Begin();
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    dev.ExecuteCommandLists(lists);
}

// Drives frames on one hidden window under the given forcing and checks,
// frame by frame, where the swapchain was recreated.
void RunForcedSuboptimalFrames(const char* forcing)
{
    ScopedEnvVar forced("GE_VK_FORCE_ACQUIRE_SUBOPTIMAL", forcing);
    // The red run of this test provokes the VUID it counts; the default-on
    // validation assert would stop the process before the count is read.
    ScopedEnvVar noAssert("GE_VK_VALIDATION_ASSERT", "0");

    Logger::Log::Initialize({Logger::LogLevel::Info});
    auto counts = std::make_shared<SwapchainLogCounts>();
    auto sink = Logger::MakeUnique<Logger::CallbackSink>();
    Logger::CallbackSink* sinkPtr = sink.get();
    const auto callbackId =
        sinkPtr->RegisterCallback([counts](const Logger::LogMessage& msg) { CountSwapchainLogLine(*counts, msg); });
    Logger::Log::AddSink(std::move(sink));

    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow((int)kWindowWidth, (int)kWindowHeight, "SuboptimalAcquire", nullptr, nullptr);
    ASSERT_NE(window, nullptr);

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    desc.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(desc);
    if (!dev || !dev->Initialize(desc))
    {
        sinkPtr->UnregisterCallback(callbackId);
        glfwDestroyWindow(window);
        glfwTerminate();
        GTEST_SKIP() << "No Vulkan device available on this agent";
    }
    auto* vk = static_cast<VulkanDevice*>(dev.get());
    const bool validationActive = dev->GetValidationStats().Enabled;
    ::testing::Test::RecordProperty("validationLayerActive", validationActive ? "true" : "false");

    WindowTargetHandle target{};
    ASSERT_TRUE(dev->CreateAndActivateWindowTarget(window, kWindowWidth, kWindowHeight, &target));
    ASSERT_GE(Snapshot(*counts).Created, 1) << "the log capture did not see the window's swapchain being created";

    int forcedFrames = 0;
    for (int frame = 0; frame < kMaxFrames && forcedFrames < kForcedFrames; ++frame)
    {
        const CountsSnapshot before = Snapshot(*counts);
        const bool began = dev->BeginFrame();
        const CountsSnapshot afterBegin = Snapshot(*counts);
        const bool forcedThisFrame = afterBegin.ForcedAcquires > before.ForcedAcquires;

        EXPECT_EQ(afterBegin.Created, before.Created)
            << "frame " << frame << ": the swapchain was recreated inside BeginFrame, before the acquired image was presented";
        if (forcedThisFrame)
        {
            ++forcedFrames;
            EXPECT_TRUE(began) << "frame " << frame << ": a SUBOPTIMAL acquire skipped the frame";
        }
        if (!began)
            continue;

        SubmitEmptyGraphicsList(*dev);
        dev->Present();
        const CountsSnapshot afterPresent = Snapshot(*counts);
        EXPECT_EQ(afterPresent.Created - afterBegin.Created, forcedThisFrame ? 1 : 0)
            << "frame " << frame << (forcedThisFrame ? ": a SUBOPTIMAL acquire was not followed by a recreate after the present"
                                                     : ": an ordinary frame recreated the swapchain");
    }
    // A forcing that never fired would pass every check above without testing anything.
    EXPECT_EQ(forcedFrames, kForcedFrames) << "the forced SUBOPTIMAL acquire fired on too few of " << kMaxFrames << " frames";

    if (validationActive)
    {
        EXPECT_EQ(VuidCount(*dev, kSemaphoreReuseVuid), 0u) << "an acquired image's semaphore was never waited on";
        EXPECT_EQ(dev->GetValidationStats().ErrorCount, 0u);
    }

    // Every recreate destroyed the old swapchain's semaphores with it.
    dev->Shutdown();
    EXPECT_EQ(vk->GetLastDeviceGenerationSemaphoreResidual(), 0u);

    dev.reset();
    sinkPtr->UnregisterCallback(callbackId);
    glfwDestroyWindow(window);
    glfwTerminate();
}
#endif // HAVE_GLFW
} // namespace

TEST(SwapchainSuboptimalAcquire, SuboptimalAcquireAndPresentRenderTheFrameThenRecreate)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    RunForcedSuboptimalFrames("2");
#endif
}

// A driver may flag only the acquire; the present then succeeds and nothing but
// the acquire's result says the swapchain no longer matches the surface.
TEST(SwapchainSuboptimalAcquire, SuboptimalAcquireWithSuccessfulPresentStillRecreates)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    RunForcedSuboptimalFrames("2:acquire");
#endif
}
