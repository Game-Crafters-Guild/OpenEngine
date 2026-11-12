// A dispatched CreateAndActivateWindowTarget returns before vkCreateSwapchainKHR ends; the
// pending target is active with a valid format and size. The device adopts the swapchain at
// BeginFrame or at a window-target change. Adoptions are counted from the "Created" log line.

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <gtest/gtest.h>

#if defined(HAVE_GLFW)
#include <GLFW/glfw3.h>
#endif

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
#if defined(HAVE_GLFW)
constexpr uint32_t kWindowWidth = 320;
constexpr uint32_t kWindowHeight = 200;
constexpr int kFrames = 3;

// Holds the job a CreateAndActivateWindowTarget dispatched, so the test decides when and where it runs.
struct HeldJob
{
    std::function<void()> Job;
    std::thread::id CallerThread;
    std::thread::id JobThread;

    IDevice::WindowTargetJobDispatch Dispatch()
    {
        return [this](std::function<void()> job)
        {
            CallerThread = std::this_thread::get_id();
            Job = std::move(job);
        };
    }

    // Runs the held job on a worker thread; Join waits for it.
    std::thread RunOnWorker()
    {
        return std::thread(
            [this]()
            {
                JobThread = std::this_thread::get_id();
                Job();
            });
    }
};

// Runs the held job when `release` is set, or after `watchdog` if it is never set.
void RunHeldJobWhenReleased(HeldJob& held, const std::atomic<bool>& release, std::chrono::milliseconds watchdog)
{
    constexpr std::chrono::milliseconds kPoll{10};
    const auto deadline = std::chrono::steady_clock::now() + watchdog;
    while (!release.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(kPoll);
    held.Job();
}

int CountCreatedLines(const std::atomic<int>& created)
{
    // Delivery to sinks is asynchronous; the flush makes the count current.
    Logger::Log::Flush();
    return created.load();
}

void SubmitEmptyGraphicsList(IDevice& dev)
{
    auto cl = dev.CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(cl, nullptr);
    cl->Begin();
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    dev.ExecuteCommandLists(lists);
}

void RunFrames(IDevice& dev)
{
    int presented = 0;
    for (int frame = 0; frame < kFrames * 4 && presented < kFrames; ++frame)
    {
        // An acquire that times out under load skips the frame; the next one draws.
        if (!dev.BeginFrame())
            continue;
        SubmitEmptyGraphicsList(dev);
        dev.Present();
        ++presented;
    }
    EXPECT_EQ(presented, kFrames) << "the adopted swapchain did not present";
}

// One hidden window and one Vulkan device with the validation layer, and a log
// capture of the swapchain adoptions.
class PendingCreationFixture
{
  public:
    bool Start()
    {
        Logger::Log::Initialize({Logger::LogLevel::Info});
        auto sink = std::make_unique<Logger::CallbackSink>();
        m_Sink = sink.get();
        m_CallbackId = m_Sink->RegisterCallback(
            [created = m_Created](const Logger::LogMessage& msg)
            {
                if (msg.Message.find("VulkanDevice: Created window swapchain") != std::string::npos)
                    created->fetch_add(1);
            });
        Logger::Log::AddSink(std::move(sink));

        if (glfwInit() != GLFW_TRUE)
            return false;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        m_Window = glfwCreateWindow((int)kWindowWidth, (int)kWindowHeight, "PendingCreation", nullptr, nullptr);
        if (!m_Window)
            return false;

        DeviceDesc desc{};
        desc.preferredAPI = GraphicsAPI::Vulkan;
        desc.enableDynamicRendering = true;
        desc.enableDebugLayer = true;
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
        {
            m_Device.reset();
            return false;
        }
        ::testing::Test::RecordProperty("validationLayerActive", ValidationActive() ? "true" : "false");
        return true;
    }

    ~PendingCreationFixture()
    {
        if (m_Device)
            m_Device->Shutdown();
        m_Device.reset();
        if (m_Sink)
            m_Sink->UnregisterCallback(m_CallbackId);
        if (m_Window)
            glfwDestroyWindow(m_Window);
        glfwTerminate();
    }

    IDevice& Device() { return *m_Device; }
    GLFWwindow* Window() const { return m_Window; }
    int Created() const { return CountCreatedLines(*m_Created); }
    bool ValidationActive() const { return m_Device->GetValidationStats().Enabled; }

  private:
    std::shared_ptr<std::atomic<int>> m_Created = std::make_shared<std::atomic<int>>(0);
    Logger::CallbackSink* m_Sink = nullptr;
    uint64_t m_CallbackId = 0;
    GLFWwindow* m_Window = nullptr;
    std::unique_ptr<IDevice> m_Device;
};
#endif // HAVE_GLFW
} // namespace

