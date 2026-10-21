#pragma once

#include "Rendering/Materials/MaterialBlend.h"

#include <cstdint>
#include <optional>

namespace GameEngine
{

/// How a blended material's colour joins what is behind it, read from its colour blend equation.
/// Every effect that fades a blended surface as it recedes (the particles' near fade, the fog) fades
/// it toward its blend's identity, the colour that leaves the destination unchanged, so they share
/// this one classification.
enum class BlendComposite : uint8_t
{
    /// Straight alpha over: SrcAlpha, OneMinusSrcAlpha (the default blend). The blend scales the
    /// colour by alpha.
    Over,
    /// Premultiplied alpha over: One, OneMinusSrcAlpha. The colour already carries its alpha.
    PremultipliedOver,
    /// Added with its alpha: SrcAlpha, One.
    AdditiveStraight,
    /// Added as it is: One, One.
    AdditivePremultiplied,
    /// Added with any other source factor: destination One.
    AdditiveOther,
    /// Multiplies what is behind it: DstColor, Zero or Zero, SrcColor. Its identity is 1.
    Multiply,
    /// Every other equation (Min, Max, the subtractions, every other factor pair): no identity colour.
    Other,
};

/// The composite of `authored`; an unauthored blend is the default straight alpha over.
inline BlendComposite ClassifyBlendComposite(const std::optional<MaterialBlendState>& authored)
{
    const MaterialBlendState blend = authored.value_or(MaterialBlendState{});
    if (blend.ColorOp != MaterialBlendOp::Add)
        return BlendComposite::Other;
    const MaterialBlendFactor src = blend.SrcColorFactor;
    const MaterialBlendFactor dst = blend.DstColorFactor;
    if (dst == MaterialBlendFactor::OneMinusSrcAlpha)
    {
        if (src == MaterialBlendFactor::SrcAlpha)
            return BlendComposite::Over;
        if (src == MaterialBlendFactor::One)
            return BlendComposite::PremultipliedOver;
        return BlendComposite::Other;
    }
    if (dst == MaterialBlendFactor::One)
    {
        if (src == MaterialBlendFactor::SrcAlpha)
            return BlendComposite::AdditiveStraight;
        if (src == MaterialBlendFactor::One)
            return BlendComposite::AdditivePremultiplied;
        return BlendComposite::AdditiveOther;
    }
    if ((src == MaterialBlendFactor::DstColor && dst == MaterialBlendFactor::Zero) ||
        (src == MaterialBlendFactor::Zero && dst == MaterialBlendFactor::SrcColor))
        return BlendComposite::Multiply;
    return BlendComposite::Other;
}

} // namespace GameEngine
