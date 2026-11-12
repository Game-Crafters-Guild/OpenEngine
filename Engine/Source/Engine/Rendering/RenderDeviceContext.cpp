#include "Engine/Rendering/RenderDeviceContext.h"

#include "Engine/Rendering/RenderServices.h"
#include "JobSystem/JobChannel.h"
#include "Logger/Logger.h"
#include "Platform/Capabilities.h"
#include "Rendering/Core/Device.h"

#include <chrono>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer
{
using namespace ::GameEngine::Rendering;

RenderDeviceContext::RenderDeviceContext() = default;

RenderDeviceContext::~RenderDeviceContext()
{
    Shutdown();
}

void RenderDeviceContext::BeginDeviceCreation(JobSystem::JobChannel& channel, const DeviceDesc& desc)
{
    assert(!m_Initialized && !m_Device && !m_CreationDesc.has_value() &&
           "RenderDeviceContext::BeginDeviceCreation needs a context without a device");
    m_OwnsDevice = true;
    m_CreationDesc = desc;
    if (Platform::SupportsTransientThreads())
        m_DeviceCreation = channel.Submit([this]() { return CreateOwnedDevice(*m_CreationDesc); });
}

RenderDeviceContext::OwnedDeviceResult RenderDeviceContext::CreateOwnedDevice(const DeviceDesc& desc)
{
    const auto tDev = std::chrono::high_resolution_clock::now();
    // The factory returns null only for a backend this build does not include.
    m_Device = GameEngine::Rendering::DeviceFactory::CreateDevice(desc);
    if (!m_Device)
    {
        Logger::Log::Error("RenderDeviceContext::Initialize: DeviceFactory::CreateDevice failed");
        return OwnedDeviceResult::BackendNotInBuild;
    }
    if (!m_Device->Initialize(desc))
    {
        Logger::Log::Error("RenderDeviceContext::Initialize: device Initialize failed");
        m_Device.reset();
        return OwnedDeviceResult::DeviceFailed;
    }
    Logger::Log::Info("[Startup]   Device create+init: {:.1f}ms",
                      std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tDev).count());
    return OwnedDeviceResult::Created;
}

RenderDeviceContext::OwnedDeviceResult RenderDeviceContext::WaitForDeviceCreation()
{
    m_DeviceCreation.Wait();
    OwnedDeviceResult result = OwnedDeviceResult::DeviceFailed;
    const bool completed = m_DeviceCreation.TryGetResult(result);
    m_DeviceCreation = JobSystem::TaskHandle();
    if (m_Device)
        m_Device->BindOwnerThread();
    return completed ? result : OwnedDeviceResult::DeviceFailed;
}

