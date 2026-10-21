#include "Inspectors/TextureLoadingSuffix.h"

#include "Assets/TextureCookActivity.h"

namespace GameEngine::Editor
{

std::string TextureLoadingSuffix(const TextureCookActivity& activity, const GUID& texture)
{
    return activity.IsCooking(texture) ? std::string("preparing…") : std::string();
}

} // namespace GameEngine::Editor
