#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{

// The speedScale last applied to the subtree of the Clip-source Animator on this entity.
// ApplyClipAnimatorSpeeds writes speedScale again only when it differs from appliedSpeedScale.
// @ge-no-add  data helper, not user-addable in the editor
// [DoNotSerialize] — runtime state; authoring lives on Animator.
// @ge-engine-internal clip animator playback
struct ClipPlaybackState
{
    static constexpr bool NotToggleable = true;

    float32 appliedSpeedScale = 1.0f;
};

} // namespace GameEngine::Components
