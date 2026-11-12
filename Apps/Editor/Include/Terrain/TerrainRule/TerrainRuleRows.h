#pragma once

namespace GameEngine
{

struct InspectorContext;
class TerrainMaterialLibraryAsset;

namespace Components
{
struct TerrainSurfaceRulesEffect;
} // namespace Components

namespace ECS
{
struct EntityHandle;
} // namespace ECS

// The Surface Rules effect's body: the rule list, each row's result material and
// blend mode, and each condition's range-band widget.
//
// Its own TU rather than another lambda in TerrainModifierInspectors.cpp. That
// file owns REGISTRATION — one short lambda per effect kind, plus the chrome
// every effect shares — and a rules body large enough to need foldouts, a custom
// two-handle band row and four per-condition control shapes would have made it
// the file where terrain inspector code goes rather than the file that registers
// terrain inspectors.
//
// The caller resolves `terrainEntity` and `library`. Both can be empty: a rules effect can be
// authored on a volume in a scene with no terrain yet, and the rows then show the semantic
// channel names.
void AddTerrainSurfaceRuleRows(const InspectorContext& ctx,
                               const Components::TerrainSurfaceRulesEffect& effect,
                               ECS::EntityHandle terrainEntity,
                               TerrainMaterialLibraryAsset* library);

} // namespace GameEngine
