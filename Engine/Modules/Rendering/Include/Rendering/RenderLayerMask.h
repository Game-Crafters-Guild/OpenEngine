#pragma once

#include "Types/Types.h"

#include <cassert>

namespace GameEngine::Rendering
{

/// The render layers a renderer belongs to, or the layers a view or a volume takes.
///
/// One bit per layer, 32 layers. A renderer is drawn by a view when its layers intersect the view's.
/// Layer 0 is the default layer. The mask is a plain 32-bit word: `GPUInstance` and `GPUCullingData`
/// carry `Bits` verbatim and the culling kernels test it with one AND.
///
/// Hiding an object is the entity's enable switch, never an empty mask: a renderer whose mask is
/// `None()` is drawn by no view.
struct RenderLayerMask
{
    static constexpr uint32 kCount = 32;

    uint32 Bits = 1u;

    /// Layer 0 only.
    static constexpr RenderLayerMask Default() { return {1u}; }
    /// Every layer.
    static constexpr RenderLayerMask All() { return {0xFFFFFFFFu}; }
    /// No layer.
    static constexpr RenderLayerMask None() { return {0u}; }

    /// The mask of one layer. `layer` is 0 to 31.
    static constexpr RenderLayerMask FromLayer(uint32 layer)
    {
        assert(layer < kCount && "RenderLayerMask::FromLayer: a layer is 0 to 31");
        return {1u << layer};
    }

    /// Whether `layer` (0 to 31) is set.
    constexpr bool Contains(uint32 layer) const { return layer < kCount && ((Bits >> layer) & 1u) != 0u; }

    /// Whether the two masks share a layer.
    constexpr bool Intersects(RenderLayerMask other) const { return (Bits & other.Bits) != 0u; }

    /// The layers of both masks.
    constexpr RenderLayerMask operator|(RenderLayerMask other) const { return {Bits | other.Bits}; }

    constexpr bool operator==(const RenderLayerMask&) const = default;
};

static_assert(sizeof(RenderLayerMask) == sizeof(uint32), "RenderLayerMask is copied verbatim into the GPU instance word");

} // namespace GameEngine::Rendering
