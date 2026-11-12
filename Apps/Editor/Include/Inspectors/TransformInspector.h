#pragma once

#include <string>

namespace GameEngine
{

namespace Mathematics
{
struct Vector3;
}

// Registers the default Transform component inspector with InspectorRegistry.
void RegisterTransformInspector();

// The text a Transform row's Copy puts on the clipboard: X, Y and Z with six significant digits, joined by ", ".
std::string TransformRowClipboardText(const Mathematics::Vector3& value);

// The row's Paste reads three numbers separated by commas, semicolons or spaces. Returns false and keeps `out` when
// the text does not start with three numbers.
bool TryParseTransformRowClipboardText(const std::string& text, Mathematics::Vector3& out);

} // namespace GameEngine
