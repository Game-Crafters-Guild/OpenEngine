#pragma once

#include "TerrainECS/TerrainHeightFunction/TerrainHeightFunctionContext.h"
#include "TerrainECS/TerrainHeightFunction/TerrainHeightRange.h"
#include "Types/Types.h"

#include <memory>
#include <span>
#include <string_view>

namespace GameEngine::TerrainECS
{

/// A planar terrain's base heights, computed by game code (Terrain Base Source "Function"). The engine
/// builds one instance per function, seed and footprint on a worker and calls both functions concurrently,
/// so they are pure functions of their arguments and of the state the constructor built.
class ITerrainHeightFunction
{
public:
    virtual ~ITerrainHeightFunction() = default;

    /// Writes the finite height at (x[i], z) to outMeters[i], in meters above the terrain's position, from
    /// terrain-local meters at the minimum corner (also past the footprint). Relief of a wavelength under
    /// `minResolvedWavelength` meters is replaced by its mean; 0 keeps all of it.
    virtual void HeightRow(float32 z, std::span<const float32> x, float32 minResolvedWavelength,
                           std::span<float32> outMeters) const = 0;

    /// The lowest and highest height in meters over [minX, maxX] x [minZ, maxZ] (terrain-local
    /// meters), at `minResolvedWavelength` and at every finer one. Conservative.
    virtual TerrainHeightRange HeightRange(float32 minX, float32 minZ, float32 maxX, float32 maxZ,
                                           float32 minResolvedWavelength) const = 0;
};

/// Builds a height function for a seed and a footprint.
using TerrainHeightFunctionFactory = std::unique_ptr<ITerrainHeightFunction> (*)(const TerrainHeightFunctionContext&);

/// Registers `factory` under `name` for the module that loads now; the newest registration of a name wins.
/// False, with an error line, when the name is empty or longer than 63 characters, or the factory is null.
bool RegisterTerrainHeightFunction(std::string_view name, TerrainHeightFunctionFactory factory);

/// The factory GE_REGISTER_TERRAIN_HEIGHT_FUNCTION registers for `Type`.
template <class Type>
std::unique_ptr<ITerrainHeightFunction> MakeTerrainHeightFunction(const TerrainHeightFunctionContext& context)
{
    return std::make_unique<Type>(context);
}

} // namespace GameEngine::TerrainECS
