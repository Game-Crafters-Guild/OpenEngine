#pragma once
// Sets the process working directory for the object's lifetime and restores the previous one
// afterwards, so a test can prove that a path or a file write does not depend on it.

#include <filesystem>
#include <system_error>

namespace GameEngine::TestUtils
{

class ScopedCurrentPath
{
  public:
    explicit ScopedCurrentPath(const std::filesystem::path& newCurrentPath)
    {
        std::error_code ec;
        m_PreviousPath = std::filesystem::current_path(ec);
        if (ec)
            return;
        std::filesystem::current_path(newCurrentPath, ec);
        m_Active = !ec;
    }
    ~ScopedCurrentPath()
    {
        if (!m_Active)
            return;
        std::error_code ec;
        std::filesystem::current_path(m_PreviousPath, ec);
    }
    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

    /// False when the working directory could not be read or changed; the guard then does nothing.
    bool IsActive() const { return m_Active; }

  private:
    std::filesystem::path m_PreviousPath;
    bool m_Active{false};
};

} // namespace GameEngine::TestUtils
