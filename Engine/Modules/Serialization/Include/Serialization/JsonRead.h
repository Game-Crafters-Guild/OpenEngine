#pragma once

#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

// Reading authored JSON without throwing. A decode reports a file it cannot read by refusing it,
// with the reason, and never by throwing: the web build links without exception catching, so a
// throw there aborts the module. These read what nlohmann::json's parse() and value() read and
// refuse exactly what those throw on.

namespace GameEngine::Serialization
{
/// The most bytes of authored text a refusal quotes; longer text is cut there and its length given.
inline constexpr size_t kQuotedTextLimit = 64u;

/// `text` as a JSON string literal: quoted and escaped, on one line.
inline std::string JsonStringLiteral(std::string_view text)
{
    return nlohmann::json(std::string(text)).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

/// Authored text as a refusal quotes it: a JSON string literal of at most kQuotedTextLimit bytes of
/// it, followed by its length when it is longer. A refusal never carries an unbounded or unescaped
/// excerpt of a file: a line break in it would split the log line.
inline std::string Quoted(std::string_view text)
{
    if (text.size() <= kQuotedTextLimit)
        return JsonStringLiteral(text);
    return std::format("{}... ({} bytes)", JsonStringLiteral(text.substr(0, kQuotedTextLimit)), text.size());
}

/// Parses `text` as JSON without throwing. Returns false when it is not JSON, with `outError`
/// naming where and why ("parse error at line 3, column 7: syntax error ..."), the text it last
/// read quoted as Quoted() quotes it.
inline bool ParseJson(std::string_view text, nlohmann::json& outDocument, std::string& outError)
{
    outDocument = nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (!outDocument.is_discarded())
        return true;

    // The DOM parse keeps no reason; a second pass through a SAX recorder reads it.
    class ParseErrorRecorder final : public nlohmann::json_sax<nlohmann::json>
    {
      public:
        bool null() override { return true; }
        bool boolean(bool) override { return true; }
        bool number_integer(number_integer_t) override { return true; }
        bool number_unsigned(number_unsigned_t) override { return true; }
        bool number_float(number_float_t, const string_t&) override { return true; }
        bool string(string_t&) override { return true; }
        bool binary(binary_t&) override { return true; }
        bool start_object(std::size_t) override { return true; }
        bool key(string_t&) override { return true; }
        bool end_object() override { return true; }
        bool start_array(std::size_t) override { return true; }
        bool end_array() override { return true; }
        bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception& error) override
        {
            // what() reads "[json.exception.parse_error.101] parse error at line ...; last read:
            // '<text>'"; the bracketed id means nothing to an author, and the text it last read runs
            // to the end of the file for an unterminated string.
            std::string_view message = error.what();
            const size_t idEnd = message.find("] ");
            if (idEnd != std::string_view::npos)
                message.remove_prefix(idEnd + 2);
            constexpr std::string_view kLastRead = "; last read: '";
            const size_t lastRead = message.find(kLastRead);
            if (lastRead == std::string_view::npos || !message.ends_with('\''))
            {
                Error = std::string(message);
                return false;
            }
            const std::string_view token =
                message.substr(lastRead + kLastRead.size(), message.size() - lastRead - kLastRead.size() - 1);
            Error = std::format("{}; last read: {}", message.substr(0, lastRead), Quoted(token));
            return false;
        }
        std::string Error;
    };
    ParseErrorRecorder recorder;
    nlohmann::json::sax_parse(text.begin(), text.end(), &recorder);
    outError = recorder.Error.empty() ? std::string("the text is not JSON") : std::move(recorder.Error);
    outDocument = nlohmann::json();
    return false;
}

/// The first key written twice in one object of `text`, or empty. JSON leaves a repeated key to the
/// reader, which keeps the last; a format that refuses it loses no definition unseen.
inline std::string FirstRepeatedKey(std::string_view text)
{
    std::vector<std::vector<std::string>> objects;
    std::string repeated;
    const auto callback = [&](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
        if (event == nlohmann::json::parse_event_t::object_start)
            objects.emplace_back();
        else if (event == nlohmann::json::parse_event_t::object_end && !objects.empty())
            objects.pop_back();
        else if (event == nlohmann::json::parse_event_t::key && !objects.empty() && repeated.empty())
        {
            const std::string key = parsed.get<std::string>();
            std::vector<std::string>& seen = objects.back();
            if (std::find(seen.begin(), seen.end(), key) != seen.end())
                repeated = key;
            seen.push_back(key);
        }
        return true;
    };
    // Only the callback is wanted. GCC does not let a void cast drop a warn_unused_result value.
    std::ignore = nlohmann::json::parse(text, callback, false);
    return repeated;
}

/// Whether a reader refuses an absent field (Required) or gives the caller's fallback (Optional).
/// Use Required for a field that has no correct default, such as a position in a save file.
enum class FieldPresence : uint8_t
{
    Optional,
    Required,
};

/**
 * @brief Reads the fields of JSON objects into C++ values without throwing.
 *
 * A value of a type the field cannot hold is refused: the field keeps its value, and the first
 * refusal is kept for the caller to report ("'globals.maxDistance' is a string; write it as a
 * number"). A field takes what nlohmann::json's value() takes: a std::string takes a string,
 * a bool takes true or false, and an arithmetic field takes a number or true or false, converted
 * as value() converts it.
 */
class JsonFieldReader
{
  public:
    /// Field names in refusals start with `context` ("elements[2].").
    void SetContext(std::string context) { m_Context = std::move(context); }

