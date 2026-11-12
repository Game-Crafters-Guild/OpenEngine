#pragma once

#include "JobSystem/TaskHandle.h"
#include "Rendering/Core/Device.h"
#include "Types/Types.h"

#include <cassert>
#include <memory>
#include <optional>
#include "Rendering/Camera/CameraDerivation.h"

namespace JobSystem
{
class JobChannel;
} // namespace JobSystem

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::DeviceDesc;
using ::GameEngine::Rendering::IDevice;
using ::GameEngine::Rendering::WindowTargetHandle;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer
{
class RenderServices;

/**
 * @brief Owns a single rendering device and its associated RenderServices.
 *
 * Centralizes shutdown ordering so higher layers do not need ad-hoc
 * WaitForIdle/cleanup calls to avoid validation errors.
 *
 * Policy:
 * - Higher layers should destroy device-using objects (UI, scene controllers, etc.)
 *   before calling Shutdown() on this context.
 * - Shutdown() is idempotent.
 */
class RenderDeviceContext final
{
  public:
    struct InitParams
    {
        Rendering::DeviceDesc deviceDesc{};
        // Optional shared device attachment mode:
        // - nullptr: this context owns its own device (legacy mode).
        // - non-null: this context attaches to an existing device and only owns
        //   per-window swapchain target + optional RenderServices.
        Rendering::IDevice* sharedDevice = nullptr;
        void* windowHandle = nullptr;
        uint32 width = 0;
        uint32 height = 0;
        // The window's swapchain is created on a job of this channel while the caller continues, and the device
        // waits for it before the first frame. Null creates it on the calling thread.
        JobSystem::JobChannel* windowTargetChannel = nullptr;

        // Create and own RenderServices for this device.
        bool createRenderServices = false;
    };

    RenderDeviceContext();
    ~RenderDeviceContext();

    RenderDeviceContext(const RenderDeviceContext&) = delete;
    RenderDeviceContext& operator=(const RenderDeviceContext&) = delete;

    /**
     * @brief Start creating this context's own device from `desc` on a job of `channel`. The next
     * Initialize joins the job, makes its own thread the device's owner and ignores
     * params.deviceDesc and params.sharedDevice; a failed job makes it log the fix and fail.
     */
    void BeginDeviceCreation(JobSystem::JobChannel& channel, const Rendering::DeviceDesc& desc);

    bool Initialize(const InitParams& params);
    void Shutdown();

    bool IsInitialized() const { return m_Initialized; }

    // Not valid while a BeginDeviceCreation job is still to be joined by Initialize.
    Rendering::IDevice* GetDevice() const
    {
        assert(!m_DeviceCreation.IsValid() && "RenderDeviceContext::GetDevice before Initialize joined the device creation");
        return m_OwnsDevice ? m_Device.get() : m_AttachedDevice;
    }
    RenderServices* GetRenderServices() const { return m_RenderServices.get(); }
    Rendering::WindowTargetHandle GetWindowTarget() const { return m_WindowTarget; }
    bool HasWindowTarget() const { return m_WindowTarget.IsValid(); }
    bool ActivateWindowTarget() const;
    bool RecreateWindowTargetSwapchain(uint32 width, uint32 height) const;

    // Advanced: release ownership (callers take over lifetime).
    std::unique_ptr<Rendering::IDevice> ReleaseDevice();
    std::unique_ptr<RenderServices> ReleaseRenderServices();

  private:
    // The outcome of creating this context's own device. The thread that joins
    // the creation logs the fix for a failure.
    enum class OwnedDeviceResult : uint8
    {
        Created,
        BackendNotInBuild,
        DeviceFailed
    };

    OwnedDeviceResult CreateOwnedDevice(const Rendering::DeviceDesc& desc);
    // Waits for the BeginDeviceCreation job and binds the device to the calling
    // thread. Returns the job's result, or DeviceFailed when the job did not complete.
    OwnedDeviceResult WaitForDeviceCreation();

    bool m_Initialized = false;
    bool m_OwnsDevice = true;
    std::unique_ptr<Rendering::IDevice> m_Device;
    // The BeginDeviceCreation desc and job; the job writes m_Device.
    std::optional<Rendering::DeviceDesc> m_CreationDesc;
    JobSystem::TaskHandle m_DeviceCreation;
    Rendering::IDevice* m_AttachedDevice = nullptr; // not owned
    Rendering::WindowTargetHandle m_WindowTarget{};
    std::unique_ptr<RenderServices> m_RenderServices;
};

} // namespace GameEngine::Engine::Renderer


