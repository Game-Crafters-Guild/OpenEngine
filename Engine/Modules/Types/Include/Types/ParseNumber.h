#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Detect whether std::from_chars supports float/double on this platform.
// MSVC and GCC 11+ (libstdc++) provide full charconv; Apple Clang does not.
#include <charconv>
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    #define GE_FLOAT_CHARCONV_AVAILABLE 1
#elif defined(_MSC_VER) && _MSC_VER >= 1924
    #define GE_FLOAT_CHARCONV_AVAILABLE 1
#endif

#ifndef GE_FLOAT_CHARCONV_AVAILABLE
    #include <cerrno>
    #include <cstdlib>
    #include <cstring>
    #ifdef __APPLE__
        #include <xlocale.h>
    #else
        #include <locale.h>
    #endif
#endif

namespace GameEngine
{

// The outcome of a locale-independent number parse.
enum class ParseNumberStatus : uint8_t
{
    Ok,
    // No number starts the text.
    NoNumber,
    // The text is a number, but past the type's range: an overflow, or an underflow that would
    // round to zero. A subnormal is in range.
    OutOfRange,
};

// Locale-independent float parse of the number at the start of `s`. On Ok, outValue holds the
// value and charsConsumed the count of characters that were part of the number (callers can use
// this to inspect trailing suffixes like "px" or "%"); on any other status both are unchanged.
inline ParseNumberStatus ParseFloat(std::string_view s, float& outValue, size_t& charsConsumed)
{
    if (s.empty())
        return ParseNumberStatus::NoNumber;

#ifdef GE_FLOAT_CHARCONV_AVAILABLE
    float result = 0.0f;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    if (ec == std::errc::result_out_of_range)
        return ParseNumberStatus::OutOfRange;
    if (ec != std::errc{})
        return ParseNumberStatus::NoNumber;
    outValue = result;
    charsConsumed = static_cast<size_t>(ptr - s.data());
    return ParseNumberStatus::Ok;
#else
    constexpr size_t kBufSize = 128;
    char buf[kBufSize];
    const char* str;
    std::string heap;
    if (s.size() < kBufSize)
    {
        std::memcpy(buf, s.data(), s.size());
        buf[s.size()] = '\0';
        str = buf;
    }
    else
    {
        heap.assign(s.data(), s.size());
        str = heap.c_str();
    }

    static locale_t cLocale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    char* end = nullptr;
    errno = 0;
    float result = strtof_l(str, &end, cLocale);
    if (end == str)
        return ParseNumberStatus::NoNumber;
    // libc may set ERANGE for a subnormal it represents; from_chars accepts those.
    if (errno == ERANGE && (result == 0.0f || std::isinf(result)))
        return ParseNumberStatus::OutOfRange;
    outValue = result;
    charsConsumed = static_cast<size_t>(end - str);
    return ParseNumberStatus::Ok;
#endif
}

// ParseFloat above for double.
inline ParseNumberStatus ParseDouble(std::string_view s, double& outValue, size_t& charsConsumed)
{
    if (s.empty())
        return ParseNumberStatus::NoNumber;

#ifdef GE_FLOAT_CHARCONV_AVAILABLE
    double result = 0.0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    if (ec == std::errc::result_out_of_range)
        return ParseNumberStatus::OutOfRange;
    if (ec != std::errc{})
        return ParseNumberStatus::NoNumber;
    outValue = result;
    charsConsumed = static_cast<size_t>(ptr - s.data());
    return ParseNumberStatus::Ok;
#else
    constexpr size_t kBufSize = 128;
    char buf[kBufSize];
    const char* str;
    std::string heap;
    if (s.size() < kBufSize)
    {
        std::memcpy(buf, s.data(), s.size());
        buf[s.size()] = '\0';
        str = buf;
    }
    else
    {
        heap.assign(s.data(), s.size());
        str = heap.c_str();
    }

    static locale_t cLocale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    char* end = nullptr;
    errno = 0;
    double result = strtod_l(str, &end, cLocale);
    if (end == str)
        return ParseNumberStatus::NoNumber;
    // libc may set ERANGE for a subnormal it represents; from_chars accepts those.
    if (errno == ERANGE && (result == 0.0 || std::isinf(result)))
        return ParseNumberStatus::OutOfRange;
    outValue = result;
    charsConsumed = static_cast<size_t>(end - str);
    return ParseNumberStatus::Ok;
#endif
}

// Locale-independent float parse. Returns the value on success, nullopt when no number starts
// the text or it is out of a float's range. If charsConsumed is non-null, stores the number of
// characters that were part of the parsed number.
inline std::optional<float> ParseFloat(std::string_view s, size_t* charsConsumed = nullptr)
{
    float value = 0.0f;
    size_t consumed = 0;
    if (ParseFloat(s, value, consumed) != ParseNumberStatus::Ok)
        return std::nullopt;
    if (charsConsumed)
        *charsConsumed = consumed;
    return value;
}

inline std::optional<double> ParseDouble(std::string_view s, size_t* charsConsumed = nullptr)
{
    double value = 0.0;
    size_t consumed = 0;
    if (ParseDouble(s, value, consumed) != ParseNumberStatus::Ok)
        return std::nullopt;
    if (charsConsumed)
        *charsConsumed = consumed;
    return value;
}

// The number at the start of `text` as std::stof reads one, without its throw: leading ASCII
// space and one '+' are skipped, then ParseFloat reads the number, locale-independent. With
// std::from_chars for float that reads decimal numbers, inf, infinity and nan as std::stof does,
// and no hex, which std::stof takes; where ParseFloat falls back to strtof_l, hex is read too.
// Nullopt when no number starts the text or it is out of a float's range, where std::stof throws.
// If charsConsumed is non-null, it counts the characters read, the leading space and sign
// included. For authored data whose parse must never throw: the web build has no exception
// catching.
inline std::optional<float> ParseLeadingFloat(std::string_view text, size_t* charsConsumed = nullptr)
{
    size_t skipped = 0;
    while (skipped < text.size() && std::isspace(static_cast<unsigned char>(text[skipped])))
        ++skipped;
    if (skipped < text.size() && text[skipped] == '+')
    {
        ++skipped;
        // std::stof refuses a second sign after '+'; ParseFloat would read the '-'.
        if (skipped < text.size() && text[skipped] == '-')
            return std::nullopt;
    }
    size_t consumed = 0;
    const std::optional<float> value = ParseFloat(text.substr(skipped), &consumed);
    if (value && charsConsumed)
        *charsConsumed = skipped + consumed;
    return value;
}

// Strict decimal parse: the intersection of what std::from_chars and strtod_l
// accept, so a string parses identically on every platform. ParseFloat and
// ParseDouble above differ across platforms at the edges — strtod_l takes hex
// ("0x10") and inf/nan, from_chars does not — and both stop at the first
// character they cannot use. This one refuses hex, a leading '+', inf and nan,
// refuses anything left over after the number, and refuses a non-finite
// result. Surrounding ASCII space is allowed and trimmed.
//
// Use it wherever a string has to mean the same number on Windows and macOS:
// authored values, serialized fields, anything a user typed.
inline bool ParseStrictDecimal(std::string_view text, double& outValue)
{
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    std::string_view number = text.substr(begin, end - begin);
    if (number.empty())
        return false;

    if (number[0] == '+')
        number.remove_prefix(1);
    // A second '+' would be left over, and from_chars would stop before it.
    if (number.empty() || number[0] == '+')
        return false;

    std::string_view hexProbe = number;
    if (hexProbe[0] == '-')
        hexProbe.remove_prefix(1);
    if (hexProbe.size() >= 2 && hexProbe[0] == '0' && (hexProbe[1] == 'x' || hexProbe[1] == 'X'))
        return false;

    std::size_t consumed = 0;
    const std::optional<double> parsed = ParseDouble(number, &consumed);
    // isfinite rejects the inf and nan that strtod_l would have taken.
    if (!parsed || consumed != number.size() || !std::isfinite(*parsed))
        return false;
    outValue = *parsed;
    return true;
}

// A whole decimal integer: an optional '-' and digits, nothing before or after them (no space, no
// '+', no hex). The whole text is the number, where ParseFloat reads the number at its start.
// Locale-independent and never throws, unlike std::stoi. Nullopt when the text is not such a number
// or it is past int64_t's range.
inline std::optional<int64_t> ParseInt(std::string_view text)
{
    int64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end)
        return std::nullopt;
    return value;
}

// ParseInt above for an unsigned integer: digits only, so "-1" is refused rather than wrapped as
// std::stoull wraps it. Nullopt past uint64_t's range.
inline std::optional<uint64_t> ParseUnsigned(std::string_view text)
{
    uint64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end)
        return std::nullopt;
    return value;
}

} // namespace GameEngine
