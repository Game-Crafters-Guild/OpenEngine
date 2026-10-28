#if defined(_WIN32)

#include "Utf8ToWide_Win32.h"

#include <windows.h>

namespace GameEngine {
namespace Platform {

std::wstring Utf8ToWide(std::string_view text)
{
    if (text.empty())
        return {};

    const int sourceLength = static_cast<int>(text.size());
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceLength, nullptr, 0);
    if (length <= 0)
        return {};

    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceLength, wide.data(), length);
    return wide;
}

std::string WideToUtf8(std::wstring_view text)
{
    if (text.empty())
        return {};

    const int sourceLength = static_cast<int>(text.size());
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), sourceLength, nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};

    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), sourceLength, utf8.data(), length, nullptr, nullptr);
    return utf8;
}

} // namespace Platform
} // namespace GameEngine

#endif // _WIN32
