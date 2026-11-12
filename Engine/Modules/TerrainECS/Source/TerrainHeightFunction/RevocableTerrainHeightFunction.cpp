#include "TerrainECS/TerrainHeightFunction/RevocableTerrainHeightFunction.h"

#include "TerrainECS/TerrainHeightFunction/ITerrainHeightFunction.h"

namespace GameEngine::TerrainECS
{
namespace
{

// Holds one in-flight count for a call's duration. The last call out after a revoke wakes Revoke.
class InFlightCall
{
public:
    InFlightCall(std::atomic<uint32>& count, const std::atomic<bool>& revoked)
        : m_Count(count)
        , m_Revoked(revoked)
    {
        m_Count.fetch_add(1u);
    }
    ~InFlightCall()
    {
        if (m_Count.fetch_sub(1u) == 1u && m_Revoked.load())
            m_Count.notify_all();
    }

    InFlightCall(const InFlightCall&) = delete;
    InFlightCall& operator=(const InFlightCall&) = delete;

private:
    std::atomic<uint32>& m_Count;
    const std::atomic<bool>& m_Revoked;
};

} // namespace

RevocableTerrainHeightFunction::RevocableTerrainHeightFunction(std::unique_ptr<ITerrainHeightFunction> function)
    : m_Function(std::move(function))
{
}

RevocableTerrainHeightFunction::~RevocableTerrainHeightFunction() = default;

bool RevocableTerrainHeightFunction::HeightRow(float32 z, std::span<const float32> x, float32 minResolvedWavelength,
                                               std::span<float32> outMeters) const
{
    const InFlightCall call(m_InFlight, m_Revoked);
    if (m_Revoked.load())
        return false;
    m_Function->HeightRow(z, x, minResolvedWavelength, outMeters);
    return true;
}

std::optional<TerrainHeightRange> RevocableTerrainHeightFunction::HeightRange(
    float32 minX, float32 minZ, float32 maxX, float32 maxZ, float32 minResolvedWavelength) const
{
    const InFlightCall call(m_InFlight, m_Revoked);
    if (m_Revoked.load())
        return std::nullopt;
    return m_Function->HeightRange(minX, minZ, maxX, maxZ, minResolvedWavelength);
}

void RevocableTerrainHeightFunction::Revoke()
{
    if (m_Revoked.exchange(true))
        return;
    for (uint32 inFlight = m_InFlight.load(); inFlight != 0u; inFlight = m_InFlight.load())
        m_InFlight.wait(inFlight);
    m_Function.reset();
}

} // namespace GameEngine::TerrainECS
