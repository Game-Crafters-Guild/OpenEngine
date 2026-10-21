#pragma once

#if defined(_WIN32)

#include <string>
#include <string_view>

namespace GameEngine {
namespace Platform {

/**
 * @brief Decode UTF-8 text into the UTF-16 every wide Win32 API takes.
 *
 * Each code point is decoded, so a character outside ASCII becomes its own UTF-16 unit (or
 * surrogate pair) rather than one widened unit per byte. Text that is not valid UTF-8 decodes
 * with U+FFFD in place of each invalid sequence.
 */
std::wstring Utf8ToWide(std::string_view text);

} // namespace Platform
} // namespace GameEngine

#endif // _WIN32
