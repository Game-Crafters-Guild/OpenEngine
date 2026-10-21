#pragma once

#include <string>

namespace GameEngine
{
class GUID;
class TextureCookActivity;
} // namespace GameEngine

namespace GameEngine::Editor
{

/// The inspector header's suffix for a selected texture whose load has not landed: "preparing..."
/// (with an ellipsis character) while its import cook runs, else empty, which leaves the default
/// "Loading <file>..." line. The texture inspector registers it as the texture type's suffix.
std::string TextureLoadingSuffix(const TextureCookActivity& activity, const GUID& texture);

} // namespace GameEngine::Editor
