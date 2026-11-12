#pragma once

namespace GameEngine::Components
{
// Tag marking an entity as engine-created runtime state that no scene file holds. WriteSceneFile
// excludes an entity with this tag. A world snapshot keeps it (the HLOD proxies). An entity that
// its owner makes again after a snapshot restore also carries GeneratedEntity.
//
// The tag is intentionally kept out of the Add Component menu — a user adding it by hand
// would silently drop the entity from every save — and is itself excluded from
// serialization (belt-and-braces: the entity-level skip already keeps it out of any file).
//
// @ge-no-add        applied by engine subsystems, never added by hand in the editor
// [DoNotSerialize]  the tag is runtime state; it must not appear in a saved scene
// @ge-engine-internal the engine subsystem that spawns the entity
struct RuntimeOnlyEntity
{
    // A marker of generator output, not a feature: it has no off state.
    static constexpr bool NotToggleable = true;

    // Presence of the component is the signal; this nominal flag exists only because the
    // reflection scanner reflects (and thus DoNotSerialize-marks) a component only when it
    // has at least one field.
    bool Active = true;
};

} // namespace GameEngine::Components
