#pragma once

#include <cstdlib>
#include <filesystem>

// Points GE_EDITOR_USER_DATA_ROOT (editor preferences and caches) at `root` for its lifetime and
// unsets it on destruction, also when an ASSERT leaves the test early.
class ScopedEditorUserDataRoot
{
  public:
    explicit ScopedEditorUserDataRoot(const std::filesystem::path& root)
    {
#ifdef _WIN32
        _putenv_s("GE_EDITOR_USER_DATA_ROOT", root.string().c_str());
#else
        setenv("GE_EDITOR_USER_DATA_ROOT", root.string().c_str(), 1);
#endif
    }

    ~ScopedEditorUserDataRoot()
    {
#ifdef _WIN32
        _putenv_s("GE_EDITOR_USER_DATA_ROOT", "");
#else
        unsetenv("GE_EDITOR_USER_DATA_ROOT");
#endif
    }

    ScopedEditorUserDataRoot(const ScopedEditorUserDataRoot&) = delete;
    ScopedEditorUserDataRoot& operator=(const ScopedEditorUserDataRoot&) = delete;
};
