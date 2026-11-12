#pragma once

#include "Components/RuntimeOnlyEntity.h"
#include "ECS/RequiredComponents.h"

namespace GameEngine::Components
{
// Tag for data that its owner system makes again after a world reset. No world snapshot holds it
// (ComponentFlags::NotInWorldSnapshot), and it requires RuntimeOnlyEntity, so no scene file holds it.
// @ge-no-add        applied by engine subsystems, never added by hand in the editor
// [DoNotSerialize]  the tag is runtime state; it must not appear in a saved scene
// @ge-engine-internal the engine subsystem that spawns the entity
struct GeneratedEntity
{
    static constexpr bool NotToggleable = true;
    static constexpr bool NotInWorldSnapshot = true;

    // Presence of the component is the signal; the reflection scanner reflects a component
    // only when it has at least one field.
    bool Active = true;
};

} // namespace GameEngine::Components

template <>
struct GameEngine::ECS::Detail::RequiredComponents<GameEngine::Components::GeneratedEntity>
{
    using type = TypeList<GameEngine::Components::RuntimeOnlyEntity>;
};
