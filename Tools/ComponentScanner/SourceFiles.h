#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// The file-system helpers every scan shares. Paths are UTF-8 text on every platform: the
// command line's encoding (main converts the Windows wide arguments), never the Windows ANSI
// code page a std::string path would mean.

// A path from UTF-8 text.
std::filesystem::path PathFromUtf8(std::string_view text);

// The path as UTF-8 text with forward slashes: the form the generated files and the report
// carry.
std::string Utf8(const std::filesystem::path& path);

// The absolute, lexically normalized form of a path (no symlink resolution), without a
// trailing separator. A path with no current directory to resolve it against is normalized
// as given, and the host file system resolves it: the wasm presets run the scanner under node,
// where NODERAWFS has no current directory on Windows and a drive path (C:/...) is relative
// by wasm's POSIX path rules.
std::filesystem::path FullPath(const std::filesystem::path& path);

// Every file under `dir` whose extension passes `accept`, sorted case-insensitively by path.
std::vector<std::filesystem::path> ListFiles(const std::filesystem::path& dir, bool (*accept)(const std::string&));

// The file's bytes, a UTF-8 byte-order mark dropped; false when it cannot be opened.
bool ReadText(const std::filesystem::path& path, std::string& outText);

// The raw text split into lines ("\r\n" and "\n" both end a line), 1-based addressable as
// lines[n - 1].
std::vector<std::string> SplitLines(std::string_view raw);

} // namespace GameEngine::ComponentScanner
