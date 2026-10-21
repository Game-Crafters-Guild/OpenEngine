#include "SourceFiles.h"

#include "Lexical.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace GameEngine::ComponentScanner
{
namespace fs = std::filesystem;

namespace
{

// The path in its native separators as UTF-8 bytes: the key the header list is sorted by, the
// same on every platform (fs::path::native() is a wide string on Windows).
std::string SortKey(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool LessBySortKey(const fs::path& a, const fs::path& b)
{
    return Lexical::LessIgnoreCase(SortKey(a), SortKey(b));
}

} // namespace

// A path from UTF-8 text: the command line's encoding on every platform (main converts the
// Windows wide arguments), never the Windows ANSI code page a std::string path would mean.
fs::path PathFromUtf8(std::string_view text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

// The path as UTF-8 text with forward slashes: the form the generated files and the report
// carry.
std::string Utf8(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// The absolute, lexically normalized form of a path (no symlink resolution), without a
// trailing separator. A path with no current directory to resolve it against is normalized
// as given, and the host file system resolves it: the wasm presets run the scanner under node,
// where NODERAWFS has no current directory on Windows and a drive path (C:/...) is relative
// by wasm's POSIX path rules. The non-throwing overload: the wasm build has no exception
// catching, so a throw there aborts the scanner.
fs::path FullPath(const fs::path& path)
{
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    std::string text = Utf8((ec ? path : absolute).lexically_normal());
    while (text.size() > 1 && text.back() == '/')
        text.pop_back();
    return PathFromUtf8(text);
}

// Every file under `dir` whose extension passes `accept`, sorted case-insensitively by path.
std::vector<fs::path> ListFiles(const fs::path& dir, bool (*accept)(const std::string&))
{
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        if (accept(Utf8(it->path().extension())))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(), LessBySortKey);
    return files;
}

// The file's bytes, a UTF-8 byte-order mark dropped.
bool ReadText(const fs::path& path, std::string& outText)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    outText = buffer.str();
    if (outText.size() >= 3 && static_cast<unsigned char>(outText[0]) == 0xEF &&
        static_cast<unsigned char>(outText[1]) == 0xBB && static_cast<unsigned char>(outText[2]) == 0xBF)
        outText.erase(0, 3);
    return true;
}

// The raw text split into lines ("\r\n" and "\n" both end a line), 1-based addressable as
// lines[n - 1].
std::vector<std::string> SplitLines(std::string_view raw)
{
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n')
            continue;
        if (raw[i] == '\n')
        {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(raw[i]);
    }
    lines.push_back(current);
    return lines;
}

} // namespace GameEngine::ComponentScanner
