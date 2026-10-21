#include "Engine/Rendering/SkyBackdrop.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "Rendering/Sky/SkySettings.h"

namespace GameEngine::Engine::Renderer
{

SkyBackdropKind ClassifySkyBackdrop(const Rendering::SkySettings& settings)
{
    if (settings.environmentTexture.IsValid())
        return SkyBackdropKind::Hdri;
    if (settings.skyMode == static_cast<uint32_t>(Components::SkyMode::Gradient))
        return SkyBackdropKind::Gradient;
    return SkyBackdropKind::Atmosphere;
}

} // namespace GameEngine::Engine::Renderer
