#include "DebugServer/Components/FieldValueReaders.h"

#include "ECS/Reflection.h"
#include "Scene/RenderLayerMaskText.h"
#include "Types/ParseNumber.h"
#include "Types/StringUtils.h"

#include <optional>

namespace GameEngine::Editor
{
namespace
{
std::string AcceptedForms(std::string_view key)
{
    return std::string(key) +
           " takes a list of layer numbers 0 to 31 in ascending order, as [1, 2] or \"[1, 2]\", or \"all\", or "
           "\"none\"";
}

std::string DeclaredEnumValues(const ECS::FieldInfo& field)
{
    std::string text = std::string(field.Name) + " takes one of ";
    for (std::size_t i = 0; i < field.EnumNames.size(); ++i)
    {
        text += (i ? ", " : "") + std::string(field.EnumNames[i].Name) + "=" +
                std::to_string(field.EnumNames[i].Value);
    }
    return text;
}

// ReflectedEnumNameDrift keeps the case-insensitive match unique: no reflected enum has two
// enumerators that differ only by case.
std::optional<std::int64_t> FindDeclaredEnumValue(const ECS::FieldInfo& field, const nlohmann::json& value)
{
    std::optional<std::int64_t> number;
    if (value.is_number_integer())
    {
        number = value.get<std::int64_t>();
    }
    else if (value.is_string())
    {
        const std::string& text = value.get_ref<const std::string&>();
        for (const ECS::EnumNameValue& enumerator : field.EnumNames)
        {
            if (EqualsIgnoreCase(enumerator.Name, text))
                return enumerator.Value;
        }
        number = ParseInt(text);
    }
    if (!number)
        return std::nullopt;
    for (const ECS::EnumNameValue& enumerator : field.EnumNames)
    {
        if (enumerator.Value == *number)
            return number;
    }
    return std::nullopt;
}
} // namespace

bool ReadRenderLayerMaskValue(const nlohmann::json& value, std::string_view key, Rendering::RenderLayerMask& out,
                              std::string& outError)
{
    std::string text;
    if (value.is_string())
    {
        text = value.get<std::string>();
    }
    else if (value.is_array())
    {
        // The JSON list goes through the scene text parser, so both forms obey one rule.
        text = "[";
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (!value[i].is_number_integer())
            {
                outError = AcceptedForms(key) + "; element " + std::to_string(i) + " is not a layer number";
                return false;
            }
            text += (i ? ", " : "") + std::to_string(value[i].get<long long>());
        }
        text += "]";
    }
    else
    {
        outError = AcceptedForms(key) + "; a " + std::string(value.type_name()) + " is not one of them";
        return false;
    }

    std::string parseError;
    if (!Scene::ParseRenderLayerMask(text, key, out, &parseError))
    {
        outError = AcceptedForms(key) + ", not '" + text + "'";
        return false;
    }
    return true;
}

bool ReadEnumFieldValue(const nlohmann::json& value, const ECS::FieldInfo& field, std::int64_t& out,
                        std::string& outError)
{
    if (const std::optional<std::int64_t> declared = FindDeclaredEnumValue(field, value))
    {
        out = *declared;
        return true;
    }
    outError = DeclaredEnumValues(field) + "; " + value.dump() + " is not one of them";
    return false;
}

} // namespace GameEngine::Editor
