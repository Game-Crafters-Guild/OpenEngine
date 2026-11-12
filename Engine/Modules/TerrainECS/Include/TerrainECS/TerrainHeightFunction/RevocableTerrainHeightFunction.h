#pragma once

#include "TerrainECS/TerrainHeightFunction/TerrainHeightRange.h"
#include "Types/Types.h"

#include <atomic>
#include <memory>
#include <optional>
#include <span>

namespace GameEngine::TerrainECS
{
class ITerrainHeightFunction;

/// A built height function that its module's unload can revoke while fills run, with no lock on the
/// fill path. A call counts itself in flight, then reads the revoked flag (sequentially consistent):
/// Revoke sets the flag and destroys the function only after every call that read "not revoked" ends.
class RevocableTerrainHeightFunction
{
public:
    explicit RevocableTerrainHeightFunction(std::unique_ptr<ITerrainHeightFunction> function);
    ~RevocableTerrainHeightFunction();

    RevocableTerrainHeightFunction(const RevocableTerrainHeightFunction&) = delete;
    RevocableTerrainHeightFunction& operator=(const RevocableTerrainHeightFunction&) = delete;

    /// ITerrainHeightFunction::HeightRow. False, with `outMeters` untouched, once revoked.
    bool HeightRow(float32 z, std::span<const float32> x, float32 minResolvedWavelength,
                   std::span<float32> outMeters) const;
    /// ITerrainHeightFunction::HeightRange. Empty once revoked.
    std::optional<TerrainHeightRange> HeightRange(float32 minX, float32 minZ, float32 maxX, float32 maxZ,
                                                  float32 minResolvedWavelength) const;

    /// Main thread. Waits for the calls in flight, then destroys the function.
    void Revoke();

private:
    std::unique_ptr<ITerrainHeightFunction> m_Function;
    mutable std::atomic<uint32> m_InFlight{0};
    std::atomic<bool> m_Revoked{false};
};

} // namespace GameEngine::TerrainECS
