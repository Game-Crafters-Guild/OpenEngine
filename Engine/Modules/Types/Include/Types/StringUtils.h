#pragma once

#include <algorithm>
#include <cctype>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace GameEngine
{

/// A std::string about to be destroyed: a view into it dangles once the full expression ends. Every
/// helper here that returns a view into its argument deletes the overload for this argument, so
/// `Split(ReadFile(path), '\n')` does not compile; name the string first. A std::string_view made
/// from a temporary still compiles and dangles the same way.
template <class Text>
concept TemporaryString = std::same_as<std::remove_cvref_t<Text>, std::string> && !std::is_lvalue_reference_v<Text>;

// The text in a fixed char buffer up to its first null byte, or the whole buffer
// when it holds none. Component string fields are such buffers, and a raw component
// write (GE_ECSABI_SetComponentBytes) can fill every byte, so never read one as a C string.
template <std::size_t Capacity>
[[nodiscard]] std::string_view FixedStringView(const char (&chars)[Capacity])
{
    const void* terminator = std::memchr(chars, '\0', Capacity);
    const std::size_t length =
        terminator ? static_cast<std::size_t>(static_cast<const char*>(terminator) - chars) : Capacity;
    return std::string_view(chars, length);
}

/// `count` followed by the noun that agrees with it: "1 entity", "0 entities", "3 entities".
/// English plurals are irregular, so the caller spells both forms.
inline std::string FormatCount(std::size_t count, std::string_view singular, std::string_view plural)
{
    std::string text = std::to_string(count);
    text += ' ';
    text += count == 1 ? singular : plural;
    return text;
}

/// `value` in decimal with a comma between each group of three digits: "4,194,304". For counts a
/// person reads (samples, bytes), where an ungrouped "4194304" has to be counted by eye.
inline std::string FormatGroupedInteger(unsigned long long value)
{
    const std::string digits = std::to_string(value);
    std::string text;
    text.reserve(digits.size() + digits.size() / 3);
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i != 0 && (digits.size() - i) % 3 == 0)
            text += ',';
        text += digits[i];
    }
    return text;
}

/// `bytes` in mebibytes for a person to read: one decimal below 10 MiB ("4.5 MiB"), whole above
/// ("773 MiB"), where a tenth of a mebibyte no longer tells a reader anything.
inline std::string FormatMebibytes(unsigned long long bytes)
{
    const double mebibytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    char text[32];
    std::snprintf(text, sizeof(text), mebibytes < 10.0 ? "%.1f MiB" : "%.0f MiB", mebibytes);
    return text;
}

inline std::string ToLowerAscii(std::string_view sv)
{
    std::string s(sv);
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// `sv` with the ASCII letters a to z raised to A to Z; every other byte is kept.
inline std::string ToUpperAscii(std::string_view sv)
{
    std::string s(sv);
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

/// The fields of `text` between each `delimiter`, as views into `text`: "a,,b" gives "a", "" and
/// "b", and "a," gives "a" and "". Empty fields are kept; an empty `text` gives no fields. The views
/// live as long as the text they point into.
inline std::vector<std::string_view> Split(std::string_view text, char delimiter)
{
    std::vector<std::string_view> fields;
    if (text.empty())
        return fields;
    std::size_t start = 0;
    for (std::size_t at = text.find(delimiter); at != std::string_view::npos; at = text.find(delimiter, start))
    {
        fields.push_back(text.substr(start, at - start));
        start = at + 1;
    }
    fields.push_back(text.substr(start));
    return fields;
}

template <TemporaryString Text>
std::vector<std::string_view> Split(Text&& text, char delimiter) = delete;

/// The elements of `parts` (anything that converts to std::string_view) with `separator` between
/// each pair: Join({"a", "b"}, ", ") is "a, b". An empty range gives an empty string.
template <std::ranges::input_range Range>
    requires std::convertible_to<std::ranges::range_reference_t<const Range&>, std::string_view>
std::string Join(const Range& parts, std::string_view separator)
{
    std::string text;
    bool first = true;
    for (const auto& part : parts)
    {
        if (!first)
            text += separator;
        text += std::string_view(part);
        first = false;
    }
    return text;
}

/// `text` with every occurrence of `from` replaced by `to`, scanning left to right; a replacement
/// is not scanned again, so ReplaceAll("aa", "a", "aa") is "aaaa". An empty `from` changes nothing.
inline std::string ReplaceAll(std::string text, std::string_view from, std::string_view to)
{
    if (from.empty())
        return text;
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

/// The number of times `needle` occurs in `haystack`, scanning left to right without overlap:
/// CountOccurrences("aaaa", "aa") is 2. An empty `needle` counts 0.
inline std::size_t CountOccurrences(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return 0;
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + needle.size()))
        ++count;
    return count;
}

// Case-insensitive substring check. Avoids allocating lowered copies.
inline bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > haystack.size())
        return false;
    auto toLower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [&](char a, char b) { return toLower(a) == toLower(b); });
    return it != haystack.end();
}

// Case-insensitive prefix check.
inline bool StartsWithIgnoreCase(std::string_view haystack, std::string_view prefix)
{
    if (prefix.size() > haystack.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(haystack[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

// Case-insensitive suffix check.
inline bool EndsWithIgnoreCase(std::string_view haystack, std::string_view suffix)
{
    if (suffix.size() > haystack.size())
        return false;
    const size_t off = haystack.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(haystack[off + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i])))
            return false;
    }
    return true;
}

// Case-insensitive equality.
inline bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && StartsWithIgnoreCase(a, b);
}

