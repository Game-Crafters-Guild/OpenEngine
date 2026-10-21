#include "NumberFormat.h"

#include "Lexical.h"

#include <Types/ParseNumber.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <type_traits>

namespace GameEngine::ComponentScanner::NumberFormat
{

using Lexical::IsAsciiDigit;
using Lexical::Trim;
using Lexical::TrimView;
namespace
{

// Room for any double in std::chars_format::scientific: sign, 17 digits, point, exponent.
constexpr std::size_t kScientificBufferSize = 64;
// The decimal-point positions ShortestRoundTrip writes in fixed notation, as
// value = 0.d1d2... x 10^scale: up to the greater of the digit count and the 17 significant
// digits a double carries, and down to 0.0001 (scale -3); outside, the "d.dddE+XX" form.
constexpr int kMaxFixedScaleDigits = 17;
constexpr int kMinFixedScale = -3;

// Converts a literal whose grammar the caller has checked; the conversion must consume all of it.
template <typename Number>
LiteralStatus ParseCanonicalNumber(const std::string& text, Number& value)
{
    std::size_t consumed = 0;
    ParseNumberStatus status = ParseNumberStatus::NoNumber;
    if constexpr (std::is_same_v<Number, float>)
        status = GameEngine::ParseFloat(text, value, consumed);
    else
        status = GameEngine::ParseDouble(text, value, consumed);
    if (status == ParseNumberStatus::OutOfRange || (status == ParseNumberStatus::Ok && !std::isfinite(value)))
        return LiteralStatus::OutOfRange;
    if (status != ParseNumberStatus::Ok || consumed != text.size())
        return LiteralStatus::NotALiteral;
    return LiteralStatus::Ok;
}

bool IsIntegerLiteral(std::string_view text)
{
    std::string_view t = TrimView(text);
    if (!t.empty() && t[0] == '-')
        t = TrimView(t.substr(1));
    while (!t.empty() && (t.back() == 'u' || t.back() == 'U' || t.back() == 'l' || t.back() == 'L'))
        t.remove_suffix(1);
    if (t.empty())
        return false;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
        return std::all_of(t.begin() + 2, t.end(), [](char c)
                           { return IsAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
    return std::all_of(t.begin(), t.end(), IsAsciiDigit);
}

bool IsFloatLiteral(std::string_view text)
{
    std::string_view t = TrimView(text);
    if (!t.empty() && t[0] == '-')
        t = TrimView(t.substr(1));
    if (!t.empty() && (t.back() == 'f' || t.back() == 'F'))
        t.remove_suffix(1);
    if (t.empty())
        return false;
    bool digits = false;
    bool point = false;
    bool exponent = false;
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        const char c = t[i];
        if (IsAsciiDigit(c))
            digits = true;
        else if (c == '.' && !point && !exponent)
            point = true;
        else if ((c == 'e' || c == 'E') && digits && !exponent)
        {
            exponent = true;
            if (i + 1 < t.size() && (t[i + 1] == '+' || t[i + 1] == '-'))
                ++i;
            if (i + 1 >= t.size())
                return false;
        }
        else
            return false;
    }
    return digits;
}

} // namespace

std::optional<double> ParseDouble(std::string_view text)
{
    std::string_view s = Lexical::TrimView(text);
    if (s.empty())
        return std::nullopt;

    bool negative = false;
    if (s[0] == '+' || s[0] == '-')
    {
        negative = s[0] == '-';
        s = s.substr(1);
    }
    // digits [ '.' digits ] [ ('e' | 'E') [sign] digits ], at least one mantissa digit: the
    // grammar is checked here so strtod never sees the hex, infinity and nan spellings it
    // would accept.
    std::size_t i = 0;
    std::size_t mantissaDigits = 0;
    while (i < s.size() && Lexical::IsAsciiDigit(s[i]))
    {
        ++i;
        ++mantissaDigits;
    }
    if (i < s.size() && s[i] == '.')
    {
        ++i;
        while (i < s.size() && Lexical::IsAsciiDigit(s[i]))
        {
            ++i;
            ++mantissaDigits;
        }
    }
    if (mantissaDigits == 0)
        return std::nullopt;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
    {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-'))
            ++i;
        std::size_t exponentDigits = 0;
        while (i < s.size() && Lexical::IsAsciiDigit(s[i]))
        {
            ++i;
            ++exponentDigits;
        }
        if (exponentDigits == 0)
            return std::nullopt;
    }
    if (i != s.size())
        return std::nullopt;

    const std::string number(s);
    const double value = std::strtod(number.c_str(), nullptr);
    return negative ? -value : value;
}

std::string ShortestRoundTrip(double value)
{
    if (value == 0.0)
        return std::signbit(value) ? "-0" : "0";

    // Shortest round-trip digits in scientific form: "[-]d[.ddd]e[+-]XX".
    char buffer[kScientificBufferSize];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
    if (ec != std::errc{})
        return "";
    std::string_view scientific(buffer, static_cast<std::size_t>(end - buffer));

    const bool negative = scientific[0] == '-';
    if (negative)
        scientific = scientific.substr(1);
    const std::size_t exponentMark = scientific.find('e');
    const std::string_view mantissa = scientific.substr(0, exponentMark);
    std::string_view exponentText = scientific.substr(exponentMark + 1);
    const bool exponentNegative = exponentText[0] == '-';
    exponentText = exponentText.substr(1);
    int exponent = 0;
    std::from_chars(exponentText.data(), exponentText.data() + exponentText.size(), exponent);
    if (exponentNegative)
        exponent = -exponent;

    std::string digits;
    for (char c : mantissa)
        if (c != '.')
            digits.push_back(c);

    // Position of the decimal point relative to the first digit: value = 0.d1d2... x 10^scale.
    const int scale = exponent + 1;
    const int digitCount = static_cast<int>(digits.size());
    const bool fixed = scale <= std::max(digitCount, kMaxFixedScaleDigits) && scale >= kMinFixedScale;

    std::string text = negative ? "-" : "";
    if (fixed)
    {
        if (scale <= 0)
        {
            text += "0.";
            text.append(static_cast<std::size_t>(-scale), '0');
            text += digits;
        }
        else if (scale >= digitCount)
        {
            text += digits;
            text.append(static_cast<std::size_t>(scale - digitCount), '0');
        }
        else
        {
            text += digits.substr(0, static_cast<std::size_t>(scale));
            text += '.';
            text += digits.substr(static_cast<std::size_t>(scale));
        }
        return text;
    }

    text += digits[0];
    if (digitCount > 1)
    {
        text += '.';
        text += digits.substr(1);
    }
    text += 'E';
    text += exponent < 0 ? '-' : '+';
    const std::string exponentDigits = std::to_string(std::abs(exponent));
    if (exponentDigits.size() < 2)
        text += '0';
    text += exponentDigits;
    return text;
}

CanonicalNumber CanonicalInteger(std::string_view literal)
{
    if (!IsIntegerLiteral(literal))
        return {};
    std::string_view t = TrimView(literal);
    const bool negative = !t.empty() && t[0] == '-';
    if (negative)
        t = TrimView(t.substr(1));
    while (!t.empty() && (t.back() == 'u' || t.back() == 'U' || t.back() == 'l' || t.back() == 'L'))
        t.remove_suffix(1);
    const bool hex = t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X');
    const std::string_view digits = hex ? t.substr(2) : t;
    unsigned long long value = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10);
    if (ec == std::errc::result_out_of_range)
        return {LiteralStatus::OutOfRange, {}};
    if (ec != std::errc{} || ptr != digits.data() + digits.size())
        return {};
    return {LiteralStatus::Ok, (negative && value != 0 ? "-" : "") + std::to_string(value)};
}

CanonicalNumber CanonicalFloat(std::string_view literal)
{
    std::string t;
    if (const CanonicalNumber integer = CanonicalInteger(literal); integer.Status != LiteralStatus::NotALiteral)
    {
        if (integer.Status == LiteralStatus::OutOfRange)
            return integer;
        t = integer.Text;
    }
    else if (IsFloatLiteral(literal))
    {
        t = Trim(literal);
        if (t.back() == 'f' || t.back() == 'F')
            t.pop_back();
    }
    else
        return {};
    float value = 0.0f;
    const LiteralStatus status = ParseCanonicalNumber(t, value);
    if (status != LiteralStatus::Ok)
        return {status, {}};
    char buffer[32];
    const auto [end, writeError] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (writeError != std::errc{})
        return {};
    return {LiteralStatus::Ok, std::string(buffer, end)};
}

CanonicalNumber CanonicalDouble(std::string_view literal)
{
    std::string t = Trim(literal);
    if (const CanonicalNumber integer = CanonicalInteger(t); integer.Status != LiteralStatus::NotALiteral)
    {
        if (integer.Status == LiteralStatus::OutOfRange)
            return integer;
        t = integer.Text;
    }
    else if (!IsFloatLiteral(t) || t.back() == 'f' || t.back() == 'F')
        return {};
    double value = 0.0;
    const LiteralStatus status = ParseCanonicalNumber(t, value);
    if (status != LiteralStatus::Ok)
        return {status, {}};
    return {LiteralStatus::Ok, ShortestRoundTrip(value)};
}

} // namespace GameEngine::ComponentScanner::NumberFormat
