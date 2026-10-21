#include "DebugServer/RenderLayerMaskValue.h"

#include "Scene/RenderLayerMaskText.h"

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

} // namespace GameEngine::Editor