    /// Reads field `key` of `object` into `out` when it is present. `object` itself is refused
    /// when it is not a JSON object.
    template <typename T>
    void Read(const nlohmann::json& object, std::string_view key, T& out)
    {
        if (!object.is_object())
        {
            Refuse(std::format("{} is {}; write it as an object", ContextName(), DescribeJsonValue(object)));
            return;
        }
        const auto field = object.find(key);
        if (field != object.end())
            ReadValue(*field, key, out);
    }

    /// Reads `value` into `out`; a refusal names it `name` after the context.
    template <typename T>
    void ReadValue(const nlohmann::json& value, std::string_view name, T& out)
    {
        if constexpr (std::is_same_v<T, std::string>)
        {
            if (value.is_string())
                out = value.get_ref<const std::string&>();
            else
                RefuseValue(value, name, "a string");
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            if (value.is_boolean())
                out = value.get<bool>();
            else
                RefuseValue(value, name, "true or false");
        }
        else
        {
            static_assert(std::is_arithmetic_v<T>, "JsonFieldReader reads strings, booleans and numbers");
            if (value.is_number() || value.is_boolean())
                out = value.get<T>();
            else
                RefuseValue(value, name, "a number");
        }
    }

    /// Field `key` of `object` as an array of at least three numbers (x, y, z). Gives `fallback` when
    /// `object` is not an object or the field is absent (refused when `presence` is Required), and
    /// refuses a field of another shape; a non-number element is refused and keeps the fallback value.
    Mathematics::Vector3 ReadVector3(const nlohmann::json& object, std::string_view key,
                                     const Mathematics::Vector3& fallback,
                                     FieldPresence presence = FieldPresence::Optional)
    {
        const nlohmann::json* array = ArrayField(object, key, 3, "[x, y, z]", presence);
        if (array == nullptr)
            return fallback;
        float values[3] = {fallback.x, fallback.y, fallback.z};
        for (size_t i = 0; i < 3; ++i)
            ReadValue((*array)[i], std::format("{}[{}]", key, i), values[i]);
        return Mathematics::Vector3(values[0], values[1], values[2]);
    }

