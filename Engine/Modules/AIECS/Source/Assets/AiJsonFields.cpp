#include "AiJsonFields.h"

#include "Types/FormatNumber.h"
#include "Types/ParseNumber.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace GameEngine::AIECS
{

namespace
{

constexpr Keywords<AI::CurvePreset, 5> kCurvePresets{{
    {"linear", AI::CurvePreset::Linear},
    {"polynomial", AI::CurvePreset::Polynomial},
    {"logistic", AI::CurvePreset::Logistic},
    {"logit", AI::CurvePreset::Logit},
    {"step", AI::CurvePreset::Step},
}};

} // namespace

bool CheckObject(const Json& value, const std::string& where, std::initializer_list<std::string_view> fields,
                 std::string& outError)
{
    if (!value.is_object())
    {
        outError = where + ": the value is not a JSON object. Write it as { ... } with the fields " +
                   Join(fields, ", ") + ".";
        return false;
    }
    for (const auto& [key, field] : value.items())
    {
        if (std::find(fields.begin(), fields.end(), key) == fields.end())
        {
            outError = where + ": the field '" + key + "' is not in the format. Use one of " + Join(fields, ", ") + ".";
            return false;
        }
    }
    return true;
}

bool ReadFloat(const Json& object, const char* key, const std::string& where, float32& out, std::string& outError)
{
    const auto it = object.find(key);
    if (it == object.end())
        return true;
    // A number reads when its float is finite: the shortest text of FLT_MAX is above FLT_MAX as a double.
    if (!it->is_number() || !std::isfinite(static_cast<float32>(it->get<double>())))
    {
        outError = where + ": '" + key + "' is out of range or is not a number. Write a number such as 1.0.";
        return false;
    }
    out = it->get<float32>();
    return true;
}

bool ReadInteger(const Json& object, const char* key, const std::string& where, uint32 minimum, uint32 maximum,
                 uint32& out, std::string& outError)
{
    const auto it = object.find(key);
    if (it == object.end())
        return true;
    if (!it->is_number_unsigned() || it->get<uint64>() < minimum || it->get<uint64>() > maximum)
    {
        outError = where + ": '" + key + "' is not a whole number from " + std::to_string(minimum) + " to " +
                   std::to_string(maximum) + ".";
        return false;
    }
    out = static_cast<uint32>(it->get<uint64>());
    return true;
}

bool ReadName(const Json& object, const char* key, const std::string& where, std::string& out, std::string& outError)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string() || it->get_ref<const std::string&>().empty())
    {
        outError = where + ": '" + key + "' is missing or is not a name. Write the name as a string.";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

namespace
{

bool ParseCurve(const Json& value, const std::string& where, AiStateTreeSource::Curve& out, std::string& outError)
{
    if (!CheckObject(value, where, {"preset", "shape", "midpoint"}, outError))
        return false;
    return ReadKeyword(value, "preset", where, kCurvePresets, true, out.Preset, outError) &&
           ReadFloat(value, "shape", where, out.Shape, outError) &&
           ReadFloat(value, "midpoint", where, out.Midpoint, outError);
}

} // namespace

OrderedJson WriteFloat(float32 value)
{
    return OrderedJson(ParseDouble(FormatFloat(value)).value_or(value));
}

bool ParseConsideration(const Json& value, const std::string& where, AI::OptionSet& options,
                        std::vector<std::string>& inputNames, std::vector<AiStateTreeSource::Curve>* outCurves,
                        std::string& outError)
{
    // The considerations, the curves and the inputs take 16-bit indices: the limit is one below 0xFFFF.
    // Each consideration adds one curve, so the consideration check also limits the curves.
    constexpr size_t kLimit = std::numeric_limits<uint16>::max() - 1;
    if (options.Considerations.size() >= kLimit)
    {
        outError = where + ": the Utility state has more than " + std::to_string(options.Considerations.size()) +
                   " considerations; the limit is " + std::to_string(kLimit) +
                   ". Split the options into more Utility states.";
        return false;
    }
    if (inputNames.size() >= kLimit)
    {
        outError = where + ": the file has more than " + std::to_string(inputNames.size()) +
                   " input names; the limit is " + std::to_string(kLimit) + ". Split the file into smaller files.";
        return false;
    }
    if (!CheckObject(value, where, {"input", "min", "max", "curve"}, outError))
        return false;
    std::string input;
    if (!ReadName(value, "input", where, input, outError))
        return false;
    AI::OptionSet::Consideration consideration;
    if (!ReadFloat(value, "min", where, consideration.Min, outError) ||
        !ReadFloat(value, "max", where, consideration.Max, outError))
        return false;
    const auto curve = value.find("curve");
    if (curve == value.end())
    {
        outError = where + ": the consideration has no 'curve'. Add { \"preset\": \"linear\" } or another preset.";
        return false;
    }
    AiStateTreeSource::Curve parsedCurve;
    if (!ParseCurve(*curve, where + ".curve", parsedCurve, outError))
        return false;
    const auto column = std::find(inputNames.begin(), inputNames.end(), input);
    consideration.Column = static_cast<uint16>(column - inputNames.begin());
    if (column == inputNames.end())
        inputNames.push_back(std::move(input));
    consideration.Curve = static_cast<uint16>(options.Curves.size());
    options.Curves.push_back(
        AI::ResponseCurve::FromPreset(parsedCurve.Preset, parsedCurve.Shape, parsedCurve.Midpoint));
    options.Considerations.push_back(consideration);
    if (outCurves)
        outCurves->push_back(parsedCurve);
    return true;
}

OrderedJson WriteConsideration(const AI::OptionSet::Consideration& consideration, const AiStateTreeSource::Curve& curve,
                               const std::vector<std::string>& inputNames)
{
    const AiStateTreeSource::Curve defaults;
    OrderedJson written = OrderedJson::object();
    written["input"] = inputNames[consideration.Column];
    if (consideration.Min != AI::OptionSet::Consideration{}.Min)
        written["min"] = WriteFloat(consideration.Min);
    if (consideration.Max != AI::OptionSet::Consideration{}.Max)
        written["max"] = WriteFloat(consideration.Max);
    OrderedJson& writtenCurve = written["curve"];
    writtenCurve["preset"] = KeywordOf(kCurvePresets, curve.Preset);
    if (curve.Shape != defaults.Shape)
        writtenCurve["shape"] = WriteFloat(curve.Shape);
    if (curve.Midpoint != defaults.Midpoint)
        writtenCurve["midpoint"] = WriteFloat(curve.Midpoint);
    return written;
}


} // namespace GameEngine::AIECS
