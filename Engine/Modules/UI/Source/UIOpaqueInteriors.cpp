#include "UIOpaqueInteriors.h"

#include "UI/UIPrimitive.h"

#include <algorithm>
#include <optional>

namespace GameEngine
{
namespace UI
{
namespace
{

// The pre-pass writes every pixel of the interior. Painted rects snap to device
// pixels (SnapPaintRect); the 1 px keeps the half-pixel reach of sdfCoverage at
// an edge that does not snap out of the interior (ui_sdf.frag, sdf_functions.glsl).
constexpr float kAntiAliasInsetPx = 1.0f;
// A clip chain longer than this is malformed; its primitive is not listed.
constexpr size_t kMaxClipChainWalk = 256;

constexpr uint32_t kOpaqueRejectFlags = kPrimGradientMask | kPrimInsetShadowBit | kPrimCaretBit;

// The largest corner semi-axes, each clamped to half the extent as the shader does.
float LargestRadius(const float (&radii)[4], float extent)
{
    const float half = std::max(0.0f, extent) * 0.5f;
    float largest = 0.0f;
    for (const float radius : radii)
        largest = std::max(largest, std::clamp(radius, 0.0f, half));
    return largest;
}

// The rect where a rounded rect has coverage 1.
Mathematics::Rect FullCoverageRect(const Mathematics::Rect& rect, const float (&radii)[4],
                                   const float (&radiiY)[4])
{
    const float insetX = LargestRadius(radii, rect.Width) + kAntiAliasInsetPx;
    const float insetY = LargestRadius(radiiY, rect.Height) + kAntiAliasInsetPx;
    return Mathematics::Rect::FromEdges(rect.X + insetX, rect.Y + insetY, rect.Right() - insetX,
                                        rect.Bottom() - insetY);
}

// `rect` inset by border widths (left, top, right, bottom); a negative width insets by 0.
Mathematics::Rect InsetByBorders(const Mathematics::Rect& rect, const float (&borders)[4])
{
    return Mathematics::Rect::FromEdges(rect.X + std::max(0.0f, borders[0]),
                                        rect.Y + std::max(0.0f, borders[1]),
                                        rect.Right() - std::max(0.0f, borders[2]),
                                        rect.Bottom() - std::max(0.0f, borders[3]));
}

// The element rect the shader shades: a primitive with effects carries the
// effect reach as padding in UvRect around it (ExpandForEffects).
Mathematics::Rect ElementRect(const UIPrimitive& prim)
{
    const bool hasEffects = prim.ShadowSoftness > 0.0f || prim.GlowRadius > 0.0f ||
                            prim.ShadowOffsetX != 0.0f || prim.ShadowOffsetY != 0.0f;
    if (!hasEffects)
        return {prim.X, prim.Y, prim.W, prim.H};
    return {prim.X + prim.UvRect[0], prim.Y + prim.UvRect[1],
            prim.W - prim.UvRect[0] - prim.UvRect[2], prim.H - prim.UvRect[1] - prim.UvRect[3]};
}

// Listed: rect mode, fill alpha 255, opacity 1, no gradient, caret, inset shadow or transform.
// The interior is inset by the borders, the largest corner radius and 1 px of anti-aliasing,
// then cut by every clip of the chain, each inset the same way.
std::optional<Mathematics::Rect> OpaqueInterior(const UIPrimitive& prim,
                                                std::span<const UIClipRect> clips)
{
    if ((prim.ModeAndFlags & kPrimModeMask) != static_cast<uint32_t>(PrimitiveMode::Rect) ||
        (prim.ModeAndFlags & kOpaqueRejectFlags) != 0 || (prim.FillColor >> 24) != 0xFFu ||
        prim.Opacity != 1.0f || prim.TransformIndex != kNoTransform)
        return std::nullopt;

    const Mathematics::Rect element = ElementRect(prim);
    Mathematics::Rect interior =
        InsetByBorders(FullCoverageRect(element, prim.Radii, prim.RadiiY), prim.BorderWidths);

    uint32_t clipIndex = (prim.ModeAndFlags & kPrimClipIndexMask) >> kPrimClipIndexShift;
    for (size_t walked = 0; clipIndex != kNoClip; ++walked)
    {
        if (walked == kMaxClipChainWalk || clipIndex >= clips.size())
            return std::nullopt;
        const UIClipRect& clip = clips[clipIndex];
        if (clip.TransformIndex != kNoTransform)
            return std::nullopt;
        const Mathematics::Rect clipRect{clip.Rect[0], clip.Rect[1], clip.Rect[2], clip.Rect[3]};
        interior = interior.Intersection(FullCoverageRect(clipRect, clip.Radii, clip.RadiiY));
        clipIndex = clip.ParentIndex;
    }
    if (interior.IsEmpty())
        return std::nullopt;
    return interior;
}

} // namespace

bool UIOpaqueInteriors::Refresh(const Sources& sources)
{
    if (m_Version != 0 && sources.PrimitiveVersion == m_PrimitiveVersion &&
        sources.ClipVersion == m_ClipVersion && sources.DrawOrderVersion == m_DrawOrderVersion)
        return false;
    m_PrimitiveVersion = sources.PrimitiveVersion;
    m_ClipVersion = sources.ClipVersion;
    m_DrawOrderVersion = sources.DrawOrderVersion;
    ++m_Version;
    Build(sources);
    return true;
}

void UIOpaqueInteriors::Build(const Sources& sources)
{
    m_Interiors.clear();
    // Positions past the last depth level would share it, and an equal depth
    // cannot hide a later primitive; such a frame lists nothing.
    if (sources.DrawOrder.size() > kMaxDrawPositions)
        return;
    for (size_t position = sources.DrawOrder.size(); position-- > 0;)
    {
        const uint32_t slot = sources.DrawOrder[position];
        if (slot >= sources.Primitives.size())
            continue;
        const UIPrimitive& prim = sources.Primitives[slot];
        const std::optional<Mathematics::Rect> interior = OpaqueInterior(prim, sources.Clips);
        if (!interior)
            continue;
        UIOpaqueInterior& entry = m_Interiors.emplace_back();
        entry.Bounds = *interior;
        entry.FillColor = prim.FillColor;
        entry.DrawPosition = static_cast<uint32_t>(position);
    }
}

} // namespace UI
} // namespace GameEngine
