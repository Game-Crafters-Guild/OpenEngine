#pragma once

namespace GameEngine::Rendering
{
struct SkySettings;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{

/// What the sky pass draws behind the scene for a frame's sky settings.
enum class SkyBackdropKind
{
    /// The physical atmosphere: the sky-view LUT plus the ground and night composite.
    Atmosphere,
    /// An equirectangular HDRI sampled by view direction; the LUT work is unused.
    Hdri,
    /// The stylized three-colour gradient; the LUT work is unused.
    Gradient,
};

/// An HDRI wins over a gradient sky if both somehow resolve (a gradient sky never sets an
/// environment texture).
SkyBackdropKind ClassifySkyBackdrop(const Rendering::SkySettings& settings);

} // namespace GameEngine::Engine::Renderer
