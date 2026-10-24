// The headless Vulkan device a test executable's tests share: created by the first test that
// asks for it, shut down once, after the last test. Creating a Vulkan device is most of a
// device-backed test's cost, and one device per process means one device teardown per
// process. A test that asserts something about the device itself (device loss and rebuild,
// the validation layer, an environment variable the device reads at creation) creates its own.
//
// Tests run on the gtest main thread one after another, so the lazy creation needs no lock.
// Retirement (SharedHeadlessDevice.h) runs at two points: fixture destruction, for fixtures
// built in a loop inside one test, and every test's end, for the tests that build GPU
// resources as locals of the test body.
#include "SharedHeadlessDevice.h"

#include "Rendering/Core/Device.h"

#include <gtest/gtest.h>

namespace
{

std::unique_ptr<GameEngine::Rendering::IDevice> g_SharedDevice;
bool g_SharedDeviceCreated = false;

class SharedHeadlessDeviceEnvironment : public ::testing::Environment
{
  public:
    // Resets the creation latch too: with --gtest_recreate_environments_when_repeating
    // the next iteration creates a fresh device instead of handing every test null.
    void TearDown() override
    {
        if (g_SharedDevice)
            g_SharedDevice->Shutdown();
        g_SharedDevice.reset();
        g_SharedDeviceCreated = false;
    }
};

// Runs after the test's fixture and the test body's locals are destroyed, so everything
// the test built has released its buffers.
class SharedHeadlessDeviceTestEndRetirement : public ::testing::EmptyTestEventListener
{
  public:
    void OnTestEnd(const ::testing::TestInfo&) override { RetireSharedDeviceReleases(); }
};

// GTest takes ownership and runs TearDown() after the last test.
[[maybe_unused]] const ::testing::Environment* const g_SharedHeadlessDeviceEnvironment =
    ::testing::AddGlobalTestEnvironment(new SharedHeadlessDeviceEnvironment());

// GTest takes ownership of the listener.
bool AppendTestEndRetirement()
{
    ::testing::UnitTest::GetInstance()->listeners().Append(new SharedHeadlessDeviceTestEndRetirement());
    return true;
}

[[maybe_unused]] const bool g_TestEndRetirementAppended = AppendTestEndRetirement();

} // namespace

std::unique_ptr<GameEngine::Rendering::IDevice> MakeHeadlessDevice()
{
    GameEngine::Rendering::DeviceDesc desc{};
    desc.preferredAPI = GameEngine::Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    desc.enableSwapchain = false;
    desc.enableDebugLayer = false;
    auto device = GameEngine::Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        return nullptr;
    return device;
}

GameEngine::Rendering::IDevice* SharedHeadlessDevice()
{
    if (!g_SharedDeviceCreated)
    {
        g_SharedDeviceCreated = true;
        g_SharedDevice = MakeHeadlessDevice();
    }
    return g_SharedDevice.get();
}

// WaitForIdle is the device's retirement point outside a frame loop: it idles the queues
// and destroys every queued release, whether or not anything was ever submitted.
void RetireSharedDeviceReleases()
{
    if (g_SharedDevice)
        g_SharedDevice->WaitForIdle();
}
