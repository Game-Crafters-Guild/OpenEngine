#pragma once

#include "AssetCore/GUID.h"

namespace GameEngine::Components
{

// Marks the root of a placed prefab instance. Source is the GUID of the .prefab asset the
// instance follows; the scene saver finds the file's path through the asset database and writes
// the [prefab] section with both. The instance's other entities carry nothing new: they are
// members by their place under this root and their scene identity tags.
// @ge-no-add  placed by the scene loader, not user-addable in the editor
// [DoNotSerialize] SceneIO writes the [prefab] section from this marker; reflection would emit invalid property lines.
// @ge-engine-internal scene loading
struct PrefabInstance
{
    GUID Source{}; // @ge-hidden  the web types have no GUID kind, and no page reads the marker
};

static_assert(sizeof(PrefabInstance) == 16, "PrefabInstance is one GUID");

} // namespace GameEngine::Components
