#pragma once
// The component mirror fixture: one component of each class.

#include "Components/Kinds.h"

#include <cstdint>

namespace GameEngine::Components
{

/// A note the editor keeps on an entity.
// @ge-editor-only
struct FixtureEditorNote
{
    std::uint32_t Revision = 0;
};

/// The GPU slot the engine gives a probe.
// @ge-engine-internal
struct FixtureGpuSlot
{
    std::uint32_t Slot = 0;
};

/// The clock the engine runs for a probe. A script reads it.
// @ge-readonly
struct FixtureClock
{
    double Elapsed = 0.0;
    FixtureGlow Phase = FixtureGlow::Off;
    char Name[8] = {};
    std::uint32_t Ticks[2] = {};
    // @ge-hidden
    std::uint32_t Cookie = 0;
};

/// Marks a probe the player has picked up.
// @ge-tag
struct FixturePickedUp
{
};

/// A struct with no fields and no tag marker: a parameter, not a component, so it has no mirror.
struct FixtureQueryParameter
{
};

} // namespace GameEngine::Components
