#pragma once

#include "Mathematics/Rect.h"

#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine
{
namespace UI
{

struct UIPrimitive;
struct UIClipRect;

/// One opaque interior of the UI draw list, as the pre-pass reads it
/// (ui_opaque.vert, binding 0). Bounds are in target pixels.
struct alignas(16) UIOpaqueInterior
{
    Mathematics::Rect Bounds;
    uint32_t FillColor = 0;    // packed RGBA8, alpha 255
    uint32_t DrawPosition = 0; // index in the draw order; sets the depth
    uint32_t Pad[2] = {0, 0};
};
static_assert(sizeof(UIOpaqueInterior) == 32, "UIOpaqueInterior must be 32 bytes (2 x vec4)");

/// The screen rects where ui_sdf.frag paints exactly a primitive's fill color, by draw position.
/// The UI pass draws them front to back into a depth target; the main draw then shades only
/// what lies above the topmost one. Rules for a listed rect: UIOpaqueInteriors.cpp.
class UIOpaqueInteriors
{
  public:
    /// The draw positions one D16 depth target separates.
    static constexpr size_t kMaxDrawPositions = 65534;

    struct Sources
    {
        std::span<const UIPrimitive> Primitives;
        std::span<const UIClipRect> Clips;
        std::span<const uint32_t> DrawOrder;
        uint64_t PrimitiveVersion = 0;
        uint64_t ClipVersion = 0;
        uint64_t DrawOrderVersion = 0;
    };

    /// Rebuilds the list when a source version moved since the last build.
    /// Returns true when it rebuilt.
    bool Refresh(const Sources& sources);

    /// Front to back: the last draw position first.
    const std::vector<UIOpaqueInterior>& Interiors() const { return m_Interiors; }

    /// Moves on every rebuild; keys the upload of Interiors().
    uint64_t Version() const { return m_Version; }

  private:
    void Build(const Sources& sources);

    std::vector<UIOpaqueInterior> m_Interiors;
    uint64_t m_Version = 0;
    uint64_t m_PrimitiveVersion = 0;
    uint64_t m_ClipVersion = 0;
    uint64_t m_DrawOrderVersion = 0;
};

} // namespace UI
} // namespace GameEngine
