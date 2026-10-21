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

} // namespace Platform
} // namespace GameEngine

#endif // _WIN32
