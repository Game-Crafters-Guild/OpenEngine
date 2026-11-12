#pragma once

#include "AI/OptionSet.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeSource.h"
#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The JSON field readers of the AI data files. Each one writes the field, the rule and the fix to
// `outError` when it refuses; `where` names the object for that text.
namespace GameEngine::AIECS
{

using Json = nlohmann::json;
// The writers keep the order of the fields as they add them.
using OrderedJson = nlohmann::ordered_json;

// Refuses a value that is not an object, or an object with a key outside `fields`.
bool CheckObject(const Json& value, const std::string& where, std::initializer_list<std::string_view> fields,
                 std::string& outError);

// Reads an optional number that a float32 holds as a finite value; `out` keeps its value when the
// field is absent.
bool ReadFloat(const Json& object, const char* key, const std::string& where, float32& out, std::string& outError);

// Reads an optional integer in [minimum, maximum]; `out` keeps its value when the field is absent.
bool ReadInteger(const Json& object, const char* key, const std::string& where, uint32 minimum, uint32 maximum,
                 uint32& out, std::string& outError);

// Reads a required non-empty string.
bool ReadName(const Json& object, const char* key, const std::string& where, std::string& out, std::string& outError);

// A keyword table: each keyword of a field and the value it reads as.
template <typename T, size_t Count>
using Keywords = std::array<std::pair<std::string_view, T>, Count>;

// Reads the keyword field `key` into `out` from `keywords`; `out` keeps its value when the field is absent
// and `required` is false. The refusal names the written value and the keywords.
template <typename T, size_t Count>
bool ReadKeyword(const Json& object, const char* key, const std::string& where, const Keywords<T, Count>& keywords,
                 bool required, T& out, std::string& outError)
{
    const auto it = object.find(key);
    if (it == object.end() && !required)
        return true;
    const auto match =
        it != object.end() && it->is_string()
            ? std::find_if(keywords.begin(), keywords.end(),
                           [&it](const auto& entry) { return entry.first == it->get_ref<const std::string&>(); })
            : keywords.end();
    if (match == keywords.end())
    {
        std::vector<std::string_view> names;
        for (const auto& [name, value] : keywords)
            names.push_back(name);
        const std::string written = it != object.end() && it->is_string()
                                        ? "'" + it->get<std::string>() + "' is not a keyword"
                                        : "is missing or is not a keyword";
        outError = where + ": '" + key + "' " + written + ". Use one of " + Join(names, ", ") + ".";
        return false;
    }
    out = match->second;
    return true;
}

// The keyword of `value` in `keywords`.
template <typename T, size_t Count>
std::string_view KeywordOf(const Keywords<T, Count>& keywords, T value)
{
    const auto match = std::find_if(keywords.begin(), keywords.end(),
                                    [value](const auto& entry) { return entry.second == value; });
    return match != keywords.end() ? match->first : std::string_view{};
}

// A JSON number that prints as the shortest text that ReadFloat reads back to `value`. `value` is finite.
OrderedJson WriteFloat(float32 value);

// Reads one consideration into `options` with its curve; its input becomes a column of `inputNames`. When
// `outCurves` is not null, it gets the curve as the file writes it, at the index of the consideration.
bool ParseConsideration(const Json& value, const std::string& where, AI::OptionSet& options,
                        std::vector<std::string>& inputNames, std::vector<AiStateTreeSource::Curve>* outCurves,
                        std::string& outError);

// Writes one consideration with its curve as ParseConsideration reads it.
OrderedJson WriteConsideration(const AI::OptionSet::Consideration& consideration, const AiStateTreeSource::Curve& curve,
                               const std::vector<std::string>& inputNames);

} // namespace GameEngine::AIECS
