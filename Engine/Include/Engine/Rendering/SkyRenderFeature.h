#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Rendering/Sky/SkyRenderer.h"
#include "Rendering/Sky/SkySettings.h"
#include "Rendering/Sky/SkySunState.h"
#include "Rendering/Sky/SkySystem.h"

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::IDevice;
using ::GameEngine::Rendering::SkyRenderer;
using ::GameEngine::Rendering::SkySettings;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
namespace Engine::Renderer
{
class SkyRenderFeature : public IRenderFeature
{
  public:
    bool Initialize(Rendering::IDevice* device);
    bool IsInitialized() const { return m_Initialized; }

    void SetSettings(const Rendering::SkySettings& settings, const Rendering::SkySunState& sunState);
    void ClearActiveSettings();
    const Rendering::SkySettings& GetSettings() const { return m_Settings; }
    /// The sun state the sky published with its settings this frame (SkySunState).
    const Rendering::SkySunState& GetSunState() const { return m_SunState; }
    bool HasActiveSettings() const { return m_HasSettings; }

    Rendering::SkyRenderer& GetRenderer() { return m_Renderer; }
    const Rendering::SkyRenderer& GetRenderer() const { return m_Renderer; }

    // Q6 slice 4 (§8-completion): forward to the owned SkyRenderer so its dead LUT /
    // sampler / star / UBO resources are recreated after a device rebuild.
    void OnDeviceRebuilt(Rendering::IDevice* /*device*/) override
    {
        if (m_Initialized)
            m_Renderer.ReprovisionAfterDeviceRebuild();
    }

  private:
    Rendering::SkyRenderer m_Renderer;
    Rendering::SkySettings m_Settings;
    Rendering::SkySunState m_SunState;
    bool m_HasSettings = false;
    bool m_Initialized = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
