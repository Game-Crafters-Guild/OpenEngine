#include "Rendering/Materials/ReflectionCache.h"
#include "Types/Fnv1a.h"
#include <functional>
#include <algorithm>
#include <utility>

namespace GameEngine { namespace Rendering {

namespace {
// A separator byte after each string, so that two keys whose strings only split differently hash apart.
constexpr uint8_t kStagePathEnd = 0xFF;
constexpr uint8_t kStageNameEnd = 0xFE;
constexpr uint8_t kEntryPointEnd = 0xFD;
} // namespace

size_t ReflectionCacheKeyHash::operator()(const ReflectionCacheKey& key) const noexcept {
    using namespace GameEngine::Hashing;
    uint64_t hash = kFnv1a64OffsetBasis;
    for (const std::string& path : key.StagePaths) {
        hash = Fnv1a64Value(Fnv1a64(path, hash), kStagePathEnd);
    }
    for (const auto& [stage, entryPoint] : key.StageEntryPoints) {
        hash = Fnv1a64Value(Fnv1a64(stage, hash), kStageNameEnd);
        hash = Fnv1a64Value(Fnv1a64(entryPoint, hash), kEntryPointEnd);
    }
    hash = Fnv1a64Values(hash, key.SpecConstantIds);
    return static_cast<size_t>(Fnv1a64Value(hash, key.ToolVersion));
}

const ShaderMeta* ReflectionCache::Find(const ReflectionCacheKey& key) const {
    auto it = m_Cache.find(key); if (it == m_Cache.end()) return nullptr; return &it->second;
}

void ReflectionCache::Put(const ReflectionCacheKey& key, ShaderMeta meta) {
    m_Cache.emplace(key, std::move(meta));
}

}} // namespace GameEngine::Rendering

