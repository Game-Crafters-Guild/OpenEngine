#pragma once

#include "ComponentMirror.h"

#include <string>

namespace GameEngine::ComponentScanner
{

// Writes Components.layout.gen.cpp: a static_assert for the size and alignment of every mirrored
// struct, the offset and size of each of its fields, and the underlying type and values of every
// enum a mirror emits. A C++ edit that moves a byte a C# mirror reads then fails the engine build
// with the field's name.
namespace ComponentMirrorLayoutEmitter
{
std::string Emit(const ComponentMirrors& mirrors);
} // namespace ComponentMirrorLayoutEmitter

} // namespace GameEngine::ComponentScanner
