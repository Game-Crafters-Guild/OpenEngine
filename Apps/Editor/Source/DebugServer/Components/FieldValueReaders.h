#pragma once

#include "Rendering/RenderLayerMask.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace GameEngine::ECS
{
struct FieldInfo;
}

namespace GameEngine::Editor
{

// A render layer field's value in a debug-server write (set_component): a list of layer numbers, as a JSON
// array ([1, 2]) or as the scene text ("[1, 2]"), or the words "all" and "none". The list is in ascending
// order, each layer once, 0 to 31. A refused value leaves `out` alone and puts the accepted forms, named by
// `key`, in `outError`.
bool ReadRenderLayerMaskValue(const nlohmann::json& value, std::string_view key, Rendering::RenderLayerMask& out,
                              std::string& outError);

// A reflected enum field's value in set_component: an enumerator name in any case, or a declared
// enumerator value as a JSON integer or a numeric string. A refusal leaves `out` alone and puts the
// field, the declared names and values and the value given in `outError`.
bool ReadEnumFieldValue(const nlohmann::json& value, const ECS::FieldInfo& field, std::int64_t& out,
                        std::string& outError);

} // namespace GameEngine::Editor