bool RenderDeviceContext::Initialize(const InitParams& params)
{
    if (m_Initialized)
    {
        return true;
    }

    auto MsSince = [](const auto& t) {
        return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t).count();
    };
    const auto tCtxStart = std::chrono::high_resolution_clock::now();

    const bool creationBegun = m_CreationDesc.has_value();
    const DeviceDesc& deviceDesc = creationBegun ? *m_CreationDesc : params.deviceDesc;
    if (creationBegun)
    {
        OwnedDeviceResult result = OwnedDeviceResult::DeviceFailed;
        if (m_DeviceCreation.IsValid())
        {
            result = WaitForDeviceCreation();
            Logger::Log::Info("[Startup]   Device join wait: {:.1f}ms", MsSince(tCtxStart));
        }
        else
        {
            result = CreateOwnedDevice(deviceDesc);
        }
        if (result == OwnedDeviceResult::BackendNotInBuild)
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: the render device was not created. "
                               "Request a graphics API that this build includes; the error above names the missing backend.");
            Shutdown();
            return false;
        }
        if (result != OwnedDeviceResult::Created)
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: the render device was not created. "
                               "Install or update the GPU driver; the errors above name the step that failed.");
            Shutdown();
            return false;
        }
    }
    else if (params.sharedDevice == nullptr)
    {
        m_OwnsDevice = true;
        if (CreateOwnedDevice(deviceDesc) != OwnedDeviceResult::Created)
            return false;
    }
    else
    {
        // Attach mode: device is owned by another context.
        m_OwnsDevice = false;
        m_AttachedDevice = params.sharedDevice;
    }

    Rendering::IDevice* device = GetDevice();
    if (!device)
    {
        Logger::Log::Error("RenderDeviceContext::Initialize: no device available");
        return false;
    }

    // Swapchain/WSI setup (if requested)
    if (deviceDesc.enableSwapchain)
    {
        auto tSwap = std::chrono::high_resolution_clock::now();
        if (!params.windowHandle)
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: enableSwapchain=true but windowHandle is null");
            Shutdown();
            return false;
        }
        if (params.width == 0 || params.height == 0)
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: invalid swapchain size {}x{}", params.width, params.height);
            Shutdown();
            return false;
        }
        IDevice::WindowTargetJobDispatch dispatch;
        if (params.windowTargetChannel)
            dispatch = [channel = params.windowTargetChannel](std::function<void()> job) { channel->Enqueue(std::move(job)); };
        if (!device->CreateAndActivateWindowTarget(params.windowHandle, params.width, params.height, &m_WindowTarget, dispatch))
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: CreateAndActivateWindowTarget failed");
            Shutdown();
            return false;
        }
        Logger::Log::Info("[Startup]   Swapchain: {:.1f}ms", MsSince(tSwap));
    }

    if (params.createRenderServices)
    {
        auto tRS = std::chrono::high_resolution_clock::now();
        m_RenderServices = std::make_unique<GameEngine::Engine::Renderer::RenderServices>();
        if (!m_RenderServices)
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: failed to allocate RenderServices");
            Shutdown();
            return false;
        }

        if (!m_RenderServices->Initialize(device))
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: RenderServices::Initialize failed");
            Shutdown();
            return false;
        }

        Logger::Log::Info("[Startup]   RenderServices: {:.1f}ms", MsSince(tRS));
    }

    Logger::Log::Info("[Startup] RenderDeviceContext TOTAL: {:.1f}ms", MsSince(tCtxStart));
    m_Initialized = true;
    return true;
}

void RenderDeviceContext::Shutdown()
{
    if (m_DeviceCreation.IsValid())
        (void)WaitForDeviceCreation();
    // The next Initialize reads its own params, not this creation's description.
    m_CreationDesc.reset();

    if (!GetDevice() && !m_RenderServices)
    {
        m_Initialized = false;
        return;
    }

    if (m_OwnsDevice)
    {
        // Owning contexts drain the entire device before teardown.
        if (Rendering::IDevice* device = GetDevice())
            device->WaitForIdle();
    }

    // Tear down RenderServices first so it can release device-backed resources
    // while the device is still valid.
    if (m_RenderServices)
    {
        m_RenderServices->Shutdown();
        m_RenderServices.reset();
    }

    // Detach/destroy this context's window target before device shutdown.
    if (Rendering::IDevice* device = GetDevice(); device && m_WindowTarget.IsValid())
    {
        device->DestroyWindowTarget(m_WindowTarget);
        m_WindowTarget = {};
    }

    // Shut down owned device last. In attach mode, caller owns device lifetime.
    if (m_OwnsDevice && m_Device)
    {
        m_Device->Shutdown();
        m_Device.reset();
    }
    m_AttachedDevice = nullptr;

    m_Initialized = false;
}

std::unique_ptr<GameEngine::Rendering::IDevice> RenderDeviceContext::ReleaseDevice()
{
    m_Initialized = false;
    return std::move(m_Device);
}

std::unique_ptr<GameEngine::Engine::Renderer::RenderServices> RenderDeviceContext::ReleaseRenderServices()
{
    m_Initialized = false;
    return std::move(m_RenderServices);
}

bool RenderDeviceContext::ActivateWindowTarget() const
{
    Rendering::IDevice* device = GetDevice();
    if (!device)
    {
        return false;
    }
    if (!m_WindowTarget.IsValid())
    {
        return true;
    }
    return device->SetActiveWindowTarget(m_WindowTarget);
}

bool RenderDeviceContext::RecreateWindowTargetSwapchain(uint32 width, uint32 height) const
{
    Rendering::IDevice* device = GetDevice();
    if (!device)
    {
        return false;
    }
    if (!m_WindowTarget.IsValid())
    {
        Logger::Log::Warning("RenderDeviceContext::RecreateWindowTargetSwapchain called without a valid window target");
        return false;
    }
    return device->RecreateWindowTargetSwapchain(m_WindowTarget, width, height);
}

} // namespace GameEngine::Engine::Renderer


