// Reflection for NavigationSurface: the Add Component entry, the inspector's reflected rows, the
// scene file and set_component all read this table. The component lives under the module's own
// include tree, which the build-time component scanner does not read, so it is registered here.

#include "PathfindingECS/Components/NavigationSurface.h"

#include "Components/ComponentRegistration.h"
#include "Components/MathReflectionTraits.h"

#include <limits>

namespace
{
constexpr auto kRuntimeField = ::GameEngine::ECS::FieldFlags::Transient | ::GameEngine::ECS::FieldFlags::Hidden;
} // namespace

GE_REFLECT_ENUM_TABLE_BEGIN(NavigationSurfaceMode)
    GE_REFLECT_ENUM_TABLE_VALUE("Mesh", 0)
    GE_REFLECT_ENUM_TABLE_VALUE("Grid", 1)
GE_REFLECT_ENUM_TABLE_END()

GE_REFLECT_ENUM_TABLE_BEGIN(NavigationGridType)
    GE_REFLECT_ENUM_TABLE_VALUE("Square", 0)
    GE_REFLECT_ENUM_TABLE_VALUE("Hex", 1)
GE_REFLECT_ENUM_TABLE_END()

GE_REGISTER_COMPONENT_BEGIN(GameEngine::Components::NavigationSurface)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, Mode)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, GridType)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, AgentRadius)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, AgentHeight)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, MaxSlope)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, StepHeight)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, CellSize)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, FitBounds)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, Center)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, Extent)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, AuthoredCells)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, NavMapIndex)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, NavMapGeneration)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, OwnerWorldId)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BakeRequested)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BakePhase)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BakeFailure)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BakeWait)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, SettingsChanged)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BakedMode)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BakeDone)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BakeTotal)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BakedWalkable)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BakedBytes)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BakeVersion)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BlockingTileX)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BlockingTileZ)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSurface, BlockingModel)
GE_REGISTER_COMPONENT_END(GameEngine::Components::NavigationSurface)

GE_REFLECT_ENUM_FIELD(GameEngine::Components::NavigationSurface, Mode, NavigationSurfaceMode)
GE_REFLECT_ENUM_FIELD(GameEngine::Components::NavigationSurface, GridType, NavigationGridType)

GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationSurface, AgentRadius, 0.01f, std::numeric_limits<float>::infinity())
GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationSurface, AgentHeight, 0.01f, std::numeric_limits<float>::infinity())
GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationSurface, MaxSlope, 0.0f, 89.0f)
GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationSurface, StepHeight, 0.0f, std::numeric_limits<float>::infinity())
GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationSurface, CellSize, 0.05f, std::numeric_limits<float>::infinity())

GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSurface, Mode,
                         "Mesh: a navigation mesh agents walk anywhere their size fits. Grid: one height and one "
                         "walkable flag per cell, for per-cell costs, reservations and hex cells.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSurface, GridType, "Square or hex cells.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSurface, AgentRadius,
                         "Meters. The surface keeps this far from walls and drops gaps narrower than twice it.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSurface, AgentHeight,
                         "Meters. Space under an overhang lower than this is not walkable.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSurface, MaxSlope,
                         "Degrees. Ground steeper than this is not walkable.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSurface, StepHeight,
                         "Meters. The tallest step an agent climbs without a slope.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSurface, CellSize,
                         "Meters. The surface's resolution: a mesh is voxelized at this size and cut into tiles of 64 "
                         "cells; a grid has one cell of this size. Smaller follows the ground more closely and bakes "
                         "longer.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSurface, FitBounds,
                         "Bake over the terrain's footprint, or over everything the bake reads when there is no "
                         "terrain. Off: bake inside Center and Extent.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSurface, Center,
                         "Meters from the entity's position to the middle of the baked box.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSurface, Extent, "Meters: half the baked box's size.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSurface, AuthoredCells,
                         "Grid only: the cell costs and blocked cells painted with the navigation grid brush (.navgrid).")

GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, NavMapIndex, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, NavMapGeneration, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, OwnerWorldId, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BakeRequested, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BakePhase, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BakeFailure, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BakeWait, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, SettingsChanged, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BakedMode, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BakeDone, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BakeTotal, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BakedWalkable, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BakedBytes, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BakeVersion, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BlockingTileX, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BlockingTileZ, kRuntimeField)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationSurface, BlockingModel, kRuntimeField)
