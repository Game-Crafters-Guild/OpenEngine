#pragma once

#include "AssetCore/AssetRegistry.h"

namespace GameEngine
{

// Registers the editor's asset type factories: UI layouts and graphs. A UIStyle asset comes from
// UIStyleAssetParser, which gives a UI module control sheet its cascade origin.
void RegisterEditorAssetTypes(AssetTypeRegistry& typeRegistry);

} // namespace GameEngine
