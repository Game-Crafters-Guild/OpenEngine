#pragma once

#include "AssetCore/GUID.h"

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace GameEngine {

/**
 * @brief The textures whose import cook is running now: what the editor counts in
 * its "Preparing N textures" status and asks for one texture whose first load has
 * not landed yet.
 *
 * A cook is entered with Begin on a cook-cache miss inside an AssetManager load
 * (TextureAsset::TryLoadViaCook) and left when its Scope ends, once the cooked
 * payload is loaded. A cache hit, a raw decode and an export's cook never enter it.
 * Thread-safe.
 */
class TextureCookActivity
{
  public:
    /// Marks one texture as cooking for its lifetime.
    class [[nodiscard]] Scope
    {
      public:
        Scope(Scope&& other) noexcept
            : m_Activity(std::exchange(other.m_Activity, nullptr)), m_Texture(other.m_Texture)
        {
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope& operator=(Scope&&) = delete;
        ~Scope();

      private:
        friend class TextureCookActivity;
        Scope(TextureCookActivity& activity, const GUID& texture) : m_Activity(&activity), m_Texture(texture) {}

        TextureCookActivity* m_Activity;
        GUID m_Texture;
    };

    /// Marks `texture` as cooking until the returned scope ends.
    Scope Begin(const GUID& texture);

    /// The number of distinct textures cooking now: a snapshot, stale once returned.
    size_t CookingCount() const;

    /// True while a cook of `texture` runs: a snapshot, stale once returned.
    bool IsCooking(const GUID& texture) const;

  private:
    void End(const GUID& texture);

    mutable std::mutex m_Mutex;
    // One entry per cooking texture with its number of running cooks: a reload's cook can
    // overlap the first load's for the same texture. Guarded by m_Mutex.
    std::vector<std::pair<GUID, size_t>> m_Cooking;
};

} // namespace GameEngine