/// True when `text` holds an ASCII control character (below 0x20, or 0x7F), a byte no
/// name or path a user authors contains.
inline bool ContainsControlCharacter(std::string_view text)
{
    constexpr unsigned char kFirstPrintable = 0x20;
    constexpr unsigned char kDelete = 0x7F;
    return std::any_of(text.begin(), text.end(),
                       [](char character)
                       {
                           const auto byte = static_cast<unsigned char>(character);
                           return byte < kFirstPrintable || byte == kDelete;
                       });
}

/// True when `text` is well-formed UTF-8: each character in its shortest form, no UTF-16
/// surrogate half (U+D800 to U+DFFF) and nothing past U+10FFFF. Text in a single-byte code page,
/// such as "caf\x82" (code page 437) from an older zip tool, is not.
inline bool IsValidUtf8(std::string_view text)
{
    // The byte ranges are the well-formed sequences of the Unicode Standard's Table 3-7: the lead
    // byte gives the length and the range of the second byte; every later byte is 0x80 to 0xBF.
    std::size_t at = 0;
    while (at < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[at]);
        if (lead < 0x80)
        {
            ++at;
            continue;
        }
        std::size_t length = 0;
        unsigned char secondLow = 0x80;
        unsigned char secondHigh = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF)
            length = 2;
        else if (lead == 0xE0)
        {
            length = 3;
            secondLow = 0xA0;
        }
        else if (lead == 0xED)
        {
            length = 3;
            secondHigh = 0x9F;
        }
        else if (lead >= 0xE1 && lead <= 0xEF)
            length = 3;
        else if (lead == 0xF0)
        {
            length = 4;
            secondLow = 0x90;
        }
        else if (lead == 0xF4)
        {
            length = 4;
            secondHigh = 0x8F;
        }
        else if (lead >= 0xF1 && lead <= 0xF3)
            length = 4;
        else
            return false;
        if (text.size() - at < length)
            return false;
        const auto second = static_cast<unsigned char>(text[at + 1]);
        if (second < secondLow || second > secondHigh)
            return false;
        for (std::size_t next = at + 2; next < at + length; ++next)
        {
            if ((static_cast<unsigned char>(text[next]) & 0xC0) != 0x80)
                return false;
        }
        at += length;
    }
    return true;
}

inline std::string TrimWhitespace(std::string s)
{
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    if (start > 0)
        s.erase(0, start);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

inline std::string_view TrimWhitespaceView(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

template <TemporaryString Text>
std::string_view TrimWhitespaceView(Text&& s) = delete;

inline std::string ToUpperSnakeCase(const std::string& name)
{
    std::string result;
    result.reserve(name.size() + 4);
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if (std::isupper(static_cast<unsigned char>(c)) && i > 0
            && std::islower(static_cast<unsigned char>(name[i - 1])))
        {
            result += '_';
        }
        result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return result;
}

/// The name after its last "::" or "." qualifier: "GameEngine::Components::Light" reads "Light" and
/// "MyGame.Combat.HealthPool" reads "HealthPool". A name with no qualifier is returned as it is.
inline std::string_view UnqualifiedName(std::string_view qualifiedName)
{
    const size_t scope = qualifiedName.rfind("::");
    const size_t dot = qualifiedName.rfind('.');
    if (scope != std::string_view::npos && (dot == std::string_view::npos || scope > dot))
        return qualifiedName.substr(scope + 2);
    if (dot != std::string_view::npos)
        return qualifiedName.substr(dot + 1);
    return qualifiedName;
}

/// A code identifier as words for a label: "castsShadows" and "CastsShadows" read "Casts Shadows",
/// "initial_linear_velocity" reads "Initial Linear Velocity" and "m_SheetColumns" reads "Sheet
/// Columns". Acronym and digit runs stay together ("GPUSkin" reads "GPU Skin", "World3D" reads
/// "World 3D"), an acronym keeps the number that follows it ("MSM4" reads "MSM4") and a dimension
/// stays whole ("Grid5x5" reads "Grid 5x5"). Each word starts with a capital and keeps the case of the
/// rest. Text that already holds a space is returned as it is.
inline std::string IdentifierToWords(std::string_view identifier)
{
    if (identifier.find(' ') != std::string_view::npos)
        return std::string(identifier);
    if (identifier.substr(0, 2) == "m_")
        identifier.remove_prefix(2);
    while (!identifier.empty() && identifier.front() == '_')
        identifier.remove_prefix(1);
    while (!identifier.empty() && identifier.back() == '_')
        identifier.remove_suffix(1);

    std::string words;
    words.reserve(identifier.size() + 8);
    bool wordLedByDigit = false;
    for (size_t i = 0; i < identifier.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(identifier[i]);
        if (c == '_')
        {
            if (!words.empty() && words.back() != ' ')
                words += ' ';
            continue;
        }
        if (!words.empty() && words.back() != ' ' && i > 0)
        {
            const unsigned char previous = static_cast<unsigned char>(identifier[i - 1]);
            const bool nextIsLower =
                i + 1 < identifier.size() && std::islower(static_cast<unsigned char>(identifier[i + 1]));
            const bool afterLower = std::islower(previous) != 0;
            const bool endsAcronym = (std::isupper(previous) || std::isdigit(previous)) && nextIsLower;
            // A number starts a word after a lowercase word, not after an acronym ("MSM4") and not
            // inside a word that began with a number ("5x5").
            const bool startsNumber =
                std::isdigit(c) && !std::isdigit(previous) && !std::isupper(previous) && !wordLedByDigit;
            const bool wordStart = (std::isupper(c) && (afterLower || endsAcronym)) || startsNumber;
            if (wordStart)
                words += ' ';
        }
        const bool firstOfWord = words.empty() || words.back() == ' ';
        if (firstOfWord)
            wordLedByDigit = std::isdigit(c) != 0;
        words += firstOfWord ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
    }
    return words;
}

} // namespace GameEngine
