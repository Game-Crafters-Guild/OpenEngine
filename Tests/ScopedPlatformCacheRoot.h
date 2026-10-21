#pragma once
// Points the platform cache directory (VulkanPipelineCache::GetPlatformCacheDirectory and
// whatever else keys off the same variable) at a scratch tree for the object's lifetime and
// restores the variable afterwards, so a device under test never reads or writes the
// user's real pipeline cache.

#include "TestEnvVar.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace GameEngine::TestUtils
{

class ScopedPlatformCacheRoot
{
  public:
    explicit ScopedPlatformCacheRoot(const std::filesystem::path& root)
    {
        if (const char* existing = std::getenv(kVariable))
            m_Previous = existing;
        Testing::SetEnvVar(kVariable, root.string().c_str());
    }
    ~ScopedPlatformCacheRoot()
    {
        // An empty value removes the variable (TestEnvVar.h), which is the state a
        // process that never had it comes back to.
        Testing::SetEnvVar(kVariable, m_Previous.value_or(std::string{}).c_str());
    }
    ScopedPlatformCacheRoot(const ScopedPlatformCacheRoot&) = delete;
    ScopedPlatformCacheRoot& operator=(const ScopedPlatformCacheRoot&) = delete;

  private:
#if defined(_WIN32)
    static constexpr const char* kVariable = "LOCALAPPDATA";
#elif defined(__APPLE__)
    static constexpr const char* kVariable = "HOME";
#else
    static constexpr const char* kVariable = "XDG_CACHE_HOME";
#endif

    std::optional<std::string> m_Previous;
};

} // namespace GameEngine::TestUtils