TEST(WindowTargetPendingCreation, SwapchainIsCreatedOnTheDispatchedJobAndAdoptedAtTheFirstFrame)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    PendingCreationFixture fixture;
    if (!fixture.Start())
        GTEST_SKIP() << "No window or Vulkan device available on this agent";
    IDevice& dev = fixture.Device();

    HeldJob held;
    WindowTargetHandle target{};
    ASSERT_TRUE(dev.CreateAndActivateWindowTarget(fixture.Window(), kWindowWidth, kWindowHeight, &target, held.Dispatch()));
    ASSERT_TRUE(target.IsValid());
    ASSERT_TRUE(held.Job) << "CreateAndActivateWindowTarget created the swapchain on the calling thread, not on the dispatch";
    EXPECT_EQ(held.CallerThread, std::this_thread::get_id());

    // Pending: the job has not run, and the target already answers as the active one.
    EXPECT_EQ(dev.GetActiveWindowTarget().id, target.id);
    EXPECT_NE(dev.GetSwapchainTextureFormat(), TextureFormat::Unknown);
    uint32_t width = 0;
    uint32_t height = 0;
    EXPECT_TRUE(dev.GetWindowTargetSize(target, width, height));
    EXPECT_EQ(width, kWindowWidth);
    EXPECT_EQ(height, kWindowHeight);
    EXPECT_FALSE(dev.HasWindowTargetCreationFailed(target));
    EXPECT_EQ(fixture.Created(), 0) << "the swapchain was adopted before the job ran";

    std::thread worker = held.RunOnWorker();
    worker.join();
    EXPECT_NE(held.JobThread, held.CallerThread);
    EXPECT_EQ(fixture.Created(), 0) << "the job adopted the swapchain; only the owner thread may";

    RunFrames(dev);
    EXPECT_EQ(fixture.Created(), 1) << "the first frame did not adopt the swapchain exactly once";
    EXPECT_FALSE(dev.HasWindowTargetCreationFailed(target));
    if (fixture.ValidationActive())
        EXPECT_EQ(dev.GetValidationStats().ErrorCount, 0u);
#endif
}

// A resize during the pending creation applies to the adopted swapchain: the
// recreate waits for the job, adopts its swapchain, then recreates it.
TEST(WindowTargetPendingCreation, RecreateDuringThePendingCreationAdoptsFirstThenRecreates)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    PendingCreationFixture fixture;
    if (!fixture.Start())
        GTEST_SKIP() << "No window or Vulkan device available on this agent";
    IDevice& dev = fixture.Device();

    HeldJob held;
    WindowTargetHandle target{};
    ASSERT_TRUE(dev.CreateAndActivateWindowTarget(fixture.Window(), kWindowWidth, kWindowHeight, &target, held.Dispatch()));
    ASSERT_TRUE(target.IsValid());
    ASSERT_TRUE(held.Job) << "CreateAndActivateWindowTarget created the swapchain on the calling thread, not on the dispatch";

    // The recreate waits for the job without a limit, so the job runs on its worker first.
    std::thread worker = held.RunOnWorker();
    EXPECT_TRUE(dev.RecreateWindowTargetSwapchain(target, kWindowWidth, kWindowHeight));
    worker.join();
    EXPECT_EQ(fixture.Created(), 2) << "the recreate did not adopt the pending swapchain and then recreate it";

    RunFrames(dev);
    EXPECT_EQ(fixture.Created(), 2) << "a frame after the recreate created another swapchain";
    if (fixture.ValidationActive())
        EXPECT_EQ(dev.GetValidationStats().ErrorCount, 0u);
#endif
}

