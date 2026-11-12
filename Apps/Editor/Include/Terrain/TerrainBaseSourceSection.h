#pragma once

namespace GameEngine
{

struct InspectorContext;
namespace Components
{
struct Terrain;
}

// The Terrain inspector's Base section: where a planar terrain's heights come from before its
// modifiers apply (flat with the Generated noise's relief, an imported heightmap, or a registered
// function) and, for a heightmap, which asset and whether it decoded.
void AddTerrainBaseSourceSection(const InspectorContext& ctx);

// True for a Function base whose name is empty or that no loaded module registers: the base is flat.
bool IsFlatFunctionBase(const Components::Terrain& terrain);

} // namespace GameEngine