    /// Field `key` of `object` as an array of at least four numbers, written w, x, y, z. Gives
    /// `fallback` when `object` is not an object, the field is absent (refused when `presence` is
    /// Required), or the reader has refused anything. A field of another shape is refused.
    Mathematics::Quaternion ReadQuaternion(const nlohmann::json& object, std::string_view key,
                                           const Mathematics::Quaternion& fallback,
                                           FieldPresence presence = FieldPresence::Optional)
    {
        const nlohmann::json* array = ArrayField(object, key, 4, "[w, x, y, z]", presence);
        if (array == nullptr)
            return fallback;
        float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (size_t i = 0; i < 4; ++i)
            ReadValue((*array)[i], std::format("{}[{}]", key, i), values[i]);
        if (Refused())
            return fallback;
        return Mathematics::Quaternion(values[0], values[1], values[2], values[3]);
    }

    bool Refused() const { return !m_Refusal.empty(); }
    const std::string& Refusal() const { return m_Refusal; }

  private:
    /// Field `key` of `object` when it is an array of at least `minimumSize` elements; null when
    /// `object` is not an object or the field is absent. A field of another shape, or an absent
    /// Required one, is refused, with `expected` as the form to write, and gives null.
    const nlohmann::json* ArrayField(const nlohmann::json& object, std::string_view key, size_t minimumSize,
                                     std::string_view expected, FieldPresence presence)
    {
        if (!object.is_object())
            return nullptr;
        const auto field = object.find(key);
        if (field == object.end())
        {
            if (presence == FieldPresence::Required)
                Refuse(std::format("{} is missing; write it as {}", QualifiedName(key), expected));
            return nullptr;
        }
        if (!field->is_array())
        {
            RefuseValue(*field, key, expected);
            return nullptr;
        }
        if (field->size() < minimumSize)
        {
            Refuse(std::format("{} is an array of {} {}; write it as {}", QualifiedName(key), field->size(),
                               field->size() == 1 ? "element" : "elements", expected));
            return nullptr;
        }
        return &*field;
    }

    /// Field `name` after the context, quoted as a refusal quotes it.
    std::string QualifiedName(std::string_view name) const { return FieldName(m_Context + std::string(name)); }

    /// How a refusal names the JSON value an author wrote: "a string", "an array", "true or false".
    static std::string_view DescribeJsonValue(const nlohmann::json& value)
    {
        switch (value.type())
        {
        case nlohmann::json::value_t::null: return "null";
        case nlohmann::json::value_t::object: return "an object";
        case nlohmann::json::value_t::array: return "an array";
        case nlohmann::json::value_t::string: return "a string";
        case nlohmann::json::value_t::boolean: return "true or false";
        case nlohmann::json::value_t::binary: return "binary data";
        case nlohmann::json::value_t::discarded: return "not a value";
        default: return "a number";
        }
    }

    /// The field the context names, quoted; the file's JSON itself when the context is empty.
    std::string ContextName() const
    {
        std::string_view name = m_Context;
        if (name.ends_with('.'))
            name.remove_suffix(1);
        return name.empty() ? std::string("the file's JSON") : FieldName(name);
    }

    /// A field's name as a refusal quotes it: in single quotes, its authored keys escaped and its
    /// length bounded as Quoted() bounds them.
    static std::string FieldName(std::string_view name)
    {
        const std::string literal = Quoted(name);
        const size_t closing = literal.rfind('"');
        return "'" + literal.substr(1, closing - 1) + "'" + literal.substr(closing + 1);
    }

    /// Keeps `refusal` unless one was kept before.
    void Refuse(std::string refusal)
    {
        if (m_Refusal.empty())
            m_Refusal = std::move(refusal);
    }

    void RefuseValue(const nlohmann::json& value, std::string_view name, std::string_view expected)
    {
        Refuse(std::format("{} is {}; write it as {}", QualifiedName(name), DescribeJsonValue(value), expected));
    }

    std::string m_Context;
    std::string m_Refusal;
};
} // namespace GameEngine::Serialization