// BeginFrame waits a bounded time for a job that has not ended, then skips the frame
// so the caller pumps its messages. The job is held past the bound; a watchdog runs it
// if BeginFrame waits without a limit.
TEST(WindowTargetPendingCreation, BeginFrameSkipsTheFrameWhenTheJobRunsPastTheBound)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    // kAtLeastTheFrameWait is below the device's 1500 ms frame wait; kWatchdog is far above it.
    constexpr std::chrono::milliseconds kAtLeastTheFrameWait{1000};
    constexpr std::chrono::milliseconds kWatchdog{6000};

    PendingCreationFixture fixture;
    if (!fixture.Start())
        GTEST_SKIP() << "No window or Vulkan device available on this agent";
    IDevice& dev = fixture.Device();

    HeldJob held;
    WindowTargetHandle target{};
    ASSERT_TRUE(dev.CreateAndActivateWindowTarget(fixture.Window(), kWindowWidth, kWindowHeight, &target, held.Dispatch()));
    ASSERT_TRUE(held.Job) << "CreateAndActivateWindowTarget created the swapchain on the calling thread, not on the dispatch";

    std::atomic<bool> release{false};
    std::thread watchdog(RunHeldJobWhenReleased, std::ref(held), std::cref(release), kWatchdog);

    const auto frameBegin = std::chrono::steady_clock::now();
    const bool frameBegun = dev.BeginFrame();
    const auto frameWait = std::chrono::steady_clock::now() - frameBegin;
    release.store(true);
    watchdog.join();
    if (frameBegun)
    {
        // Close the wrongly begun frame, so the next acquire is valid and the failure reports cleanly.
        SubmitEmptyGraphicsList(dev);
        dev.Present();
    }

    EXPECT_FALSE(frameBegun) << "BeginFrame acquired while the swapchain job was held";
    EXPECT_GE(frameWait, kAtLeastTheFrameWait) << "BeginFrame did not wait for the job";
    EXPECT_LT(frameWait, kWatchdog) << "BeginFrame waited for the job without a limit";
    EXPECT_FALSE(dev.HasWindowTargetCreationFailed(target)) << "a skipped frame marked the pending creation failed";
    EXPECT_EQ(fixture.Created(), 0) << "the skipped frame adopted a swapchain";

    RunFrames(dev);
    EXPECT_EQ(fixture.Created(), 1) << "the frames after the job did not adopt the swapchain exactly once";
    if (fixture.ValidationActive())
        EXPECT_EQ(dev.GetValidationStats().ErrorCount, 0u);
#endif
}

// A second device takes the window while the job is held, so the driver call fails with
// VK_ERROR_NATIVE_WINDOW_IN_USE_KHR. BeginFrame stops the start and destroys the target.
TEST(WindowTargetPendingCreation, FailedJobMakesBeginFrameFailAndMarksTheTarget)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    PendingCreationFixture fixture;
    if (!fixture.Start())
        GTEST_SKIP() << "No window or Vulkan device available on this agent";
    IDevice& dev = fixture.Device();

    HeldJob held;
    WindowTargetHandle target{};
    ASSERT_TRUE(dev.CreateAndActivateWindowTarget(fixture.Window(), kWindowWidth, kWindowHeight, &target, held.Dispatch()));
    ASSERT_TRUE(held.Job) << "CreateAndActivateWindowTarget created the swapchain on the calling thread, not on the dispatch";

    DeviceDesc otherDesc{};
    otherDesc.preferredAPI = GraphicsAPI::Vulkan;
    otherDesc.enableDynamicRendering = true;
    std::unique_ptr<IDevice> other = DeviceFactory::CreateDevice(otherDesc);
    ASSERT_TRUE(other && other->Initialize(otherDesc)) << "a second Vulkan device did not initialize";
    ASSERT_TRUE(other->CreateAndActivateWindowTarget(fixture.Window(), kWindowWidth, kWindowHeight))
        << "the second device did not take the window, so the job cannot fail";

    std::thread worker = held.RunOnWorker();
    worker.join();

    EXPECT_FALSE(dev.BeginFrame()) << "BeginFrame acquired after the swapchain job failed";
    EXPECT_TRUE(dev.HasWindowTargetCreationFailed(target));
    EXPECT_FALSE(dev.GetActiveWindowTarget().IsValid()) << "the failed target is still active";
    if (fixture.ValidationActive())
        EXPECT_EQ(dev.GetValidationStats().ErrorCount, 0u);
#endif
}
