#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine
{

// `path` spelled relative to `root` when it lies strictly inside `root`, and an
// empty path otherwise: when `root` is empty, when `path` is `root` itself, when
// the relative spelling would walk out through `..`, or when the two share no
// root name. Purely lexical over both paths normalised — neither has to exist —
// and case-sensitive, as std::filesystem::path is. The walk-out test compares
// path elements, not characters, so a name that merely starts with two dots is
// inside, and the test reads the same on every platform whatever
// path::native() is spelled in.
inline std::filesystem::path RelativePathUnderRoot(const std::filesystem::path& path, const std::filesystem::path& root)
{
    if (root.empty())
        return {};
    const std::filesystem::path relative =
        path.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty() || relative == "." || *relative.begin() == "..")
        return {};
    return relative;
}

/// The path that `utf8`, a UTF-8 string such as a zip entry name or a name from a JSON
/// file, spells. A path built from a std::string reads it in the active code page on
/// Windows, so a non-ASCII name becomes other characters; this reads it as UTF-8 on every
/// platform.
inline std::filesystem::path PathFromUtf8(std::string_view utf8)
{
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

/// `path` as UTF-8 in its generic form ('/' separators), for a message, a log line or a
/// process argument. path::string() and path::generic_string() convert through the active code
/// page on Windows: they change a non-ASCII name to other characters, and they throw when the
/// code page has no character for one. This conversion never throws and is the same on every
/// platform.
inline std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}

} // namespace GameEngine
