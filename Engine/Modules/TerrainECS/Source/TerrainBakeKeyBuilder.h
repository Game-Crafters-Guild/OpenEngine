#pragma once

#include "Types/Types.h"

#include <cstddef>
#include <type_traits>

namespace GameEngine::TerrainECS
{

/// Version of a terrain's modifier bake output. Bump it with every change that can alter a baked
/// height or splat texel for the same inputs. It seeds every bake key, so a bump makes every stored
/// surface page a miss.
inline constexpr uint32 kTerrainBakeVersion = 8u;

/// A terrain bake's content key: FNV-1a 64 over the bytes fed in, in order, seeded with
/// kTerrainBakeVersion. Feed only portable content (authored values, payload and asset bytes), never
/// a handle, pointer or per-process counter: two processes must agree on the key.
class TerrainBakeKeyBuilder
{
public:
    TerrainBakeKeyBuilder();

    void Bytes(const void* data, std::size_t size);

    template <typename T>
    void Value(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T> && std::has_unique_object_representations_v<T>,
                      "feed a padding-free value; pad-carrying structs must be fed field by field");
        Bytes(&value, sizeof(T));
    }

    void Float(float32 value) { Bytes(&value, sizeof(value)); }

    uint64 Key() const { return m_Hash; }

private:
    uint64 m_Hash;
};

} // namespace GameEngine::TerrainECS
