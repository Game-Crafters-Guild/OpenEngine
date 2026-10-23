// Whether the deforming-motion producer can write a view, and why not.
//
// Private to the producer: the declaration asks it once per view, and the
// producer tests read it directly.
#pragma once

#include <cstdint>

namespace GameEngine::Engine::Renderer
{

/// Why the producer writes no deforming motion for a view this frame.
///
/// Every refusal has the same fallback: the producer records nothing for the
/// view, and the mover lane owns the target exactly as it does on a view with
/// nothing deforming. It clears the target to the sentinel and writes the
/// movers, so the deforming surfaces reproject from the camera alone.
enum class DeformationMotionRefusal : uint8_t
{
    /// The view can be produced for.
    None = 0,
    /// The renderer runs the compatibility profile. Its cooked material set has
    /// no motion variant, so a draw would ask for a shader that was never made.
    CompatibilityProfile,
    /// The view renders into a letterboxed sub-rectangle of its target. The
    /// mover lane writes nothing on such a view either, and neither consumer
    /// is proven to read a delta measured in a sub-rectangle, so no lane
    /// writes exact motion there.
    Letterboxed,
    /// The view's depth is multisampled. The payload has no correct multisample
    /// resolve: averaging two surfaces' motion describes neither.
    Multisampled,
    /// No material in the view moves its vertices in a form the producer can
    /// follow.
    NoDeformingMaterial,
    /// The scatter has not allocated the view's rendered level and phase
    /// history yet, so per-instance validity cannot be read. Expected on the
    /// frames before the allocation lands.
    NoRenderedHistory,
};

/// What the producer knows about a view before it declares anything.
struct DeformationMotionViewState
{
    bool CompatibilityProfile = false;
    bool Letterboxed = false;
    uint32_t DepthSampleCount = 1;
    bool HasDeformingBatchKeys = false;
    bool HasRenderedHistory = false;
};

/// The first reason the producer cannot write a view, or `None`. "Nothing in
/// the view deforms" is checked first, so only a view that has something to
/// produce is refused for its profile or its shape.
DeformationMotionRefusal ClassifyDeformationMotionView(const DeformationMotionViewState& view);

/// The refusal in plain words, for the one log line a refused view gets.
const char* DeformationMotionRefusalReason(DeformationMotionRefusal refusal);

} // namespace GameEngine::Engine::Renderer
