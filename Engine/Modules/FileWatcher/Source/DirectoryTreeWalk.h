#pragma once

#include "Types/Types.h"

#include <dirent.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

/**
 * @brief Walks a directory tree in pre-order with opendir and readdir, in the order of
 * std::filesystem::recursive_directory_iterator. It keeps its buffers between walks, so
 * with libc++ a walk of a tree that it walked before makes no operator new call.
 */
class DirectoryTreeWalk
{
  public:
    enum class EntryType : uint8
    {
        Directory,
        RegularFile,
        Symlink,
        Other,
    };

    DirectoryTreeWalk() = default;
    ~DirectoryTreeWalk();

    /**
     * @brief Open root and close what a previous walk left open.
     * Throws std::filesystem::filesystem_error when root cannot be opened.
     */
    void Begin(const std::filesystem::path& root);

    /**
     * @brief Move to the next entry, and close each directory that has no more entries.
     * @return false when the walk is complete.
     * Throws std::filesystem::filesystem_error when a directory cannot be read.
     */
    bool Next();

    /**
     * @brief Open the current entry, a directory, so that Next continues inside it.
     * Throws std::filesystem::filesystem_error when the directory cannot be opened.
     */
    void Descend();

    /**
     * @brief Close every open directory.
     */
    void Close();

    EntryType CurrentType() const { return m_CurrentType; }

    /**
     * @brief The current entry's path: the root joined with the names under it, as
     * std::filesystem::path::operator/ joins them.
     */
    const std::filesystem::path& CurrentPath() const { return m_CurrentPath; }

    std::string_view CurrentName() const { return std::string_view(m_PathText).substr(m_NameStart); }

    /**
     * @brief The current entry's path under the root, with '/' between the names.
     */
    std::string_view CurrentRelativePath() const { return std::string_view(m_PathText).substr(m_RelativeStart); }

  private:
    struct OpenDirectory
    {
        DIR* Stream;
        // The length of this directory's path in m_PathText.
        size_t PathLength;
    };

    void AppendSeparatorIfNeeded();
    [[noreturn]] void Fail(const char* operation, size_t pathLength, int error);

    std::string m_PathText;
    std::filesystem::path m_CurrentPath;
    std::vector<OpenDirectory> m_OpenDirectories;
    size_t m_RelativeStart = 0;
    size_t m_NameStart = 0;
    EntryType m_CurrentType = EntryType::Other;

    DISALLOW_COPY_AND_ASSIGN(DirectoryTreeWalk);
};

} // namespace GameEngine
