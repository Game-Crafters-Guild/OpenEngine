#pragma once

#include "Rendering/RenderLayerMask.h"

#include <string>
#include <string_view>

namespace GameEngine::Scene
{

/// The scene text of a render layer mask: the layer numbers in brackets, sorted ("[0]", "[1, 2]"), or the
/// words `all` and `none`. Every scene field of the type writes this, through the hand-written schemas and
/// the reflected field serializer alike.
std::string FormatRenderLayerMask(Rendering::RenderLayerMask mask);

/// Read the text `FormatRenderLayerMask` writes. Layer numbers are 0 to 31, ascending, each once. Anything
/// else is refused and `out` is left alone; the message names `key` (the field's scene key) and the
/// accepted forms.
bool ParseRenderLayerMask(std::string_view text, std::string_view key, Rendering::RenderLayerMask& out,
                          std::string* outError);

} // namespace GameEngine::Scene
