#include "TerrainBakeKeyBuilder.h"

#include "Types/Fnv1a.h"

namespace GameEngine::TerrainECS
{

TerrainBakeKeyBuilder::TerrainBakeKeyBuilder()
    : m_Hash(Hashing::Fnv1a64Value(Hashing::kFnv1a64OffsetBasis, kTerrainBakeVersion))
{
}

void TerrainBakeKeyBuilder::Bytes(const void* data, std::size_t size)
{
    m_Hash = Hashing::Fnv1a64(data, size, m_Hash);
}

} // namespace GameEngine::TerrainECS
