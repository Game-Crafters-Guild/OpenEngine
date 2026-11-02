#pragma once

#include <cstdint>
#include <span>

namespace GameEngine::Engine::Renderer
{

// World-space bounds enclosing BOTH the previous and current caster bounds.
// A departing caster must invalidate the map that still contains its old shadow.
struct ShadowCasterChangeSphere
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float Radius = 0.0f;
};

// What one caster version advance can have changed.
enum class ShadowCasterChangeScope : uint8_t
{
    Bounded,           // The spheres enclose every changed caster.
    Unbounded,         // Any caster can have changed: every light and cascade refreshes.
    VertexDeformation, // Only material vertex motion (vegetation sway): no caster moved,
                       // appeared or vanished. The spheres are empty; a consumer that
                       // cannot bound the drift treats it as Unbounded.
};

// Explains only the latest version advance. Consumers that missed a version
// must invalidate conservatively. The span is borrowed until the next notify.
struct ShadowCasterChangeSet
{
    uint64_t Version = 0;
    std::span<const ShadowCasterChangeSphere> Spheres{};
    ShadowCasterChangeScope Scope = ShadowCasterChangeScope::Unbounded;
};

} // namespace GameEngine::Engine::Renderer
