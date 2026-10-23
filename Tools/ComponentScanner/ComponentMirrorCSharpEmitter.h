#pragma once

#include "ComponentMirror.h"

#include <map>
#include <string>

namespace GameEngine::ComponentScanner
{

// Writes the C# component mirrors (ComponentMirror.h has the rules): one file per header, by its
// MirrorCSharpFile name, holding the header's mirrored structs and the enums it declares that a
// mirror uses. A component is `[StructLayout(LayoutKind.Explicit, Size = ...)]`,
// `[BuiltInComponent("<C++ name>")]`, `partial`, IComponent, with a `[FieldOffset]` per field and
// a parameterless constructor that copies ComponentType<T>.Default.
namespace ComponentMirrorCSharpEmitter
{
// The files by their path relative to Generated/Components ("Rendering/MeshRenderer.g.cs").
std::map<std::string, std::string> Emit(const ComponentMirrors& mirrors);
} // namespace ComponentMirrorCSharpEmitter

} // namespace GameEngine::ComponentScanner
